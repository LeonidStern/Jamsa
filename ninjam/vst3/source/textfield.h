/*
    NINJAM VST3 - textfield.h
    Copyright (C) 2026 and onward, NINJAM contributors

    NINJAM is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.
*/

/*

  Cut, Copy, Paste and Select All in the system's text fields of the editor.

  While a text box is typed in, VSTGUI puts a text field of the system's in it:
  an NSTextField on macOS, a Win32 EDIT control on Windows (on Linux VSTGUI
  draws one itself). On macOS the text field leaves Cmd+X, C, V, A to the
  application's Edit menu, whose items send cut:, copy:, paste:, selectAll: to
  it; in a host the Edit menu is the host's, and its items (Ableton Live's) do
  the host's own commands instead. On Windows the EDIT control does Ctrl+X, C,
  V from the characters the host's message loop makes of the keys, but a host
  that has these keys as its own shortcuts (Live) takes them out of the loop
  before that. So the plug-in looks at the keys before the host does (an
  NSEvent monitor on macOS, a WH_GETMESSAGE hook on Windows) and, when the
  focus is in one of its text fields, does the command in that field itself;
  the host never sees the key.

  The functions take the frame's view in the system: its NSView on macOS, its
  HWND on Windows (IPlatformFrame::getPlatformRepresentation()). The UI thread
  calls them. On Linux they do nothing.

*/

#ifndef _NINJAM_VST3_TEXTFIELD_H_
#define _NINJAM_VST3_TEXTFIELD_H_

#include "textkeys.h"

namespace ninjam_vst3 {
namespace textfield {

// Does the command in the system's text field that has the keyboard focus, when it is one
// of the frame's; false when none of them has it. Paste pastes the clipboard's text as one
// line (textkeys::oneLine).
bool perform(void *frameView, textkeys::Command command);

// Catches the commands' keys in the frame's text fields before the host sees them, from
// start() until stop() (or until it is destroyed).
class ShortcutCatcher
{
public:
  ShortcutCatcher() = default;
  ShortcutCatcher(const ShortcutCatcher &) = delete;
  ShortcutCatcher &operator=(const ShortcutCatcher &) = delete;
  ~ShortcutCatcher() { stop(); }

  void start(void *frameView);
  void stop();

private:
  void *m_frameView = nullptr;
  void *m_monitor = nullptr; // macOS: the NSEvent monitor (retained)
};

} // namespace textfield
} // namespace ninjam_vst3

#endif
