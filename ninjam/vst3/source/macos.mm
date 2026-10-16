/*
    NINJAM VST3 - macos.mm
    Copyright (C) 2026 and onward, NINJAM contributors

    NINJAM is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.
*/

// What macOS does in Objective-C: the clipboard's text (clipboard.h, from NSPasteboard) and
// the commands in the editor's text fields (textfield.h). Built with manual retain and
// release, as VSTGUI is; it would build with ARC too.

#import <Cocoa/Cocoa.h>

#include <string>

#include "clipboard.h"
#include "textfield.h"
#include "textkeys.h"

namespace ninjam_vst3 {

namespace clipboard {

bool getText(std::string *text)
{
  @autoreleasepool
  {
    NSString *s = [[NSPasteboard generalPasteboard] stringForType:NSPasteboardTypeString];
    if (!s) return false;
    const char *utf8 = [s UTF8String];
    if (text) text->assign(utf8 ? utf8 : "");
    return true;
  }
}

} // namespace clipboard

namespace textfield {

namespace {

// The field editor (the NSTextView of the window that does the typing in its text fields) when
// it has the keyboard focus in a text field in root, else nil. While it edits a field it is in
// that field, and the field is its delegate.
NSTextView *focusedFieldEditor(NSView *root)
{
  NSWindow *window = [root window];
  if (!window) return nil;
  NSResponder *responder = [window firstResponder];
  if (![responder isKindOfClass:[NSTextView class]]) return nil;
  NSTextView *editor = (NSTextView *)responder;
  if (![editor isFieldEditor]) return nil;
  if ([editor isDescendantOf:root]) return editor;
  id field = [editor delegate];
  if ([field isKindOfClass:[NSView class]] && [(NSView *)field isDescendantOf:root]) return editor;
  return nil;
}

void performIn(NSTextView *editor, textkeys::Command command)
{
  switch (command)
  {
    case textkeys::Command::Cut: [editor cut:nil]; break;
    case textkeys::Command::Copy: [editor copy:nil]; break;
    case textkeys::Command::Paste:
    {
      std::string text;
      if (clipboard::getText(&text))
      {
        // in place of the selection, as if typed (the text field hears of it as of typing)
        const std::string line = textkeys::oneLine(text);
        NSString *s = line.empty() ? nil : [NSString stringWithUTF8String:line.c_str()];
        if (s) [editor insertText:s replacementRange:[editor selectedRange]];
      }
      break;
    }
    case textkeys::Command::SelectAll: [editor selectAll:nil]; break;
    default: break;
  }
}

// The command of a key: with Cmd held, a layout like Russian types its Latin letters
// (characters), and charactersIgnoringModifiers its own; the key's place is the last resort.
textkeys::Command commandForEvent(NSEvent *event)
{
  if ([event type] != NSEventTypeKeyDown) return textkeys::Command::None;
  NSString *typed = [event characters], *plain = [event charactersIgnoringModifiers];
  const char32_t a = [typed length] ? (char32_t)[typed characterAtIndex:0] : 0;
  const char32_t b = [plain length] ? (char32_t)[plain characterAtIndex:0] : 0;
  const char32_t letter = textkeys::latinLetter({ a, b }, textkeys::macUsLetter([event keyCode]));
  const NSEventModifierFlags flags = [event modifierFlags];
  return textkeys::commandFor(letter, (flags & NSEventModifierFlagCommand) != 0, (flags & NSEventModifierFlagShift) != 0,
                              (flags & NSEventModifierFlagOption) != 0, (flags & NSEventModifierFlagControl) != 0);
}

} // namespace

bool perform(void *frameView, textkeys::Command command)
{
  if (command == textkeys::Command::None || !frameView) return false;
  NSTextView *editor = focusedFieldEditor((__bridge NSView *)frameView);
  if (!editor) return false;
  performIn(editor, command);
  return true;
}

void ShortcutCatcher::start(void *frameView)
{
  stop();
  if (!frameView) return;
  // A local monitor sees each key the application gets before it is dispatched: before the
  // window and the host's Edit menu (or the host's own key handling) see it.
  void *rootView = frameView;
  id monitor = [NSEvent addLocalMonitorForEventsMatchingMask:NSEventMaskKeyDown
                                                     handler:^NSEvent *(NSEvent *event) {
                                                       NSView *root = (__bridge NSView *)rootView;
                                                       NSWindow *window = [event window];
                                                       if (!window || window != [root window]) return event;
                                                       const textkeys::Command command = commandForEvent(event);
                                                       if (command == textkeys::Command::None) return event;
                                                       NSTextView *editor = focusedFieldEditor(root);
                                                       if (!editor) return event;
                                                       performIn(editor, command);
                                                       return nil; // done: nobody else gets the key
                                                     }];
  if (!monitor) return;
#if __has_feature(objc_arc)
  m_monitor = (__bridge_retained void *)monitor;
#else
  m_monitor = (__bridge void *)[monitor retain];
#endif
  m_frameView = frameView;
}

void ShortcutCatcher::stop()
{
  if (m_monitor)
  {
#if __has_feature(objc_arc)
    id monitor = (__bridge_transfer id)m_monitor;
    [NSEvent removeMonitor:monitor];
#else
    id monitor = (__bridge id)m_monitor;
    [NSEvent removeMonitor:monitor];
    [monitor release];
#endif
    m_monitor = nullptr;
  }
  m_frameView = nullptr;
}

} // namespace textfield
} // namespace ninjam_vst3
