#include "JamTabaVST3.h"
#include "JamTabaVST3View.h"
#include "MainControllerVST3.h"
#include "NinjamControllerPlugin.h"
#include "MainWindowPlugin.h"
#include "QtPluginApp.h"
#include "log/Logging.h"

#include "pluginterfaces/base/ustring.h"
#include "pluginterfaces/vst/ivsthostapplication.h"
#include "pluginterfaces/vst/ivstprocesscontext.h"
#include "pluginterfaces/vst/vstspeaker.h"

#include <QLabel>
#include <cmath>
#include <cstring>

using namespace Steinberg;
using namespace Steinberg::Vst;

const FUID JamTabaVST3::cid(0xCF6BD7B5, 0x51B04CDD, 0x8AAD136F, 0x85C7F82B);

static const int INPUT_CHANNELS = 4; // two stereo inputs, one per local channel
static const int OUTPUT_CHANNELS = 2;

FUnknown *JamTabaVST3::createInstance(void *)
{
    return static_cast<IAudioProcessor *>(new JamTabaVST3());
}

JamTabaVST3::JamTabaVST3() :
    JamTabaPlugin(INPUT_CHANNELS, OUTPUT_CHANNELS),
    sampleRate(44100.0f),
    hostTempo(0.0),
    hostPlaying(false),
    controllerReady(false),
    mainWindow(nullptr),
    duplicateInstanceWidget(nullptr),
    currentView(nullptr),
    linuxRunLoop(nullptr)
{
    std::memset(&processContext, 0, sizeof(processContext));
    processContextRequirements.needTempo()
        .needTimeSignature()
        .needProjectTimeMusic()
        .needBarPositionMusic()
        .needTransportState();
}

JamTabaVST3::~JamTabaVST3()
{
}

tresult PLUGIN_API JamTabaVST3::initialize(FUnknown *context)
{
    tresult result = SingleComponentEffect::initialize(context);
    if (result != kResultOk)
        return result;

    addAudioInput(STR16("Input"), SpeakerArr::kStereo);
    addAudioInput(STR16("Input 2"), SpeakerArr::kStereo, kAux, 0); // second local channel, off by default
    addAudioOutput(STR16("Output"), SpeakerArr::kStereo);

    FUnknownPtr<IHostApplication> hostApplication(context);
    if (hostApplication) {
        String128 name = {0};
        if (hostApplication->getName(name) == kResultOk)
            hostName = QString::fromUtf16(reinterpret_cast<const char16_t *>(name));
    }

    return kResultOk;
}

tresult PLUGIN_API JamTabaVST3::terminate()
{
    controllerReady = false;

    if (mainWindow)
        mainWindow->closeAllFloatingWindows();

    close();
    controller.reset(); // saves the input settings from the main window, leaves the room, stops JamTaba's threads
    deleteMainWindow();

    if (duplicateInstanceWidget) {
        delete duplicateInstanceWidget;
        duplicateInstanceWidget = nullptr;
    }

    if (linuxRunLoop) {
        qtPluginApp::stopLinuxEventPump(linuxRunLoop);
        linuxRunLoop = nullptr;
    }

    return SingleComponentEffect::terminate();
}

void JamTabaVST3::close()
{
    if (isRunning()) // a duplicate instance must not clear the flag of the running one
        JamTabaPlugin::close();
}

bool JamTabaVST3::isDuplicateInstance() const
{
    return !isRunning() && JamTabaPlugin::pluginIsInitialized();
}

tresult PLUGIN_API JamTabaVST3::setActive(TBool state)
{
    if (state)
        hostWasPlayingInLastAudioCallBack = false;
    return SingleComponentEffect::setActive(state);
}

tresult PLUGIN_API JamTabaVST3::setState(IBStream *)
{
    return kResultOk; // JamTaba keeps its settings in its own files, not in the DAW project
}

tresult PLUGIN_API JamTabaVST3::getState(IBStream *)
{
    return kResultOk;
}

tresult PLUGIN_API JamTabaVST3::setBusArrangements(SpeakerArrangement *inputs, int32 numIns,
                                                    SpeakerArrangement *outputs, int32 numOuts)
{
    if (numIns < 1 || numIns > 2 || numOuts != 1 || outputs[0] != SpeakerArr::kStereo)
        return kResultFalse;

    for (int32 i = 0; i < numIns; ++i) {
        if (inputs[i] != SpeakerArr::kStereo)
            return kResultFalse;
    }

    return SingleComponentEffect::setBusArrangements(inputs, numIns, outputs, numOuts);
}

