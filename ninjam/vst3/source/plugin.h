/*
    NINJAM VST3 - plugin.h
    Copyright (C) 2026 and onward, NINJAM contributors

    NINJAM is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.
*/

/*

  The plug-in: a stereo effect whose input is sent to the NINJAM server as
  our channel, and whose output is what the other players play, plus the
  metronome and, if monitoring is on, our own input.

  Processor and controller are one object (SingleComponentEffect): the
  plug-in has no automatable parameters, and its editor talks to the Engine
  directly.

*/

#ifndef _NINJAM_VST3_PLUGIN_H_
#define _NINJAM_VST3_PLUGIN_H_

#include <memory>

#include "public.sdk/source/vst/vstsinglecomponenteffect.h"

#include "engine.h"
#include "pluginids.h"

namespace ninjam_vst3 {

class NinjamPlugin : public Steinberg::Vst::SingleComponentEffect, public INinjamCommand
{
public:
  NinjamPlugin();
  ~NinjamPlugin() override;

  static Steinberg::FUnknown *createInstance(void *)
  {
    return (Steinberg::Vst::IAudioProcessor *)new NinjamPlugin;
  }

  // IComponent / IPluginBase
  Steinberg::tresult PLUGIN_API initialize(Steinberg::FUnknown *context) override;
  Steinberg::tresult PLUGIN_API terminate() override;
  Steinberg::tresult PLUGIN_API setActive(Steinberg::TBool state) override;
  Steinberg::tresult PLUGIN_API setState(Steinberg::IBStream *state) override;
  Steinberg::tresult PLUGIN_API getState(Steinberg::IBStream *state) override;

  // IAudioProcessor
  Steinberg::tresult PLUGIN_API setBusArrangements(Steinberg::Vst::SpeakerArrangement *inputs, Steinberg::int32 numIns,
                                                   Steinberg::Vst::SpeakerArrangement *outputs, Steinberg::int32 numOuts) override;
  Steinberg::tresult PLUGIN_API canProcessSampleSize(Steinberg::int32 symbolicSampleSize) override;
  Steinberg::tresult PLUGIN_API setupProcessing(Steinberg::Vst::ProcessSetup &setup) override;
  Steinberg::tresult PLUGIN_API setProcessing(Steinberg::TBool state) override;
  Steinberg::tresult PLUGIN_API process(Steinberg::Vst::ProcessData &data) override;
  Steinberg::uint32 PLUGIN_API getTailSamples() override { return Steinberg::Vst::kInfiniteTail; }

  // IEditController
  Steinberg::tresult PLUGIN_API setComponentState(Steinberg::IBStream *state) override;
  Steinberg::IPlugView *PLUGIN_API createView(Steinberg::FIDString name) override;

  // INinjamCommand
  Steinberg::int32 PLUGIN_API runCommand(const char *line, char *reply, Steinberg::int32 replySize) override;

  Engine &engine() { return *m_engine; }

  OBJ_METHODS(NinjamPlugin, Steinberg::Vst::SingleComponentEffect)
  DEFINE_INTERFACES
    DEF_INTERFACE(INinjamCommand)
  END_DEFINE_INTERFACES(Steinberg::Vst::SingleComponentEffect)
  REFCOUNT_METHODS(Steinberg::Vst::SingleComponentEffect)

private:
  std::unique_ptr<Engine> m_engine;
};

} // namespace ninjam_vst3

#endif
