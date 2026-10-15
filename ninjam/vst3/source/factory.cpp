/*
    NINJAM VST3 - factory.cpp
    Copyright (C) 2026 and onward, NINJAM contributors

    NINJAM is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.
*/

#include "public.sdk/source/main/pluginfactory.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"

#include "plugin.h"
#include "pluginids.h"
#include "version.h"

using namespace Steinberg;
using namespace Steinberg::Vst;

namespace {

// On Linux, the SDK's VSTGUI glue asks the factory to replace VSTGUI's run loop with the host
// context's each time the host calls setHostContext, which hosts may do on any thread, also
// while an editor is open: the editor's timers then go to a run loop that is gone, and the host
// crashes (pluginval's "Background thread state" test does this). The editor takes the run
// loop from its plug frame instead (see NinjamEditor::open), so the factory doesn't offer that
// hook.
class NinjamPluginFactory : public CPluginFactory
{
public:
  using CPluginFactory::CPluginFactory;

  tresult PLUGIN_API queryInterface(const TUID _iid, void **obj) override
  {
#if SMTG_OS_LINUX
    if (FUnknownPrivate::iidEqual(_iid, IPluginFactoryInternal::iid))
    {
      *obj = nullptr;
      return kNoInterface;
    }
#endif
    return CPluginFactory::queryInterface(_iid, obj);
  }
};

} // namespace

BEGIN_FACTORY_CLASS(NinjamPluginFactory, NINJAM_VST3_VENDOR, NINJAM_VST3_URL, "", Vst::kDefaultFactoryFlags)

DEF_CLASS2(INLINE_UID_FROM_FUID(ninjam_vst3::kNinjamPluginUID),
           PClassInfo::kManyInstances,
           kVstAudioEffectClass,
           NINJAM_VST3_NAME,
           0, // processor and controller are one object, so not distributable
           PlugType::kFxNetwork,
           NINJAM_VST3_VERSION,
           kVstVersionString,
           ninjam_vst3::NinjamPlugin::createInstance)

END_FACTORY
