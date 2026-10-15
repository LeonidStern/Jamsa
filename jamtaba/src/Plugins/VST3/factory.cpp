#include "JamTabaVST3.h"

#include "public.sdk/source/main/pluginfactory.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"

#define stringPluginName "JamTaba"
#define stringVendor "JamTaba"
#define stringVendorUrl "https://github.com/LeonidStern/ninjam"
#define stringVendorEmail ""

BEGIN_FACTORY_DEF(stringVendor, stringVendorUrl, stringVendorEmail)

DEF_CLASS2(INLINE_UID_FROM_FUID(JamTabaVST3::cid),
           PClassInfo::kManyInstances,
           kVstAudioEffectClass,
           stringPluginName,
           0, // single component: not distributable
           Steinberg::Vst::PlugType::kFx,
           JAMTABA_VST3_VERSION,
           kVstVersionString,
           JamTabaVST3::createInstance)

END_FACTORY
