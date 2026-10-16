/*
    NINJAM VST3 - textfield.cpp
    Copyright (C) 2026 and onward, NINJAM contributors

    NINJAM is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.
*/

// Windows, and Linux (where there is nothing to do); macOS is in macos.mm.

#include "textfield.h"

#if defined(_WIN32)
#include <windows.h>

#include <algorithm>
#include <mutex>
#include <string>
#include <vector>

#include "clipboard.h"
#endif

namespace ninjam_vst3 {
namespace textfield {

#if defined(_WIN32)

namespace {

std::mutex g_mutex;
struct Frame
{
  HWND window; // VSTGUI makes the frame's EDIT controls in it
  DWORD thread;
};
std::vector<Frame> g_frames;
struct ThreadHook
{
  DWORD thread;
  HHOOK hook;
  int users;
};
std::vector<ThreadHook> g_hooks; // one hook for each thread with frames

// w is an EDIT control in the frame's window, or in one of ours when frame is null
bool isEditIn(HWND w, HWND frame)
{
  if (!w) return false;
  const HWND parent = GetParent(w);
  if (!parent) return false;
  if (frame)
  {
    if (parent != frame) return false;
  }
  else
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (std::none_of(g_frames.begin(), g_frames.end(), [&](const Frame &f) { return f.window == parent; })) return false;
  }
  wchar_t name[16] = {};
  return GetClassNameW(w, name, 16) > 0 && lstrcmpiW(name, L"Edit") == 0;
}

// the command of a WM_KEYDOWN's key, with the modifiers held now (as the thread's
// messages say: GetKeyState)
textkeys::Command commandForKey(WPARAM vk)
{
  if (vk < 'A' || vk > 'Z') return textkeys::Command::None; // the letters' virtual keys are their capitals
  return textkeys::commandFor(textkeys::letterOf((char32_t)vk), GetKeyState(VK_CONTROL) < 0, GetKeyState(VK_SHIFT) < 0,
                              GetKeyState(VK_MENU) < 0, GetKeyState(VK_LWIN) < 0 || GetKeyState(VK_RWIN) < 0);
}

std::wstring wide(const std::string &s)
{
  std::wstring w;
  const int n = s.empty() ? 0 : MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
  if (n > 0)
  {
    w.resize((size_t)n);
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
  }
  return w;
}

void performIn(HWND edit, textkeys::Command command)
{
  switch (command)
  {
    case textkeys::Command::Cut: SendMessageW(edit, WM_CUT, 0, 0); break;
    case textkeys::Command::Copy: SendMessageW(edit, WM_COPY, 0, 0); break;
    case textkeys::Command::Paste:
    {
      std::string text;
      if (clipboard::getText(&text))
      {
        // in place of the selection, as if typed (EN_CHANGE tells VSTGUI; it can be undone)
        const std::wstring w = wide(textkeys::oneLine(text));
        if (!w.empty()) SendMessageW(edit, EM_REPLACESEL, TRUE, (LPARAM)w.c_str());
      }
      break;
    }
    case textkeys::Command::SelectAll: SendMessageW(edit, EM_SETSEL, 0, -1); break;
    default: break;
  }
}

// Called with each message the thread's message loop gets, before the loop looks at it (the
// host's shortcuts, TranslateMessage): a command's key in one of our EDIT controls is done
// here, and the loop gets WM_NULL instead.
LRESULT CALLBACK getMessageHook(int code, WPARAM wParam, LPARAM lParam)
{
  if (code == HC_ACTION && lParam)
  {
    MSG *m = (MSG *)lParam;
    if (m->message == WM_KEYDOWN)
    {
      const textkeys::Command command = commandForKey(m->wParam);
      if (command != textkeys::Command::None && isEditIn(m->hwnd, nullptr))
      {
        const HWND edit = m->hwnd;
        m->message = WM_NULL;
        m->wParam = 0;
        m->lParam = 0;
        // done when taken from the queue; a look that leaves it there (PM_NOREMOVE) sees WM_NULL
        if (wParam == PM_REMOVE) performIn(edit, command);
      }
    }
  }
  return CallNextHookEx(nullptr, code, wParam, lParam);
}

const int g_inThisModule = 0; // an address in the plug-in, to find its module by

} // namespace

bool perform(void *frameView, textkeys::Command command)
{
  const HWND focus = GetFocus();
  if (command == textkeys::Command::None || !frameView || !isEditIn(focus, (HWND)frameView)) return false;
  performIn(focus, command);
  return true;
}

void ShortcutCatcher::start(void *frameView)
{
  stop();
  if (!frameView) return;
  const DWORD thread = GetCurrentThreadId();
  std::lock_guard<std::mutex> lock(g_mutex);
  auto h = std::find_if(g_hooks.begin(), g_hooks.end(), [&](const ThreadHook &t) { return t.thread == thread; });
  if (h != g_hooks.end()) h->users++;
  else
  {
    HMODULE module = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCWSTR)&g_inThisModule, &module);
    const HHOOK hook = SetWindowsHookExW(WH_GETMESSAGE, getMessageHook, module, thread);
    if (!hook) return;
    g_hooks.push_back({ thread, hook, 1 });
  }
  g_frames.push_back({ (HWND)frameView, thread });
  m_frameView = frameView;
}

void ShortcutCatcher::stop()
{
  if (!m_frameView) return;
  std::lock_guard<std::mutex> lock(g_mutex);
  auto f = std::find_if(g_frames.begin(), g_frames.end(), [&](const Frame &fr) { return fr.window == (HWND)m_frameView; });
  if (f != g_frames.end())
  {
    const DWORD thread = f->thread;
    g_frames.erase(f);
    auto h = std::find_if(g_hooks.begin(), g_hooks.end(), [&](const ThreadHook &t) { return t.thread == thread; });
    if (h != g_hooks.end() && --h->users == 0)
    {
      UnhookWindowsHookEx(h->hook);
      g_hooks.erase(h);
    }
  }
  m_frameView = nullptr;
}

#elif !defined(__APPLE__)

// Linux: VSTGUI draws the text fields itself, and they take their keys from the frame
bool perform(void *, textkeys::Command)
{
  return false;
}

void ShortcutCatcher::start(void *frameView)
{
  m_frameView = frameView;
  m_monitor = nullptr;
}

void ShortcutCatcher::stop()
{
  m_frameView = nullptr;
}

#endif

} // namespace textfield
} // namespace ninjam_vst3
