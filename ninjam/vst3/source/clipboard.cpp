/*
    NINJAM VST3 - clipboard.cpp
    Copyright (C) 2026 and onward, NINJAM contributors

    NINJAM is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.
*/

#include "clipboard.h"

#if defined(__linux__)
#include <fcntl.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <xcb/xcb.h>

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#else
#include "vstgui/lib/cclipboard.h"
#endif

namespace ninjam_vst3 {
namespace clipboard {

#if defined(__linux__)

namespace {

const size_t MAX_TEXT = 60000; // more would need X11's INCR transfers; an address is short

xcb_atom_t intern(xcb_connection_t *c, const char *name)
{
  xcb_intern_atom_reply_t *r = xcb_intern_atom_reply(c, xcb_intern_atom(c, 0, (uint16_t)strlen(name), name), nullptr);
  const xcb_atom_t a = r ? r->atom : (xcb_atom_t)XCB_ATOM_NONE;
  free(r);
  return a;
}

// A connection to the X server. Up to three tries: Xvfb sometimes hangs up on a connection
// made just as another of ours closed (seen in CI, about once in 70 runs); the next one works.
xcb_connection_t *connect_x()
{
  for (int attempt = 0; attempt < 3; attempt++)
  {
    xcb_connection_t *c = xcb_connect(nullptr, nullptr);
    if (!xcb_connection_has_error(c)) return c;
    xcb_disconnect(c);
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return nullptr;
}

// a window of our own to own the selection with (never shown)
xcb_window_t make_window(xcb_connection_t *c)
{
  const xcb_screen_t *screen = xcb_setup_roots_iterator(xcb_get_setup(c)).data;
  if (!screen) return 0;
  const xcb_window_t w = xcb_generate_id(c);
  const uint32_t events = XCB_EVENT_MASK_PROPERTY_CHANGE;
  xcb_create_window(c, XCB_COPY_FROM_PARENT, w, screen->root, 0, 0, 1, 1, 0, XCB_WINDOW_CLASS_INPUT_ONLY,
                    XCB_COPY_FROM_PARENT, XCB_CW_EVENT_MASK, &events);
  return w;
}

// waits for the connection to have something to read, at most `ms` (-1: no limit) or until
// `wakeFd` is readable; false on a timeout
bool wait_readable(xcb_connection_t *c, int wakeFd, int ms, bool *woken)
{
  pollfd fds[2] = { { xcb_get_file_descriptor(c), POLLIN, 0 }, { wakeFd, POLLIN, 0 } };
  const int n = poll(fds, wakeFd >= 0 ? 2 : 1, ms);
  if (woken) *woken = wakeFd >= 0 && n > 0 && (fds[1].revents & POLLIN);
  return n > 0;
}

class Owner
{
public:
  ~Owner() { stop(); }

  bool set(const std::string &text)
  {
    if (text.size() > MAX_TEXT) return false;
    std::unique_lock<std::mutex> lock(m_mutex);
    // twice: the thread may have just let go of the selection (another program copied) and be ending
    for (int attempt = 0; attempt < 2; attempt++)
    {
      if (!m_running)
      {
        if (m_thread.joinable())
        {
          // the thread of an earlier copy, which let go of the selection
          lock.unlock();
          m_thread.join();
          lock.lock();
        }
        if (!open()) return false;
        m_running = true;
        m_quit = false;
        m_thread = std::thread(&Owner::run, this);
      }
      m_text = text;
      const unsigned long long want = ++m_asked;
      wake();
      m_cv.wait_for(lock, std::chrono::seconds(2), [&] { return m_answered >= want || !m_running; });
      if (m_answered >= want) return m_owned;
      if (m_running) return false; // the X server doesn't answer
    }
    return false;
  }

  void stop()
  {
    std::unique_lock<std::mutex> lock(m_mutex);
    if (m_running)
    {
      m_quit = true;
      wake();
    }
    lock.unlock();
    if (m_thread.joinable()) m_thread.join();
  }

private:
  // the connection and the window, made by set() before the thread starts
  bool open()
  {
    m_conn = connect_x();
    if (!m_conn) return false;
    m_window = make_window(m_conn);
    m_clipboard = intern(m_conn, "CLIPBOARD");
    m_targets = intern(m_conn, "TARGETS");
    m_timestamp = intern(m_conn, "TIMESTAMP");
    m_utf8 = intern(m_conn, "UTF8_STRING");
    m_textAtom = intern(m_conn, "TEXT");
    m_plainUtf8 = intern(m_conn, "text/plain;charset=utf-8");
    m_plain = intern(m_conn, "text/plain");
    m_timeProperty = intern(m_conn, "NINJAM_CLIPBOARD_TIME");
    if (!m_window || pipe(m_wake) != 0)
    {
      xcb_disconnect(m_conn);
      m_conn = nullptr;
      return false;
    }
    fcntl(m_wake[0], F_SETFL, O_NONBLOCK);
    fcntl(m_wake[1], F_SETFL, O_NONBLOCK);
    xcb_flush(m_conn);
    return true;
  }

  void wake()
  {
    const char b = 1;
    if (m_wake[1] >= 0 && write(m_wake[1], &b, 1) < 0) {} // full: it is awake anyway
  }

  void run()
  {
    unsigned long long taking = 0; // the request whose selection we are taking
    for (;;)
    {
      bool woken = false;
      wait_readable(m_conn, m_wake[0], -1, &woken);
      if (woken)
      {
        char buf[64];
        while (read(m_wake[0], buf, sizeof(buf)) > 0) {}
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_quit) break;
        if (m_asked > m_answered && !taking)
        {
          // The selection is taken at a time the server gave us (as X11 asks): appending
          // nothing to a property of our window sends us a PropertyNotify with that time.
          taking = m_asked;
          xcb_change_property(m_conn, XCB_PROP_MODE_APPEND, m_window, m_timeProperty, XCB_ATOM_STRING, 8, 0, nullptr);
          xcb_flush(m_conn);
        }
      }
      bool lost = false;
      while (xcb_generic_event_t *e = xcb_poll_for_event(m_conn))
      {
        switch (e->response_type & ~0x80)
        {
          case XCB_PROPERTY_NOTIFY:
          {
            auto *p = (xcb_property_notify_event_t *)e;
            if (taking && p->window == m_window && p->atom == m_timeProperty) take(p->time, &taking);
            break;
          }
          case XCB_SELECTION_REQUEST: answer((xcb_selection_request_event_t *)e); break;
          case XCB_SELECTION_CLEAR:
          {
            auto *sc = (xcb_selection_clear_event_t *)e;
            if (sc->selection == m_clipboard && sc->time >= m_time) lost = true; // another program copied something
            break;
          }
          default: break;
        }
        free(e);
      }
      if (xcb_connection_has_error(m_conn)) lost = true;
      if (lost)
      {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_asked == m_answered) break; // nobody waits to copy: done until the next copy
        m_owned = false;
      }
    }
    std::lock_guard<std::mutex> lock(m_mutex);
    xcb_disconnect(m_conn);
    m_conn = nullptr;
    close(m_wake[0]);
    close(m_wake[1]);
    m_wake[0] = m_wake[1] = -1;
    m_owned = false;
    m_running = false;
    m_cv.notify_all();
  }

