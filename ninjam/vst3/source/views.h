/*
    NINJAM VST3 - views.h
    Copyright (C) 2026 and onward, NINJAM contributors

    NINJAM is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.
*/

/*

  The editor's own views, drawn in code (no bitmaps): level meters, the
  interval progress bar, wrapped scrolling text (chat, license) and panels.

*/

#ifndef _NINJAM_VST3_VIEWS_H_
#define _NINJAM_VST3_VIEWS_H_

#include <string>
#include <vector>

#include "vstgui/vstgui.h"

namespace ninjam_vst3 {
namespace ui {

using namespace VSTGUI;

struct Colors
{
  static CColor background() { return CColor(29, 32, 37); }
  static CColor panel() { return CColor(38, 42, 49); }
  static CColor panelLine() { return CColor(58, 64, 74); }
  static CColor field() { return CColor(21, 23, 27); }
  static CColor text() { return CColor(232, 232, 232); }
  static CColor dim() { return CColor(154, 163, 173); }
  static CColor accent() { return CColor(63, 178, 127); }
  static CColor blue() { return CColor(90, 160, 230); }
  static CColor warn() { return CColor(224, 160, 48); }
  static CColor error() { return CColor(235, 95, 95); }
  static CColor server() { return CColor(190, 160, 255); }
  static CColor topic() { return CColor(127, 200, 255); }
};

// a stereo level meter: two bars, -60 dB to +6 dB
class MeterView : public CView
{
public:
  explicit MeterView(const CRect &size) : CView(size) {}
  void setLevels(float left, float right); // linear peak levels
  void draw(CDrawContext *context) override;

private:
  float m_level[2] = { 0, 0 };
};

// the interval: one cell per beat, filled up to the current position
class IntervalView : public CView
{
public:
  IntervalView(const CRect &size, CFontRef font) : CView(size), m_font(font) {}
  void setPosition(int pos, int length, int bpi);
  void setMessage(const std::string &text); // shown instead of the beats when not empty
  void draw(CDrawContext *context) override;

private:
  SharedPointer<CFontDesc> m_font;
  int m_pos = 0, m_length = 0, m_bpi = 0;
  std::string m_message;
};

// lines of text, wrapped to the width, newest at the bottom; the mouse wheel scrolls back
class TextLinesView : public CView
{
public:
  struct Line
  {
    std::string who; // drawn in whoColor before the text, if not empty
    std::string text;
    CColor whoColor, textColor;
  };

  TextLinesView(const CRect &size, CFontRef font, CFontRef boldFont);
  void setLines(const std::vector<Line> &lines);
  void setStartAtTop(bool top) { m_startAtTop = top; } // for a document rather than a log
  void scrollToStart() { m_scroll = 0; invalid(); }
  void draw(CDrawContext *context) override;
  void onMouseWheelEvent(MouseWheelEvent &event) override;
  void setViewSize(const CRect &rect, bool invalid = true) override;

private:
  struct Visual
  {
    std::string who, text;
    CColor whoColor, textColor;
    CCoord whoWidth = 0;
  };
  void wrap(CDrawContext *context);

  SharedPointer<CFontDesc> m_font, m_bold;
  std::vector<Line> m_lines;
  std::vector<Visual> m_visual;
  bool m_wrapped = false, m_startAtTop = false;
  CCoord m_wrapWidth = -1;
  int m_scroll = 0; // visual lines away from the newest (or from the top, with startAtTop)
};

// a container with a rounded background
class PanelView : public CViewContainer
{
public:
  PanelView(const CRect &size, CColor fill, CColor line, CCoord radius)
    : CViewContainer(size), m_fill(fill), m_line(line), m_radius(radius)
  {
    setBackgroundColor(kTransparentCColor);
  }
  void drawBackgroundRect(CDrawContext *context, const CRect &updateRect) override;

private:
  CColor m_fill, m_line;
  CCoord m_radius;
};

// a slider without bitmaps, in the editor's colours
CSlider *makeSlider(const CRect &size, IControlListener *listener, int32_t tag, bool fromCenter);
// a push or on/off button in the editor's colours
CTextButton *makeButton(const CRect &size, IControlListener *listener, int32_t tag, const char *title, bool onOff, CFontRef font);
CTextLabel *makeLabel(const CRect &size, const char *text, CFontRef font, CColor color, CHoriTxtAlign align = kLeftText);
CTextEdit *makeEdit(const CRect &size, IControlListener *listener, int32_t tag, const char *text, CFontRef font, const char *placeholder);

// volume slider position (0..1) <-> gain (0..2, 1 = 0 dB at 0.707)
inline float sliderToGain(float v) { return 2.0f * v * v; }
float gainToSlider(float gain);

} // namespace ui
} // namespace ninjam_vst3

#endif
