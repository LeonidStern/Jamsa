/*
    NINJAM VST3 - textkeys.h
    Copyright (C) 2026 and onward, NINJAM contributors

    NINJAM is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.
*/

/*

  Cut, Copy, Paste and Select All in the editor's text boxes, without VSTGUI
  or the system (so the unit test can check it): which key does which, and
  what a pasted text becomes in a box of one line.

  The keys are Cmd+X, C, V, A on macOS and Ctrl+X, C, V, A elsewhere. Hosts
  take them before the system's text field sees them (they are the host's own
  Edit commands), so the plug-in catches them itself (textfield.h).

*/

#ifndef _NINJAM_VST3_TEXTKEYS_H_
#define _NINJAM_VST3_TEXTKEYS_H_

#include <initializer_list>
#include <string>

namespace ninjam_vst3 {
namespace textkeys {

enum class Command { None, Cut, Copy, Paste, SelectAll };

// The command of a key pressed with these modifiers: primary is Cmd on macOS and Ctrl
// elsewhere; other is Ctrl on macOS and the Windows (Super) key elsewhere. letter is what
// the key types (letterOf), 0 for none. Only the primary modifier alone makes a command:
// with Alt (AltGr on Windows types letters), Shift or the other one it is another key.
Command commandFor(char32_t letter, bool primary, bool shift, bool alt, bool other);

// A key's Latin letter in lower case, from a character it types: 'C' and 'c' are 'c', and
// so is 3, the control character Ctrl+C types; 0 for anything else.
char32_t letterOf(char32_t c);

// The letter of a key: the first of what the system says it types that is a Latin letter
// (on macOS, a layout like Russian types Latin letters with Cmd held, and its own without),
// else usLetter, the letter on that key of a US keyboard (0 when none).
char32_t latinLetter(std::initializer_list<char32_t> typed, char32_t usLetter);

// The letter of a key on a US keyboard from its macOS key code (kVK_ANSI_A ...), 0 when it
// isn't one of the letters of the commands.
char32_t macUsLetter(unsigned short keyCode);

// What clipboard text becomes when pasted into a text box of one line. Tabs are spaces and
// other control characters go. Text of several lines ("host:2049\n", a paragraph) is its
// lines without the spaces at their ends, the empty ones left out, joined by spaces; text
// of one line keeps its spaces.
std::string oneLine(const std::string &text);

} // namespace textkeys
} // namespace ninjam_vst3

#endif
