/*
    NINJAM VST3 - textkeys.cpp
    Copyright (C) 2026 and onward, NINJAM contributors

    NINJAM is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.
*/

#include "textkeys.h"

namespace ninjam_vst3 {
namespace textkeys {

Command commandFor(char32_t letter, bool primary, bool shift, bool alt, bool other)
{
  if (!primary || shift || alt || other) return Command::None;
  switch (letter)
  {
    case 'x': return Command::Cut;
    case 'c': return Command::Copy;
    case 'v': return Command::Paste;
    case 'a': return Command::SelectAll;
    default: return Command::None;
  }
}

char32_t letterOf(char32_t c)
{
  if (c >= 'a' && c <= 'z') return c;
  if (c >= 'A' && c <= 'Z') return c - 'A' + 'a';
  if (c >= 1 && c <= 26) return c - 1 + 'a'; // Ctrl+A is 1 ... Ctrl+Z is 26
  return 0;
}

char32_t latinLetter(std::initializer_list<char32_t> typed, char32_t usLetter)
{
  for (char32_t c : typed)
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) return letterOf(c);
  return usLetter;
}

char32_t macUsLetter(unsigned short keyCode)
{
  // Carbon's kVK_ANSI_A, kVK_ANSI_X, kVK_ANSI_C, kVK_ANSI_V
  switch (keyCode)
  {
    case 0x00: return 'a';
    case 0x07: return 'x';
    case 0x08: return 'c';
    case 0x09: return 'v';
    default: return 0;
  }
}

std::string oneLine(const std::string &text)
{
  const bool lines = text.find_first_of("\r\n") != std::string::npos;
  std::string out, line;
  auto endLine = [&] {
    if (lines)
    {
      const size_t b = line.find_first_not_of(' ');
      if (b == std::string::npos)
      {
        line.clear();
        return;
      }
      line = line.substr(b, line.find_last_not_of(' ') - b + 1);
      if (!out.empty()) out += ' ';
    }
    out += line;
    line.clear();
  };
  for (size_t i = 0; i < text.size(); i++)
  {
    const unsigned char c = (unsigned char)text[i];
    if (c == '\r' || c == '\n')
    {
      if (c == '\r' && i + 1 < text.size() && text[i + 1] == '\n') i++;
      endLine();
    }
    else if (c == '\t') line += ' ';
    else if (c >= 0x20 && c != 0x7f) line += (char)c;
  }
  endLine();
  return out;
}

} // namespace textkeys
} // namespace ninjam_vst3
