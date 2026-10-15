/*
    NINJAM VST3 - plugin.cpp
    Copyright (C) 2026 and onward, NINJAM contributors

    NINJAM is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.
*/

#include "plugin.h"

#include <string.h>
#include <algorithm>
#include <string>

#include "base/source/fstreamer.h"
#include "pluginterfaces/base/ibstream.h"
#include "pluginterfaces/vst/ivstprocesscontext.h"

#include "editor.h"

using namespace Steinberg;
using namespace Steinberg::Vst;

namespace ninjam_vst3 {

DEF_CLASS_IID(INinjamCommand)

static const int64 MAX_STATE_SIZE = 1 << 20;

NinjamPlugin::NinjamPlugin()
{
}

NinjamPlugin::~NinjamPlugin()
{
  if (m_engine) m_engine->stop();
}

tresult PLUGIN_API NinjamPlugin::initialize(FUnknown *context)
{
  const tresult r = SingleComponentEffect::initialize(context);
  if (r != kResultOk) return r;

  addAudioInput(STR16("Input"), SpeakerArr::kStereo);
  addAudioOutput(STR16("Output"), SpeakerArr::kStereo);

  if (!m_engine) m_engine.reset(new Engine);
  m_engine->start();
  return kResultOk;
}

tresult PLUGIN_API NinjamPlugin::terminate()
{
  if (m_engine) m_engine->stop();
  return SingleComponentEffect::terminate();
}

tresult PLUGIN_API NinjamPlugin::setActive(TBool)
{
  return kResultOk;
}

tresult PLUGIN_API NinjamPlugin::setBusArrangements(SpeakerArrangement *inputs, int32 numIns, SpeakerArrangement *outputs, int32 numOuts)
{
  // a mono or stereo input, a stereo output
  if (numIns != 1 || numOuts != 1) return kResultFalse;
  if (inputs[0] != SpeakerArr::kStereo && inputs[0] != SpeakerArr::kMono) return kResultFalse;
  if (outputs[0] != SpeakerArr::kStereo) return kResultFalse;
  return SingleComponentEffect::setBusArrangements(inputs, numIns, outputs, numOuts);
}

tresult PLUGIN_API NinjamPlugin::canProcessSampleSize(int32 symbolicSampleSize)
{
  return symbolicSampleSize == kSample32 ? kResultTrue : kResultFalse;
}

tresult PLUGIN_API NinjamPlugin::setupProcessing(ProcessSetup &setup)
{
  if (setup.symbolicSampleSize != kSample32) return kResultFalse;
  if (m_engine) m_engine->setupProcessing(setup.sampleRate, setup.maxSamplesPerBlock);
  return SingleComponentEffect::setupProcessing(setup);
}

tresult PLUGIN_API NinjamPlugin::setProcessing(TBool)
{
  return kResultOk;
}

tresult PLUGIN_API NinjamPlugin::process(ProcessData &data)
{
  if (data.numSamples <= 0 || data.numOutputs < 1 || !data.outputs) return kResultOk; // a parameter flush
  AudioBusBuffers &out = data.outputs[0];
  if (out.numChannels < 1 || !out.channelBuffers32) return kResultOk;

  float **in = nullptr;
  int numIn = 0;
  if (data.numInputs > 0 && data.inputs && data.inputs[0].channelBuffers32)
  {
    in = data.inputs[0].channelBuffers32;
    numIn = data.inputs[0].numChannels;
  }
  m_engine->process(in, numIn, out.channelBuffers32, out.numChannels, data.numSamples);
  out.silenceFlags = 0;
  return kResultOk;
}

// The state is text (see Settings::serialize). SingleComponentEffect has one setState and
// one getState for both the component and the controller, so a host may load it twice.
tresult PLUGIN_API NinjamPlugin::setState(IBStream *state)
{
  if (!state || !m_engine) return kInvalidArgument;
  std::string data;
  char buf[4096];
  for (;;)
  {
    int32 got = 0;
    if (state->read(buf, sizeof(buf), &got) != kResultOk || got <= 0) break;
    data.append(buf, (size_t)got);
    if ((int64)data.size() > MAX_STATE_SIZE) return kResultFalse;
  }
  Settings s;
  if (!s.deserialize(data)) return kResultFalse;
  m_engine->setSettings(s);
  return kResultOk;
}

tresult PLUGIN_API NinjamPlugin::getState(IBStream *state)
{
  if (!state || !m_engine) return kInvalidArgument;
  const std::string data = m_engine->getSettings().serialize();
  int32 written = 0;
  if (state->write((void *)data.data(), (int32)data.size(), &written) != kResultOk || written != (int32)data.size()) return kResultFalse;
  return kResultOk;
}

tresult PLUGIN_API NinjamPlugin::setComponentState(IBStream *)
{
  // the same object already has the state (setState)
  return kResultOk;
}

IPlugView *PLUGIN_API NinjamPlugin::createView(FIDString name)
{
  if (!name || strcmp(name, ViewType::kEditor) != 0 || !m_engine) return nullptr;
  return new NinjamEditor(this);
}

int32 PLUGIN_API NinjamPlugin::runCommand(const char *line, char *reply, int32 replySize)
{
  const std::string r = m_engine && line ? m_engine->command(line) : std::string("error: not initialized");
  if (reply && replySize > 0)
  {
    const size_t n = std::min(r.size(), (size_t)replySize - 1);
    memcpy(reply, r.data(), n);
    reply[n] = 0;
  }
  return (int32)r.size();
}

} // namespace ninjam_vst3