tresult PLUGIN_API JamTabaVST3::canProcessSampleSize(int32 symbolicSampleSize)
{
    return symbolicSampleSize == kSample32 ? kResultTrue : kResultFalse;
}

tresult PLUGIN_API JamTabaVST3::setupProcessing(ProcessSetup &setup)
{
    setSampleRate(static_cast<float>(setup.sampleRate));

    // grow the buffers now so process() never allocates
    inputBuffer.setFrameLenght(setup.maxSamplesPerBlock);
    outputBuffer.setFrameLenght(setup.maxSamplesPerBlock);

    return SingleComponentEffect::setupProcessing(setup);
}

uint32 PLUGIN_API JamTabaVST3::getTailSamples()
{
    return kInfiniteTail; // remote players keep sounding when our input is silent
}

void JamTabaVST3::passThrough(ProcessData &data)
{
    AudioBusBuffers &out = data.outputs[0];
    const AudioBusBuffers *in = data.numInputs > 0 ? &data.inputs[0] : nullptr;
    for (int32 c = 0; c < out.numChannels; ++c) {
        float *dest = out.channelBuffers32[c];
        if (in && c < in->numChannels && in->channelBuffers32[c]) {
            if (dest != in->channelBuffers32[c])
                std::memcpy(dest, in->channelBuffers32[c], sizeof(float) * data.numSamples);
        } else {
            std::memset(dest, 0, sizeof(float) * data.numSamples);
        }
    }
    out.silenceFlags = in ? in->silenceFlags : ~static_cast<uint64>(0);
}

tresult PLUGIN_API JamTabaVST3::process(ProcessData &data)
{
    if (data.processContext)
        processContext = *data.processContext;
    else
        processContext.state = 0;

    const bool playing = (processContext.state & ProcessContext::kPlaying) != 0;
    hostPlaying = playing;
    if (processContext.state & ProcessContext::kTempoValid)
        hostTempo = processContext.tempo;

    if (data.numSamples <= 0 || data.numOutputs < 1 || data.outputs[0].numChannels < 2)
        return kResultOk;

    MainControllerPlugin *mainController = controllerReady ? controller.data() : nullptr;
    if (!mainController) {
        passThrough(data);
        hostWasPlayingInLastAudioCallBack = playing;
        return kResultOk;
    }

    if (mainController->isPlayingInNinjamRoom() && transportStartDetectedInHost()) { // user pressed play in the host
        NinjamControllerPlugin *ninjamController = mainController->getNinjamController();
        if (ninjamController && ninjamController->isWaitingForHostSync())
            ninjamController->startSynchronizedWithHost(getStartPositionForHostSync());
    }

    const int frames = data.numSamples;
    inputBuffer.setFrameLenght(frames);
    for (int c = 0; c < INPUT_CHANNELS; ++c) {
        const int bus = c / 2;
        const int channel = c % 2;
        float *dest = inputBuffer.getSamplesArray(c);
        const bool available = bus < data.numInputs && channel < data.inputs[bus].numChannels
                               && data.inputs[bus].channelBuffers32 && data.inputs[bus].channelBuffers32[channel];
        if (available)
            std::memcpy(dest, data.inputs[bus].channelBuffers32[channel], sizeof(float) * frames);
        else
            std::memset(dest, 0, sizeof(float) * frames);
    }

    outputBuffer.setFrameLenght(frames);
    outputBuffer.zero();

    mainController->process(inputBuffer, outputBuffer, static_cast<int>(sampleRate.load()));

    AudioBusBuffers &out = data.outputs[0];
    for (int c = 0; c < OUTPUT_CHANNELS && c < out.numChannels; ++c)
        std::memcpy(out.channelBuffers32[c], outputBuffer.getSamplesArray(c), sizeof(float) * frames);
    out.silenceFlags = 0;

    hostWasPlayingInLastAudioCallBack = playing;
    return kResultOk;
}

bool JamTabaVST3::hostIsPlaying() const
{
    return hostPlaying;
}

int JamTabaVST3::getHostBpm() const
{
    return static_cast<int>(std::round(hostTempo.load()));
}

