/*
    NINJAM VST3 - views.h
    Copyright (C) 2026 and onward, NINJAM contributors

    NINJAM is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.
*/

/*

  The editor's own views, drawn in code (no bitmaps), in the look of Ableton
  Live's devices: grey strips, dark value boxes, flat toggles in Live's
  yellow, orange and blue, segmented meters.

  - Strip, StripContainer: a rectangle of one colour (strips, panes, separators)
  - FlatButton: a toggle or a push button
  - Led: a small round light
  - DragNumberBox: Live's tempo box: drag it up or down, or double-click and type
  - MenuBox: a value box with a menu (BPI, bitrate)
  - FaderBox: a horizontal volume box, filled to the level, with its dB
  - ArcKnob: a small knob with an arc (pan, click volume)
  - SegmentMeter: a 7-segment level meter
  - BeatCells: one cell per beat of the interval
  - AddressBox: an address and its Copy button
  - TextLinesView: wrapped scrolling text (chat, license)

*/

#ifndef _NINJAM_VST3_VIEWS_H_
#define _NINJAM_VST3_VIEWS_H_

#include <functional>
#include <string>
#include <vector>

#include "vstgui/vstgui.h"

namespace ninjam_vst3 {
namespace ui {

using namespace VSTGUI;

struct Colors
{
  static CColor window() { return CColor(0x2b, 0x2b, 0x2b); }
  static CColor strip() { return CColor(0x36, 0x36, 0x36); }
  static CColor pane() { return CColor(0x1f, 0x1f, 0x1f); }
  static CColor row() { return CColor(0x2a, 0x2a, 0x2a); }
  static CColor myRow() { return CColor(0x33, 0x30, 0x2a); }
  static CColor box() { return CColor(0x1c, 0x1c, 0x1c); }
  static CColor boxLine() { return CColor(0x12, 0x12, 0x12); }
  static CColor buttonOff() { return CColor(0x4a, 0x4a, 0x4a); }
  static CColor buttonText() { return CColor(0xd0, 0xd0, 0xd0); }
  static CColor separator() { return CColor(0x4a, 0x4a, 0x4a); }
  static CColor label() { return CColor(0x8c, 0x8c, 0x8c); }
  static CColor value() { return CColor(0xe6, 0xe6, 0xe6); }
  static CColor text() { return CColor(0xc9, 0xc9, 0xc9); }
  static CColor status() { return CColor(0x9a, 0x9a, 0x9a); }
  static CColor placeholder() { return CColor(0x6c, 0x6c, 0x6c); }
  static CColor dark() { return CColor(0x1c, 0x1c, 0x1c); } // text on a lit button
  static CColor yellow() { return CColor(0xff, 0xb5, 0x32); }
  static CColor yellowLine() { return CColor(0xc9, 0x8a, 0x1e); }
  static CColor orange() { return CColor(0xff, 0x7d, 0x3b); }
  static CColor orangeLine() { return CColor(0xc4, 0x5a, 0x26); }
  static CColor blue() { return CColor(0x6f, 0xb2, 0xee); }
  static CColor blueLine() { return CColor(0x4a, 0x86, 0xbd); }
  static CColor green() { return CColor(0x5f, 0xd1, 0x6a); }
  static CColor purple() { return CColor(0xb9, 0x9f, 0xf0); }
  static CColor red() { return CColor(0xe8, 0x5f, 0x55); }
  static CColor pastBeat() { return CColor(0x5a, 0x4a, 0x2a); }
  static CColor downbeat() { return CColor(0x26, 0x26, 0x26); }
  static CColor faderFill() { return CColor(0x6b, 0x6b, 0x6b); }
  static CColor meterOff() { return CColor(0x2a, 0x2a, 0x2a); }
  static CColor meterYellow() { return CColor(0xff, 0xd2, 0x3a); }
  static CColor knobRing() { return CColor(0x55, 0x55, 0x55); }
};

struct Fonts
{
  SharedPointer<CFontDesc> value, label, bold, small, tiny, button, chat, chatBold, mono, menu;
  void make(); // Arial (11 px values, 10 px labels, ...), a monospace font for addresses
};

// the width of text in a font, without a draw context
CCoord textWidth(CFontRef font, const std::string &text);

// a rectangle of one colour, with round corners if radius > 0
class Strip : public CView
{
public:
  Strip(const CRect &size, CColor color, CCoord radius = 0) : CView(size), m_color(color), m_radius(radius) {}
  void draw(CDrawContext *context) override;

private:
  CColor m_color;
  CCoord m_radius;
};

// a container that draws a rectangle of one colour behind its views
class StripContainer : public CViewContainer
{
public:
  StripContainer(const CRect &size, CColor color, CCoord radius = 2);
  void setColor(CColor c);
  void drawBackgroundRect(CDrawContext *context, const CRect &updateRect) override;

private:
  CColor m_color;
  CCoord m_radius;
};

// A flat button: a toggle (lit in its on colours when on) or a push button (lit while pressed).
class FlatButton : public CControl
{
public:
  enum Kind { Toggle, Push };
  FlatButton(const CRect &size, IControlListener *listener, int32_t tag, const std::string &title, Kind kind, CFontRef font);
  void setTitle(const std::string &title);
  const std::string &title() const { return m_title; }
  void setOnColors(CColor fill, CColor line) { m_onFill = fill; m_onLine = line; invalid(); }
  void setEnabled(bool on);
  bool enabled() const { return m_enabled; }
  CCoord preferredWidth() const;
  void draw(CDrawContext *context) override;
  CMouseEventResult onMouseDown(CPoint &where, const CButtonState &buttons) override;
  CMouseEventResult onMouseUp(CPoint &where, const CButtonState &buttons) override;
  CMouseEventResult onMouseMoved(CPoint &where, const CButtonState &buttons) override;
  CMouseEventResult onMouseCancel() override;
  CLASS_METHODS(FlatButton, CControl)

private:
  std::string m_title;
  Kind m_kind;
  SharedPointer<CFontDesc> m_font;
  CColor m_onFill, m_onLine;
  bool m_pressed = false, m_enabled = true;
};

// a small round light
class Led : public CView
{
public:
  explicit Led(const CRect &size) : CView(size) {}
  void setColor(CColor c, bool lit); // lit: with a glow
  void draw(CDrawContext *context) override;

private:
  CColor m_color = Colors::buttonOff();
  bool m_lit = false;
};

// Live's tempo box: drag it up or down for a value (valueChanged() when the mouse goes up),
// or double-click and type one (valueChanged() when Return is pressed). The editor sets what
// it shows.
class DragNumberBox : public CTextEdit
{
public:
  DragNumberBox(const CRect &size, IControlListener *listener, int32_t tag, CFontRef font);
  // the value a drag starts from, and the text the box shows while not dragged or typed in
  void setShown(int value, const std::string &text);
  void setEnabled(bool on);
  bool busy() const { return m_dragging || m_typing; } // being dragged or typed in
  // in valueChanged(): true for a drag (draggedValue()), false for typed text (getText())
  bool committedByDrag() const { return m_dragCommitted; }
  int draggedValue() const { return m_dragValue; }
  std::function<int(int start, double dyUp, bool fine)> dragMap; // the value after a drag of dyUp pixels
  std::function<std::string(int)> format;                       // a value as the box shows it

