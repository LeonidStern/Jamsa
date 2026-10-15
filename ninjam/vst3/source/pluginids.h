/*
    NINJAM VST3 - pluginids.h
    Copyright (C) 2026 and onward, NINJAM contributors

    NINJAM is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.
*/

#ifndef _NINJAM_VST3_PLUGINIDS_H_
#define _NINJAM_VST3_PLUGINIDS_H_

#include "pluginterfaces/base/funknown.h"

namespace ninjam_vst3 {

// the plug-in's class ID; hosts store it in projects, so it never changes
static const Steinberg::FUID kNinjamPluginUID(0x521298D0, 0x959C4442, 0xA011C9AD, 0xA462CFB0);

// The plug-in's text command interface, for tests and automation: a host gets it with
// queryInterface from the component. See Engine::command for the commands.
class INinjamCommand : public Steinberg::FUnknown
{
public:
  // Runs one command and copies the reply into reply (cut to replySize - 1 bytes, then a 0);
  // returns the whole reply's length, so a caller can ask again with more room.
  virtual Steinberg::int32 PLUGIN_API runCommand(const char *line, char *reply, Steinberg::int32 replySize) = 0;

  static const Steinberg::FUID iid;
};
DECLARE_CLASS_IID(INinjamCommand, 0x7D3E5A41, 0x2C9B4F18, 0x9E61B0D7, 0x43A85C2E)

} // namespace ninjam_vst3

#endif