qint32 JamTabaVST3::getStartPositionForHostSync() const
{
    // same rules as the VST2 plugin: start the interval on the next bar line
    const double rate = processContext.sampleRate > 0 ? processContext.sampleRate : sampleRate.load();
    const double tempo = (processContext.state & ProcessContext::kTempoValid) && processContext.tempo > 0 ? processContext.tempo : 120.0;
    const double samplesPerBeat = (60.0 * rate) / tempo;
    const double ppqPos = processContext.projectTimeMusic;
    const double barStartPos = (processContext.state & ProcessContext::kBarPositionValid) ? processContext.barPositionMusic : ppqPos;
    const int beatsPerBar = (processContext.state & ProcessContext::kTimeSigValid) && processContext.timeSigNumerator > 0 ? processContext.timeSigNumerator : 4;

    qint32 startPosition = 0;
    if (ppqPos > 0) {
        const double cursorPosInMeasure = ppqPos - barStartPos;
        if (cursorPosInMeasure > 0.00000001) // the host cursor is not on a bar line, shift the interval start
            startPosition = static_cast<qint32>(-(beatsPerBar - cursorPosInMeasure) * samplesPerBeat);
    } else {
        startPosition = static_cast<qint32>(ppqPos * samplesPerBeat); // pre-roll: negative position
    }
    return startPosition;
}

QString JamTabaVST3::getHostName()
{
    return hostName.isEmpty() ? QStringLiteral("VST3 host") : hostName;
}

float JamTabaVST3::getSampleRate() const
{
    return sampleRate;
}

void JamTabaVST3::setSampleRate(float newSampleRate)
{
    sampleRate = newSampleRate;
    JamTabaPlugin::setSampleRate(newSampleRate);
}

MainControllerPlugin *JamTabaVST3::createPluginMainController(const persistence::Settings &settings, JamTabaPlugin *plugin) const
{
    return new MainControllerVST3(settings, static_cast<JamTabaVST3 *>(plugin));
}

IPlugView *PLUGIN_API JamTabaVST3::createView(FIDString name)
{
    if (!name || std::strcmp(name, ViewType::kEditor) != 0)
        return nullptr;

    QWidget *content = getEditorWidget();
    if (!content)
        return nullptr;

    return new JamTabaVST3View(this, content);
}

QWidget *JamTabaVST3::getEditorWidget()
{
    if (!qtPluginApp::ensureApplication())
        return nullptr;

    if (isDuplicateInstance()) {
        // JamTaba allows one connection per DAW; this instance stays a pass-through
        if (!duplicateInstanceWidget) {
            auto label = new QLabel(QObject::tr("JamTaba is already running on another track in this project.\n"
                                                "This instance passes its input through unchanged."));
            label->setAlignment(Qt::AlignCenter);
            label->setMargin(24);
            label->resize(520, 140);
            duplicateInstanceWidget = label;
        }
        return duplicateInstanceWidget;
    }

    if (!isRunning()) {
        JamTabaPlugin::initialize(); // first editor open: load settings and start JamTaba
        controllerReady = !controller.isNull();
    }

    if (controller.isNull())
        return nullptr;

    if (!mainWindow) {
        auto mainController = static_cast<MainControllerVST3 *>(controller.data());
        auto window = new MainWindowPlugin(mainController);
        mainController->setMainWindow(window);
        window->initialize();
        mainWindow = window;
    }

    return mainWindow;
}

void JamTabaVST3::editorAttached(JamTabaVST3View *view)
{
    currentView = view;
}

void JamTabaVST3::editorRemoved(JamTabaVST3View *view)
{
    if (currentView != view)
        return;

    currentView = nullptr;

    if (mainWindow && !controller.isNull()) {
        controller->storeWindowSettings(false, QPointF(), mainWindow->size());
        mainWindow->closeAllFloatingWindows();
    }
}

void JamTabaVST3::resizeEditor(int width, int height)
{
    if (currentView)
        currentView->requestResize(width, height);
}

void JamTabaVST3::setLinuxRunLoop(Linux::IRunLoop *runLoop)
{
    // keep pumping Qt events after the editor closes: the jam goes on without it
    if (linuxRunLoop || !runLoop)
        return;
    linuxRunLoop = runLoop;
    qtPluginApp::startLinuxEventPump(runLoop);
}

void JamTabaVST3::deleteMainWindow()
{
    if (!mainWindow)
        return;

    if (!controller.isNull())
        controller->setMainWindow(nullptr);

    delete mainWindow;
    mainWindow = nullptr;
}