  void take(xcb_timestamp_t time, unsigned long long *taking)
  {
    xcb_set_selection_owner(m_conn, m_window, m_clipboard, time);
    xcb_get_selection_owner_reply_t *r = xcb_get_selection_owner_reply(m_conn, xcb_get_selection_owner(m_conn, m_clipboard), nullptr);
    const bool owned = r && r->owner == m_window;
    free(r);
    std::lock_guard<std::mutex> lock(m_mutex);
    m_owned = owned;
    m_time = time;
    m_answered = *taking;
    *taking = 0;
    if (m_asked > m_answered)
    {
      // copied again meanwhile
      *taking = m_asked;
      xcb_change_property(m_conn, XCB_PROP_MODE_APPEND, m_window, m_timeProperty, XCB_ATOM_STRING, 8, 0, nullptr);
      xcb_flush(m_conn);
    }
    m_cv.notify_all();
  }

  // a program pastes: it asks for the text in a format (a target), into a property of its window
  void answer(const xcb_selection_request_event_t *e)
  {
    const xcb_atom_t property = e->property != XCB_ATOM_NONE ? e->property : e->target; // obsolete clients give none
    bool ok = false;
    std::string text;
    xcb_timestamp_t since = 0;
    bool owned = false;
    {
      std::lock_guard<std::mutex> lock(m_mutex);
      text = m_text;
      since = m_time;
      owned = m_owned;
    }
    if (e->selection == m_clipboard && owned && (e->time == XCB_CURRENT_TIME || e->time >= since))
    {
      if (e->target == m_targets)
      {
        const xcb_atom_t targets[] = { m_targets, m_timestamp, m_utf8, m_plainUtf8, m_plain, m_textAtom, XCB_ATOM_STRING };
        xcb_change_property(m_conn, XCB_PROP_MODE_REPLACE, e->requestor, property, XCB_ATOM_ATOM, 32,
                            sizeof(targets) / sizeof(targets[0]), targets);
        ok = true;
      }
      else if (e->target == m_timestamp)
      {
        const uint32_t t = since;
        xcb_change_property(m_conn, XCB_PROP_MODE_REPLACE, e->requestor, property, XCB_ATOM_INTEGER, 32, 1, &t);
        ok = true;
      }
      else if (e->target == m_utf8 || e->target == m_plainUtf8 || e->target == m_plain || e->target == m_textAtom ||
               e->target == XCB_ATOM_STRING)
      {
        // TEXT is answered as UTF-8; STRING is Latin-1, which an address (ASCII) is too
        const xcb_atom_t type = e->target == m_textAtom ? m_utf8 : e->target;
        xcb_change_property(m_conn, XCB_PROP_MODE_REPLACE, e->requestor, property, type, 8, (uint32_t)text.size(), text.data());
        ok = true;
      }
    }
    xcb_selection_notify_event_t n;
    memset(&n, 0, sizeof(n));
    n.response_type = XCB_SELECTION_NOTIFY;
    n.time = e->time;
    n.requestor = e->requestor;
    n.selection = e->selection;
    n.target = e->target;
    n.property = ok ? property : (xcb_atom_t)XCB_ATOM_NONE;
    char event[32]; // xcb_send_event() sends 32 bytes
    memset(event, 0, sizeof(event));
    memcpy(event, &n, sizeof(n));
    xcb_send_event(m_conn, 0, e->requestor, XCB_EVENT_MASK_NO_EVENT, event);
    xcb_flush(m_conn);
  }

