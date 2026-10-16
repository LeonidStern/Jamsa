/*
    NINJAM VST3 - clipboard.h
    Copyright (C) 2026 and onward, NINJAM contributors

    NINJAM is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.
*/

/*

  Text to and from the system clipboard: for the host bar's Copy buttons, and
  for pasting into the text boxes (textfield.h).

  On Windows and macOS the text goes on VSTGUI's clipboard, and is read with
  the system's own calls (Win32's clipboard, NSPasteboard in macos.mm). On
  Linux VSTGUI has none (its X11 factory doesn't do it yet), so the plug-in
  owns X11's CLIPBOARD selection itself: a thread with a connection of its own
  to the X server answers the programs that paste, until another program
  copies something (or the module is unloaded; a clipboard manager, as
  desktops have, keeps the text after that).

*/

#ifndef _NINJAM_VST3_CLIPBOARD_H_
#define _NINJAM_VST3_CLIPBOARD_H_

#include <string>

namespace ninjam_vst3 {
namespace clipboard {

// false when there is no clipboard to put it on
bool setText(const std::string &text);
// the text on the clipboard, as UTF-8; false when it holds none (or no text)
bool getText(std::string *text);

#if defined(__linux__)
// What the CLIPBOARD selection holds now, as UTF-8, read the way a program that pastes does;
// false when nobody owns it or it doesn't answer within timeoutSeconds. getText() and the
// tests read it so.
bool readX11(std::string *text, double timeoutSeconds);
// Lets go of the selection and stops the thread (the module does when it is unloaded).
void stopX11();
#endif

} // namespace clipboard
} // namespace ninjam_vst3

#endif
