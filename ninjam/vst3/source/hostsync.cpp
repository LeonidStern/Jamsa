/*
    NINJAM VST3 - hostsync.cpp
    Copyright (C) 2026 and onward, NINJAM contributors

    NINJAM is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.
*/

#include "hostsync.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <algorithm>

namespace ninjam_vst3 {

// ---------------------------------------------------------------------------
// the bar math
// ---------------------------------------------------------------------------

namespace barmath {

double hostIntervalPos(double ppq, int bpi, double len)
{
  if (bpi <= 0 || len <= 0) return 0;
  double beat = fmod(ppq, (double)bpi);
  if (beat < 0) beat += bpi;
  const double pos = beat / bpi * len;
  return pos < len ? pos : 0.0;
}

double wrapSamples(double samples, double len)
{
  if (len <= 0) return 0;
  double w = fmod(samples + len / 2, len);
  if (w < 0) w += len;
  return w - len / 2;
}

int intervalEnd(int pos, int len, double target)
{
  double remaining = len - target; // the host's next interval starts then
  if (pos - target < -len / 2.0 || remaining < 1) remaining += len;
  return (int)std::max(1L, lround(remaining));
}

} // namespace barmath

// ---------------------------------------------------------------------------
// BarSync (audio thread)
// ---------------------------------------------------------------------------

void BarSync::reset()
{
  m_state = Off;
  m_errorMs = 0;
  m_wasPlaying = false;
  m_loopOffset = 0;
  m_fixedInterval = -1;
}

int BarSync::process(const HostPosition &host, int frames, double sampleRate, const Interval &iv)
{
  if (!iv.running || iv.len <= 0 || iv.bpi <= 0 || iv.bpm <= 0 || sampleRate <= 0)
  {
    m_state = Off;
    m_wasPlaying = false;
    return 0;
  }
  if (!host.hasPosition || host.tempo <= 0)
  {
    m_state = NoPosition;
    m_wasPlaying = false;
    return 0;
  }
  if (!host.playing)
  {
    m_state = Stopped; // the intervals play on as they are
    m_wasPlaying = false;
    return 0;
  }

  // the host started, or plays on, or looped, or its playhead moved
  bool moved = !m_wasPlaying;
  if (m_wasPlaying && fabs(host.ppq - m_expectedPpq) > SAME_PLACE)
  {
    // a loop: the last block went over the loop's end, and this one is back in the loop
    const bool looped = host.cycleActive && host.cycleEnd > host.cycleStart && m_expectedPpq >= host.cycleEnd - SAME_PLACE &&
                        m_expectedPpq - m_blockBeats <= host.cycleEnd + SAME_PLACE && host.ppq >= host.cycleStart - SAME_PLACE &&
                        host.ppq < host.cycleEnd;
    if (looped) m_loopOffset += m_expectedPpq - host.ppq; // the intervals play on as before the loop
    else moved = true;
  }
  if (moved)
  {
    m_loopOffset = 0;
    m_fixedInterval = -1; // line up again, even if this interval's end moved already
  }
  m_wasPlaying = true;
  m_blockBeats = frames / sampleRate * host.tempo / 60.0;
  m_expectedPpq = host.ppq + m_blockBeats;

  if (fabs(host.tempo - iv.bpm) > SAME_TEMPO)
  {
    m_state = OtherTempo;
    return 0;
  }

  const double target = barmath::hostIntervalPos(host.ppq + m_loopOffset, iv.bpi, iv.len);
  m_errorMs = barmath::wrapSamples(target - iv.pos, iv.len) / sampleRate * 1000.0;
  if (iv.count == m_fixedInterval)
  {
    m_state = Lining; // the next interval starts in line
    return 0;
  }
  if (fabs(m_errorMs) <= TOLERANCE_MS)
  {
    m_state = Aligned;
    return 0;
  }
  m_fixedInterval = iv.count;
  m_corrections++;
  m_state = Lining;
  return barmath::intervalEnd(iv.pos, iv.len, target);
}

// ---------------------------------------------------------------------------
// TempoFollower (network thread)
// ---------------------------------------------------------------------------

void TempoFollower::arm()
{
  m_armed = true;
}

int TempoFollower::update(double now, double hostTempo, int roomBpm)
{
  if (hostTempo <= 0) return 0;
  if (fabs(hostTempo - m_tempo) > SAME)
  {
    // the host's tempo changed (or is known now): a vote once it stays
    m_tempo = hostTempo;
    m_since = now;
    m_armed = true;
    return 0;
  }
  if (!m_armed || roomBpm <= 0 || now - m_since < QUIET) return 0;
  m_armed = false;
  const long n = lround(hostTempo);
  if (n == roomBpm || n < MIN_BPM || n > MAX_BPM) return 0;
  m_votes++;
  return (int)n;
}

// ---------------------------------------------------------------------------
// the editor's note
// ---------------------------------------------------------------------------

std::string bpmText(double bpm)
{
  char buf[32];
  if (fabs(bpm - lround(bpm)) < 0.005) snprintf(buf, sizeof(buf), "%ld", lround(bpm));
  else
  {
    snprintf(buf, sizeof(buf), "%.2f", bpm);
    char *e = buf + strlen(buf) - 1;
    while (*e == '0') *e-- = 0; // 109.60 -> 109.6
  }
  return buf;
}

std::string shortHostName(const std::string &hostName)
{
  size_t a = 0, b = hostName.size();
  while (a < b && (unsigned char)hostName[a] <= ' ') a++;
  while (b > a && (unsigned char)hostName[b - 1] <= ' ') b--;
  const std::string name = hostName.substr(a, b - a);
  std::string lower = name;
  for (auto &c : lower) c = (char)tolower((unsigned char)c);
  if (lower.find("ableton") != std::string::npos || lower == "live" || lower.compare(0, 5, "live ") == 0) return "Ableton";
  if (name.empty()) return "The DAW";
  size_t n = std::min<size_t>(name.size(), 32);
  while (n < name.size() && n > 0 && ((unsigned char)name[n] & 0xc0) == 0x80) n--; // not inside a UTF-8 character
  return name.substr(0, n);
}

std::string hostTempoNote(const std::string &hostName, double hostTempo, int roomBpm, bool following)
{
  if (hostTempo <= 0 || roomBpm <= 0 || fabs(hostTempo - roomBpm) <= BarSync::SAME_TEMPO) return "";
  const std::string host = shortHostName(hostName), room = std::to_string(roomBpm);
  std::string s = host + " is at " + bpmText(hostTempo) + " BPM, the room at " + room + ": set " +
                  (host == "Ableton" ? std::string("Ableton") : std::string("its tempo")) + " to " + room;
  if (!following) s += ", or turn on Follow Ableton's tempo";
  return s;
}

} // namespace ninjam_vst3