  std::mutex m_mutex;
  std::condition_variable m_cv;
  std::thread m_thread;
  bool m_running = false, m_quit = false, m_owned = false;
  unsigned long long m_asked = 0, m_answered = 0;
  std::string m_text;
  xcb_timestamp_t m_time = 0;
  int m_wake[2] = { -1, -1 };

  xcb_connection_t *m_conn = nullptr;
  xcb_window_t m_window = 0;
  xcb_atom_t m_clipboard = 0, m_targets = 0, m_timestamp = 0, m_utf8 = 0, m_textAtom = 0, m_plainUtf8 = 0, m_plain = 0;
  xcb_atom_t m_timeProperty = 0;
};

// stopped (its thread joined) when the module is unloaded, before its code goes away
Owner g_owner;

} // namespace

bool setText(const std::string &text)
{
  return g_owner.set(text);
}

void stopX11()
{
  g_owner.stop();
}

bool readX11(std::string *text, double timeoutSeconds)
{
  xcb_connection_t *c = connect_x();
  if (!c) return false;
  const xcb_window_t w = make_window(c);
  const xcb_atom_t clipboard = intern(c, "CLIPBOARD"), utf8 = intern(c, "UTF8_STRING"), into = intern(c, "NINJAM_PASTE");
  xcb_convert_selection(c, w, clipboard, utf8, into, XCB_CURRENT_TIME);
  xcb_flush(c);

  bool ok = false, done = false;
  const auto end = std::chrono::steady_clock::now() + std::chrono::duration<double>(timeoutSeconds);
  while (!done && !xcb_connection_has_error(c))
  {
    while (xcb_generic_event_t *e = xcb_poll_for_event(c))
    {
      if ((e->response_type & ~0x80) == XCB_SELECTION_NOTIFY)
      {
        auto *n = (xcb_selection_notify_event_t *)e;
        done = true;
        if (n->property != XCB_ATOM_NONE)
        {
          xcb_get_property_reply_t *r = xcb_get_property_reply(c, xcb_get_property(c, 1, w, into, XCB_GET_PROPERTY_TYPE_ANY, 0, MAX_TEXT / 4 + 1), nullptr);
          if (r)
          {
            if (text) text->assign((const char *)xcb_get_property_value(r), xcb_get_property_value_length(r));
            ok = true;
            free(r);
          }
        }
      }
      free(e);
    }
    if (done) break;
    const int ms = (int)std::chrono::duration_cast<std::chrono::milliseconds>(end - std::chrono::steady_clock::now()).count();
    if (ms <= 0 || !wait_readable(c, -1, ms, nullptr)) break;
  }
  xcb_disconnect(c);
  return ok;
}

#else

bool setText(const std::string &text)
{
  return VSTGUI::CClipboard::setString(text.c_str());
}

#endif

} // namespace clipboard
} // namespace ninjam_vst3