  CMouseEventResult onMouseDown(CPoint &where, const CButtonState &buttons) override;
  CMouseEventResult onMouseMoved(CPoint &where, const CButtonState &buttons) override;
  CMouseEventResult onMouseUp(CPoint &where, const CButtonState &buttons) override;
  CMouseEventResult onMouseCancel() override;
  void takeFocus() override;
  void looseFocus() override;
  CLASS_METHODS(DragNumberBox, CTextEdit)

private:
  int m_shown = 0;
  std::string m_shownText;
  bool m_enabled = true, m_dragging = false, m_dragCommitted = false, m_typing = false;
  CCoord m_startY = 0;
  int m_dragValue = 0;
};

// a value box that opens a menu of choices ("16 ▾")
class MenuBox : public COptionMenu
{
public:
  MenuBox(const CRect &size, IControlListener *listener, int32_t tag, CFontRef font);
  void setShown(const std::string &text); // what the box says (the room's value may not be one of the choices)
  void setEnabled(bool on);
  void draw(CDrawContext *context) override;
  CMouseEventResult onMouseDown(CPoint &where, const CButtonState &buttons) override;
  CLASS_METHODS(MenuBox, COptionMenu)

private:
  std::string m_shown;
  SharedPointer<CFontDesc> m_boxFont;
  bool m_enabled = true;
};

// shared by FaderBox and ArcKnob: drag to change the value (0..1), double-click for the default,
// the mouse wheel in small steps, Shift for fine steps
class DragControl : public CControl
{
public:
  DragControl(const CRect &size, IControlListener *listener, int32_t tag, bool vertical, CCoord pixelsForRange);
  CMouseEventResult onMouseDown(CPoint &where, const CButtonState &buttons) override;
  CMouseEventResult onMouseMoved(CPoint &where, const CButtonState &buttons) override;
  CMouseEventResult onMouseUp(CPoint &where, const CButtonState &buttons) override;
  CMouseEventResult onMouseCancel() override;
  void onMouseWheelEvent(MouseWheelEvent &event) override;

protected:
  bool m_vertical;
  CCoord m_range; // pixels for the whole range (0: the view's width)
  CPoint m_start;
  float m_startValue = 0;
};

// a horizontal volume box: grey up to the level, the value (dB) at the right
class FaderBox : public DragControl
{
public:
  FaderBox(const CRect &size, IControlListener *listener, int32_t tag, CFontRef font);
  std::function<std::string(float)> format; // the value as text
  void draw(CDrawContext *context) override;
  CLASS_METHODS(FaderBox, CControl)

private:
  SharedPointer<CFontDesc> m_font;
};

// a small knob: a dark disc in a grey ring, with a yellow arc from the top (bipolar: pan) or
// from the start of its travel (a volume)
class ArcKnob : public DragControl
{
public:
  ArcKnob(const CRect &size, IControlListener *listener, int32_t tag, bool bipolar);
  void draw(CDrawContext *context) override;
  CLASS_METHODS(ArcKnob, CControl)

private:
  bool m_bipolar;
};

// a meter of 7 segments, left to right: five green, two yellow
class SegmentMeter : public CView
{
public:
  explicit SegmentMeter(const CRect &size) : CView(size) {}
  void setPeak(float peak); // linear
  void draw(CDrawContext *context) override;

private:
  int m_lit = 0;
};

// one cell per beat of the interval: the current one yellow, the ones before it brown, the
// first of each four a little lighter
class BeatCells : public CView
{
public:
  explicit BeatCells(const CRect &size) : CView(size) {}
  void setPosition(int pos, int length, int bpi); // bpi 0: empty cells
  void draw(CDrawContext *context) override;

private:
  int m_beat = -1, m_bpi = 0;
};

// "LAN 192.168.1.20:2049 [Copy]": a box with a label, an address (monospace) and a Copy button;
// valueChanged() when Copy is clicked
class AddressBox : public CControl
{
public:
  AddressBox(const CRect &size, IControlListener *listener, int32_t tag, const Fonts &fonts, const std::string &key);
  void setAddress(const std::string &address, const std::string &placeholder); // the placeholder when there is none
  const std::string &address() const { return m_address; }
  void setCopyTitle(const std::string &title); // "Copy", "Copied"
  CCoord preferredWidth() const;
  void draw(CDrawContext *context) override;
  CMouseEventResult onMouseDown(CPoint &where, const CButtonState &buttons) override;
  CMouseEventResult onMouseUp(CPoint &where, const CButtonState &buttons) override;
  CMouseEventResult onMouseCancel() override;
  CLASS_METHODS(AddressBox, CControl)

private:
  CRect copyRect() const;
  SharedPointer<CFontDesc> m_keyFont, m_monoFont, m_buttonFont;
  std::string m_key, m_address, m_placeholder, m_copyTitle = "Copy";
  bool m_pressed = false;
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
  void setBackground(CColor c) { m_background = c; }
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
  CColor m_background = Colors::pane();
  bool m_wrapped = false, m_startAtTop = false;
  CCoord m_wrapWidth = -1;
  int m_scroll = 0; // visual lines away from the newest (or from the top, with startAtTop)
};

CTextLabel *makeLabel(const CRect &size, const std::string &text, CFontRef font, CColor color, CHoriTxtAlign align = kLeftText);
// a text field in a value box
CTextEdit *makeEdit(const CRect &size, IControlListener *listener, int32_t tag, CFontRef font, const char *placeholder);
// the look of the menus VSTGUI draws itself (on Linux)
void styleMenus(CFrame *frame, CFontRef font);

// volume control position (0..1) <-> gain (0..2, 1 = 0 dB at 0.707)
inline float sliderToGain(float v) { return 2.0f * v * v; }
float gainToSlider(float gain);

} // namespace ui
} // namespace ninjam_vst3

#endif
