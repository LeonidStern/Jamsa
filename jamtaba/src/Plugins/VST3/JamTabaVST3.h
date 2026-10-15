#ifndef JAMTABA_VST3_H
#define JAMTABA_VST3_H

#include "JamTabaPlugin.h" // base plugin class

#include "public.sdk/source/vst/vstsinglecomponenteffect.h"
#include "pluginterfaces/vst/ivstprocesscontext.h"

#include <QString>
#include <atomic>

class MainWindow;
class JamTabaVST3View;
QT_BEGIN_NAMESPACE
class QWidget;
QT_END_NAMESPACE

namespace Steinberg {
namespace Linux {
class IRunLoop;
}
}

/**
    JamTaba as a VST 3 effect: one stereo input (plus an optional second stereo input
    for a second local channel) and one stereo output.

    Processor and edit controller are one object (SingleComponentEffect): the GUI and the
    audio engine share the JamTaba MainController, as they did in the VST2 plugin.
*/
class JamTabaVST3 : public Steinberg::Vst::SingleComponentEffect, public JamTabaPlugin
{
public:
    static const Steinberg::FUID cid;

    JamTabaVST3();
    ~JamTabaVST3() override;

    static Steinberg::FUnknown *createInstance(void *);

    // IPluginBase
    Steinberg::tresult PLUGIN_API initialize(Steinberg::FUnknown *context) override;
    Steinberg::tresult PLUGIN_API terminate() override;

    // IComponent
    Steinberg::tresult PLUGIN_API setActive(Steinberg::TBool state) override;
    Steinberg::tresult PLUGIN_API setState(Steinberg::IBStream *state) override;
    Steinberg::tresult PLUGIN_API getState(Steinberg::IBStream *state) override;

    // IAudioProcessor
    Steinberg::tresult PLUGIN_API setBusArrangements(Steinberg::Vst::SpeakerArrangement *inputs, Steinberg::int32 numIns,
                                                     Steinberg::Vst::SpeakerArrangement *outputs, Steinberg::int32 numOuts) override;
    Steinberg::tresult PLUGIN_API canProcessSampleSize(Steinberg::int32 symbolicSampleSize) override;
    Steinberg::tresult PLUGIN_API setupProcessing(Steinberg::Vst::ProcessSetup &setup) override;
    Steinberg::tresult PLUGIN_API process(Steinberg::Vst::ProcessData &data) override;
    Steinberg::uint32 PLUGIN_API getTailSamples() override;

    // IEditController
    Steinberg::IPlugView *PLUGIN_API createView(Steinberg::FIDString name) override;

    // JamTabaPlugin
    QString getHostName() override;
    float getSampleRate() const override;
    void setSampleRate(float sampleRate) override;
    int getHostBpm() const override;
    void close() override;

    // GUI side, called from the view
    QWidget *getEditorWidget(); // creates JamTaba's main window (and starts JamTaba) the first time
    void editorAttached(JamTabaVST3View *view);
    void editorRemoved(JamTabaVST3View *view);
    void resizeEditor(int width, int height); // JamTaba asks the host for a new editor size
    void setLinuxRunLoop(Steinberg::Linux::IRunLoop *runLoop);

    bool isDuplicateInstance() const; // another JamTaba instance is already running in this process

protected:
    bool hostIsPlaying() const override;
    qint32 getStartPositionForHostSync() const override;
    MainControllerPlugin *createPluginMainController(const persistence::Settings &settings, JamTabaPlugin *plugin) const override;

private:
    void passThrough(Steinberg::Vst::ProcessData &data);
    void deleteMainWindow();

    QString hostName;
    std::atomic<float> sampleRate;
    std::atomic<double> hostTempo;
    std::atomic<bool> hostPlaying;
    std::atomic<bool> controllerReady;
    Steinberg::Vst::ProcessContext processContext; // copy of the host's context for the current block

    MainWindow *mainWindow;
    QWidget *duplicateInstanceWidget;
    JamTabaVST3View *currentView;
    Steinberg::Linux::IRunLoop *linuxRunLoop;
};

#endif
