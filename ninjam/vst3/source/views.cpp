/*
    NINJAM VST3 - views.cpp
    Copyright (C) 2026 and onward, NINJAM contributors

    NINJAM is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.
*/

#include "views.h"

#include <math.h>
#include <algorithm>

#include "vstgui/lib/platform/common/genericoptionmenu.h"
#include "vstgui/lib/platform/iplatformfont.h"
#include "vstgui/lib/platform/iplatformframe.h"

#include "textkeys.h"
#include "uitext.h"

namespace ninjam_vst3 {
namespace ui {

static const CCoord RADIUS = 2;

float gainToSlider(float gain)
{
  if (!(gain > 0.0f)) return 0.0f;
  return std::min(1.0f, sqrtf(gain * 0.5f));
}

void Fonts::make()
{
  const char *sans = "Arial";
#if defined(__APPLE__)
  const char *monoFace = "Menlo";
#elif defined(_WIN32)
  const char *monoFace = "Consolas";
#else
  const char *monoFace = "monospace";
#endif
  value = makeOwned<CFontDesc>(sans, 11);
  label = makeOwned<CFontDesc>(sans, 10);
  bold = makeOwned<CFontDesc>(sans, 11, kBoldFace);
  small = makeOwned<CFontDesc>(sans, 9.5);
  tiny = makeOwned<CFontDesc>(sans, 9);
  button = makeOwned<CFontDesc>(sans, 10.5);
  chat = makeOwned<CFontDesc>(sans, 10.5);
  chatBold = makeOwned<CFontDesc>(sans, 10.5, kBoldFace);
  mono = makeOwned<CFontDesc>(monoFace, 10.5);
  menu = makeOwned<CFontDesc>(sans, 11);
}

CCoord textWidth(CFontRef font, const std::string &text)
{
  if (!font || text.empty()) return 0;
  if (auto platformFont = font->getPlatformFont())
    if (auto painter = platformFont->getPainter())
    {
      const CCoord w = painter->getStringWidth(nullptr, UTF8String(text.c_str()).getPlatformString(), true);
      if (w > 0) return ceil(w);
    }
  return ceil(text.size() * font->getSize() * 0.56); // a guess, if the platform can't say
}

static void fillRect(CDrawContext *context, const CRect &r, CCoord radius, const CColor &fill, const CColor *line = nullptr)
{
  if (radius > 0)
  {
    if (SharedPointer<CGraphicsPath> path = owned(context->createRoundRectGraphicsPath(r, radius)))
    {
      context->setFillColor(fill);
      context->drawGraphicsPath(path, CDrawContext::kPathFilled);
      if (line)
      {
        SharedPointer<CGraphicsPath> edge = owned(context->createRoundRectGraphicsPath(CRect(r).inset(0.5, 0.5), radius));
        context->setFrameColor(*line);
        context->setLineWidth(1);
        if (edge) context->drawGraphicsPath(edge, CDrawContext::kPathStroked);
      }
      return;
    }
  }
  context->setFillColor(fill);
  context->drawRect(r, kDrawFilled);
  if (line)
  {
    context->setFrameColor(*line);
    context->setLineWidth(1);
    context->drawRect(CRect(r).inset(0.5, 0.5), kDrawStroked);
  }
}

// a value box: #1c1c1c with a #121212 edge
static void drawBox(CDrawContext *context, const CRect &r)
{
  const CColor line = Colors::boxLine();
  fillRect(context, r, RADIUS, Colors::box(), &line);
}

static CColor withAlpha(CColor c, float a)
{
  c.alpha = (uint8_t)(c.alpha * a);
  return c;
}

// ---------------------------------------------------------------------------

void Strip::draw(CDrawContext *context)
{
  context->setDrawMode(kAntiAliasing);
  fillRect(context, getViewSize(), m_radius, m_color);
  setDirty(false);
}

StripContainer::StripContainer(const CRect &size, CColor color, CCoord radius)
  : CViewContainer(size), m_color(color), m_radius(radius)
{
  setBackgroundColor(kTransparentCColor);
}

void StripContainer::setColor(CColor c)
{
  if (c == m_color) return;
  m_color = c;
  invalid();
}

void StripContainer::drawBackgroundRect(CDrawContext *context, const CRect &)
{
  // in the container's own coordinates
  context->setDrawMode(kAntiAliasing);
  fillRect(context, CRect(0, 0, getViewSize().getWidth(), getViewSize().getHeight()), m_radius, m_color);
}

// ---------------------------------------------------------------------------

FlatButton::FlatButton(const CRect &size, IControlListener *listener, int32_t tag, const std::string &title, Kind kind, CFontRef font)
  : CControl(size, listener, tag), m_title(title), m_kind(kind), m_font(font), m_onFill(Colors::yellow()), m_onLine(Colors::yellowLine())
{
  setMin(0);
  setMax(1);
}

void FlatButton::setTitle(const std::string &title)
{
  if (title == m_title) return;
  m_title = title;
  invalid();
}

void FlatButton::setEnabled(bool on)
{
  if (on == m_enabled) return;
  m_enabled = on;
  setMouseEnabled(on);
  invalid();
}

CCoord FlatButton::preferredWidth() const
{
  return textWidth(m_font, m_title) + 12;
}

void FlatButton::draw(CDrawContext *context)
{
  const CRect r = getViewSize();
  const bool lit = m_kind == Toggle ? getValue() > 0.5f : m_pressed;
  context->setDrawMode(kAntiAliasing);
  CColor fill = lit ? (m_kind == Toggle ? m_onFill : CColor(0x5e, 0x5e, 0x5e)) : Colors::buttonOff();
  CColor line = lit && m_kind == Toggle ? m_onLine : Colors::box();
  CColor text = lit && m_kind == Toggle ? Colors::dark() : Colors::buttonText();
  if (!m_enabled)
  {
    fill = withAlpha(fill, 0.5f);
    text = withAlpha(text, 0.5f);
  }
  fillRect(context, r, RADIUS, fill, &line);
  context->setFont(m_font);
  context->setFontColor(text);
  context->drawString(m_title.c_str(), r, kCenterText);
  setDirty(false);
}

CMouseEventResult FlatButton::onMouseDown(CPoint &, const CButtonState &buttons)
{
  if (!(buttons & kLButton) || !m_enabled) return kMouseEventNotHandled;
  if (m_kind == Toggle)
  {
    // a toggle switches when pressed, as Live's do
    beginEdit();
    setValue(getValue() > 0.5f ? 0.0f : 1.0f);
    valueChanged();
    endEdit();
    invalid();
    return kMouseDownEventHandledButDontNeedMovedOrUpEvents;
  }
  m_pressed = true;
  invalid();
  return kMouseEventHandled;
}

CMouseEventResult FlatButton::onMouseMoved(CPoint &where, const CButtonState &buttons)
{
  if (m_kind == Push && (buttons & kLButton))
  {
    const bool inside = getViewSize().pointInside(where);
    if (inside != m_pressed)
    {
      m_pressed = inside;
      invalid();
    }
  }
  return kMouseEventHandled;
}

CMouseEventResult FlatButton::onMouseUp(CPoint &where, const CButtonState &)
{
  if (m_kind != Push) return kMouseEventHandled;
  const bool clicked = m_pressed && getViewSize().pointInside(where);
  m_pressed = false;
  invalid();
  if (clicked)
  {
    // a push: 1, then back to 0
    beginEdit();
    setValue(1.0f);
    valueChanged();
    setValue(0.0f);
    valueChanged();
    endEdit();
  }
  return kMouseEventHandled;
}

CMouseEventResult FlatButton::onMouseCancel()
{
  m_pressed = false;
  invalid();
  return kMouseEventHandled;
}

// ---------------------------------------------------------------------------

void Led::setColor(CColor c, bool lit)
{
  if (c == m_color && lit == m_lit) return;
  m_color = c;
  m_lit = lit;
  invalid();
}

void Led::draw(CDrawContext *context)
{
  const CRect r = getViewSize();
  const CCoord d = 7;
  const CPoint c = r.getCenter();
  context->setDrawMode(kAntiAliasing);
  if (m_lit)
  {
    // the glow
    for (int i = 3; i >= 1; i --)
    {
      context->setFillColor(withAlpha(m_color, 0.12f));
      context->drawEllipse(CRect(c.x - d / 2 - i, c.y - d / 2 - i, c.x + d / 2 + i, c.y + d / 2 + i), kDrawFilled);
    }
  }
  context->setFillColor(m_color);
  context->drawEllipse(CRect(c.x - d / 2, c.y - d / 2, c.x + d / 2, c.y + d / 2), kDrawFilled);
  setDirty(false);
}

// ---------------------------------------------------------------------------

DragNumberBox::DragNumberBox(const CRect &size, IControlListener *listener, int32_t tag, CFontRef font)
  : CTextEdit(size, listener, tag, "", nullptr, kDoubleClickStyle)
{
  setFont(font);
  setFontColor(Colors::value());
  setBackColor(Colors::box());
  setFrameColor(Colors::boxLine());
  setStyle(getStyle() | kRoundRectStyle);
  setRoundRectRadius(RADIUS);
  setHoriAlign(kCenterText);
  setTextInset(CPoint(3, 0));
}

void DragNumberBox::setShown(int value, const std::string &text)
{
  m_shown = value;
  m_shownText = text;
  if (busy()) return;
  if (getText() != text.c_str())
  {
    setText(text.c_str());
    invalid();
  }
}

void DragNumberBox::setEnabled(bool on)
{
  if (on == m_enabled) return;
  m_enabled = on;
  setMouseEnabled(on);
  setAlphaValue(on ? 1.0f : 0.5f);
  if (!on && m_dragging) onMouseCancel();
}

CMouseEventResult DragNumberBox::onMouseDown(CPoint &where, const CButtonState &buttons)
{
  if (!(buttons & kLButton) || !m_enabled) return kMouseEventNotHandled;
  if (buttons.isDoubleClick() || (getFrame() && getFrame()->getFocusView() == this))
    return CTextEdit::onMouseDown(where, buttons); // type a value
  m_dragging = true;
  m_dragCommitted = false;
  m_startY = where.y;
  m_dragValue = m_shown;
  return kMouseEventHandled;
}

CMouseEventResult DragNumberBox::onMouseMoved(CPoint &where, const CButtonState &buttons)
{
  if (!m_dragging) return kMouseEventNotHandled;
  const int v = dragMap ? dragMap(m_shown, m_startY - where.y, (buttons & kShift) != 0) : m_shown;
  if (v != m_dragValue)
  {
    m_dragValue = v;
    setText(format ? format(v).c_str() : std::to_string(v).c_str());
    setFontColor(Colors::yellow()); // a value not voted for yet
    invalid();
  }
  return kMouseEventHandled;
}

CMouseEventResult DragNumberBox::onMouseUp(CPoint &, const CButtonState &)
{
  if (!m_dragging) return kMouseEventNotHandled;
  m_dragging = false;
  setFontColor(Colors::value());
  if (m_dragValue != m_shown)
  {
    m_dragCommitted = true;
    beginEdit();
    valueChanged();
    endEdit();
    m_dragCommitted = false;
  }
  // until the room says otherwise, the box shows what it says now
  setText(m_shownText.c_str());
  invalid();
  return kMouseEventHandled;
}

CMouseEventResult DragNumberBox::onMouseCancel()
{
  if (m_dragging)
  {
    m_dragging = false;
    setFontColor(Colors::value());
    setText(m_shownText.c_str());
    invalid();
  }
  return kMouseEventHandled;
}

void DragNumberBox::takeFocus()
{
  m_typing = true;
  CTextEdit::takeFocus();
}

void DragNumberBox::looseFocus()
{
  CTextEdit::looseFocus(); // valueChanged() if the text changed
  m_typing = false;
  setText(m_shownText.c_str());
  invalid();
}

// ---------------------------------------------------------------------------

MenuBox::MenuBox(const CRect &size, IControlListener *listener, int32_t tag, CFontRef font)
  : COptionMenu(size, listener, tag, nullptr, nullptr, kCheckStyle), m_boxFont(font)
{
  setFont(font);
  setFontColor(Colors::value());
  setBackColor(Colors::box());
  setFrameColor(Colors::boxLine());
}

void MenuBox::setShown(const std::string &text)
{
  if (text == m_shown) return;
  m_shown = text;
  invalid();
}

void MenuBox::setEnabled(bool on)
{
  if (on == m_enabled) return;
  m_enabled = on;
  setMouseEnabled(on);
  invalid();
}

void MenuBox::draw(CDrawContext *context)
{
  const CRect r = getViewSize();
  context->setDrawMode(kAntiAliasing);
  drawBox(context, r);
  const float a = m_enabled ? 1.0f : 0.5f;
  // the text, then a small triangle
  const CCoord arrowW = 7;
  const CCoord tw = textWidth(m_boxFont, m_shown);
  const CCoord total = tw + 3 + arrowW;
  CCoord x = r.left + std::max<CCoord>(4, floor((r.getWidth() - total) / 2));
  context->setFont(m_boxFont);
  context->setFontColor(withAlpha(Colors::value(), a));
  context->drawString(m_shown.c_str(), CRect(x, r.top, r.right - arrowW - 2, r.bottom), kLeftText);
  x = std::min(x + tw + 3, r.right - arrowW - 3);
  const CCoord cy = floor(r.getCenter().y) + 0.5;
  if (SharedPointer<CGraphicsPath> p = owned(context->createGraphicsPath()))
  {
    p->beginSubpath(CPoint(x, cy - 2));
    p->addLine(CPoint(x + arrowW - 1, cy - 2));
    p->addLine(CPoint(x + (arrowW - 1) / 2, cy + 2));
    p->closeSubpath();
    context->setFillColor(withAlpha(Colors::label(), a));
    context->drawGraphicsPath(p, CDrawContext::kPathFilled);
  }
  setDirty(false);
}

CMouseEventResult MenuBox::onMouseDown(CPoint &where, const CButtonState &buttons)
{
  if (!m_enabled) return kMouseEventNotHandled;
  return COptionMenu::onMouseDown(where, buttons);
}

// ---------------------------------------------------------------------------

DragControl::DragControl(const CRect &size, IControlListener *listener, int32_t tag, bool vertical, CCoord pixelsForRange)
  : CControl(size, listener, tag), m_vertical(vertical), m_range(pixelsForRange)
{
  setMin(0);
  setMax(1);
  setWheelInc(0.02f);
}

CMouseEventResult DragControl::onMouseDown(CPoint &where, const CButtonState &buttons)
{
  if (!(buttons & kLButton)) return kMouseEventNotHandled;
  if (buttons.isDoubleClick())
  {
    // back to the default (0 dB, the middle)
    beginEdit();
    setValue(getDefaultValue());
    valueChanged();
    endEdit();
    invalid();
    return kMouseDownEventHandledButDontNeedMovedOrUpEvents;
  }
  m_start = where;
  m_startValue = getValue();
  beginEdit();
  return kMouseEventHandled;
}

CMouseEventResult DragControl::onMouseMoved(CPoint &where, const CButtonState &buttons)
{
  if (!isEditing() || !(buttons & kLButton)) return kMouseEventNotHandled;
  const CCoord range = m_range > 0 ? m_range : getViewSize().getWidth();
  CCoord d = m_vertical ? m_start.y - where.y : where.x - m_start.x;
  if (buttons & kShift) d /= 8; // fine
  const float v = std::min(1.0f, std::max(0.0f, m_startValue + (float)(d / range)));
  if (v != getValue())
  {
    setValue(v);
    valueChanged();
    invalid();
  }
  return kMouseEventHandled;
}

CMouseEventResult DragControl::onMouseUp(CPoint &, const CButtonState &)
{
  if (isEditing()) endEdit();
  return kMouseEventHandled;
}

CMouseEventResult DragControl::onMouseCancel()
{
  if (isEditing())
  {
    setValue(m_startValue);
    valueChanged();
    invalid();
    endEdit();
  }
  return kMouseEventHandled;
}

void DragControl::onMouseWheelEvent(MouseWheelEvent &event)
{
  double d = m_vertical ? event.deltaY : (event.deltaY != 0 ? event.deltaY : -event.deltaX);
  if (d == 0) return;
  if (event.flags & MouseWheelEvent::DirectionInvertedFromDevice) d = -d;
  const float step = getWheelInc() * ((event.modifiers.has(ModifierKey::Shift)) ? 0.1f : 1.0f);
  const float v = std::min(1.0f, std::max(0.0f, getValue() + (float)(d > 0 ? step : -step)));
  if (v != getValue())
  {
    beginEdit();
    setValue(v);
    valueChanged();
    endEdit();
    invalid();
  }
  event.consumed = true;
}

// ---------------------------------------------------------------------------

FaderBox::FaderBox(const CRect &size, IControlListener *listener, int32_t tag, CFontRef font)
  : DragControl(size, listener, tag, false, 0), m_font(font)
{
}

void FaderBox::draw(CDrawContext *context)
{
  const CRect r = getViewSize();
  context->setDrawMode(kAntiAliasing);
  drawBox(context, r);
  CRect fill(r);
  fill.inset(1, 1);
  fill.right = fill.left + floor(fill.getWidth() * std::min(1.0f, std::max(0.0f, getValue())) + 0.5);
  if (fill.right > fill.left)
  {
    context->setFillColor(Colors::faderFill());
    context->drawRect(fill, kDrawFilled);
  }
  if (format)
  {
    context->setFont(m_font);
    context->setFontColor(Colors::buttonText());
    CRect t(r);
    t.right -= 3;
    context->drawString(format(getValue()).c_str(), t, kRightText);
  }
  setDirty(false);
}

// ---------------------------------------------------------------------------

ArcKnob::ArcKnob(const CRect &size, IControlListener *listener, int32_t tag, bool bipolar)
  : DragControl(size, listener, tag, true, 100), m_bipolar(bipolar)
{
  setDefaultValue(bipolar ? 0.5f : gainToSlider(1.0f));
}

void ArcKnob::draw(CDrawContext *context)
{
  const CRect r = getViewSize();
  const CCoord d = std::min(r.getWidth(), r.getHeight());
  const CPoint c = r.getCenter();
  const CRect disc(c.x - d / 2, c.y - d / 2, c.x + d / 2, c.y + d / 2);
  context->setDrawMode(kAntiAliasing);
  context->setFillColor(Colors::box());
  context->drawEllipse(disc, kDrawFilled);

  // the ring, 2 px wide, 2 px in; then the value's arc over it
  CRect ring(disc);
  ring.inset(3, 3);
  context->setLineWidth(2);
  context->setLineStyle(CLineStyle(CLineStyle::kLineCapButt));
  context->setFrameColor(Colors::knobRing());
  context->drawEllipse(ring, kDrawStroked);

  const double v = std::min(1.0f, std::max(0.0f, getValue()));
  double from, to;
  if (m_bipolar)
  {
    from = 270; // the top
    to = 270 + (v - 0.5) * 2 * 135;
  }
  else
  {
    from = 135; // the start of its travel, bottom left
    to = 135 + v * 270;
  }
  if (fabs(to - from) > 0.5)
    if (SharedPointer<CGraphicsPath> p = owned(context->createGraphicsPath()))
    {
      p->addArc(ring, std::min(from, to), std::max(from, to), true);
      context->setFrameColor(Colors::yellow());
      context->drawGraphicsPath(p, CDrawContext::kPathStroked);
    }
  // a tick at the value
  const double a = to * 3.14159265358979 / 180.0;
  const CCoord rr = ring.getWidth() / 2;
  context->setLineWidth(1.5);
  context->setFrameColor(Colors::value());
  context->drawLine(CPoint(c.x + cos(a) * (rr - 3), c.y + sin(a) * (rr - 3)), CPoint(c.x + cos(a) * (rr + 1), c.y + sin(a) * (rr + 1)));
  setDirty(false);
}

// ---------------------------------------------------------------------------

void SegmentMeter::setPeak(float peak)
{
  const int n = uitext::litSegments(peak);
  if (n == m_lit) return;
  m_lit = n;
  invalid();
}

void SegmentMeter::draw(CDrawContext *context)
{
  const CRect r = getViewSize();
  const CCoord h = std::min<CCoord>(8, r.getHeight());
  const CRect m(r.left, floor(r.getCenter().y - h / 2), r.right, floor(r.getCenter().y - h / 2) + h);
  context->setFillColor(Colors::boxLine());
  context->drawRect(m, kDrawFilled);
  const int n = uitext::METER_SEGMENTS;
  const CCoord gap = 1, inner = m.getWidth() - 2 - gap * (n - 1);
  for (int i = 0; i < n; i ++)
  {
    const CCoord x0 = m.left + 1 + floor(i * (inner / n + gap)), x1 = m.left + 1 + floor(i * (inner / n + gap) + inner / n);
    CColor c = Colors::meterOff();
    if (i < m_lit) c = i >= n - 2 ? Colors::meterYellow() : Colors::green();
    context->setFillColor(c);
    context->drawRect(CRect(x0, m.top + 1, x1, m.bottom - 1), kDrawFilled);
  }
  setDirty(false);
}

// ---------------------------------------------------------------------------

void BeatCells::setPosition(int pos, int length, int bpi)
{
  bpi = bpi > 0 && bpi <= 1024 ? bpi : 0;
  const int beat = bpi && length > 0 ? std::min(bpi - 1, std::max(0, (int)((double)pos / length * bpi))) : -1;
  if (beat == m_beat && bpi == m_bpi) return;
  m_beat = beat;
  m_bpi = bpi;
  invalid();
}

void BeatCells::draw(CDrawContext *context)
{
  const CRect r = getViewSize();
  const int n = m_bpi > 0 ? m_bpi : 16;
  const CCoord h = std::min<CCoord>(12, r.getHeight());
  const CCoord top = floor(r.getCenter().y - h / 2);
  const CCoord gap = r.getWidth() / n >= 6 ? 2 : (r.getWidth() / n >= 3 ? 1 : 0);
  const CCoord cellW = (r.getWidth() - gap * (n - 1)) / n;
  for (int b = 0; b < n; b ++)
  {
    const CCoord x0 = floor(r.left + b * (cellW + gap)), x1 = floor(r.left + b * (cellW + gap) + cellW);
    if (x1 <= x0) continue;
    CColor fill = b % 4 == 0 ? Colors::downbeat() : Colors::box();
    if (m_bpi > 0 && b < m_beat) fill = Colors::pastBeat();
    if (m_bpi > 0 && b == m_beat) fill = Colors::yellow();
    const CRect cell(x0, top, x1, top + h);
    context->setFillColor(Colors::boxLine());
    context->drawRect(cell, kDrawFilled);
    context->setFillColor(fill);
    context->drawRect(CRect(cell).inset(1, 1), kDrawFilled);
  }
  setDirty(false);
}

// ---------------------------------------------------------------------------

AddressBox::AddressBox(const CRect &size, IControlListener *listener, int32_t tag, const Fonts &fonts, const std::string &key)
  : CControl(size, listener, tag), m_keyFont(fonts.small), m_monoFont(fonts.mono), m_buttonFont(fonts.label), m_key(key)
{
  setMin(0);
  setMax(1);
}

void AddressBox::setAddress(const std::string &address, const std::string &placeholder)
{
  if (address == m_address && placeholder == m_placeholder) return;
  m_address = address;
  m_placeholder = placeholder;
  invalid();
}

void AddressBox::setCopyTitle(const std::string &title)
{
  if (title == m_copyTitle) return;
  m_copyTitle = title;
  invalid();
}

CCoord AddressBox::preferredWidth() const
{
  const std::string shown = m_address.empty() ? m_placeholder : m_address;
  const CFontRef font = m_address.empty() ? m_keyFont : m_monoFont;
  return 5 + textWidth(m_keyFont, m_key) + 5 + textWidth(font, shown) + 6 + textWidth(m_buttonFont, "Copied") + 10;
}

CRect AddressBox::copyRect() const
{
  const CRect r = getViewSize();
  const CCoord w = textWidth(m_buttonFont, "Copied") + 10;
  return CRect(r.right - 1 - w, r.top + 1, r.right - 1, r.bottom - 1);
}

void AddressBox::draw(CDrawContext *context)
{
  const CRect r = getViewSize();
  context->setDrawMode(kAntiAliasing);
  drawBox(context, r);
  CCoord x = r.left + 5;
  context->setFont(m_keyFont);
  context->setFontColor(Colors::label());
  context->drawString(m_key.c_str(), CRect(x, r.top, r.right, r.bottom), kLeftText);
  x += textWidth(m_keyFont, m_key) + 5;
  const CRect copy = copyRect();
  if (m_address.empty())
  {
    context->setFont(m_keyFont);
    context->setFontColor(Colors::placeholder());
    context->drawString(m_placeholder.c_str(), CRect(x, r.top, copy.left - 4, r.bottom), kLeftText);
  }
  else
  {
    context->setFont(m_monoFont);
    context->setFontColor(Colors::value());
    context->drawString(m_address.c_str(), CRect(x, r.top, copy.left - 4, r.bottom), kLeftText);
  }
  // the button: off when there is nothing to copy
  const bool can = !m_address.empty();
  context->setFillColor(m_pressed ? CColor(0x5e, 0x5e, 0x5e) : withAlpha(Colors::buttonOff(), can ? 1.0f : 0.5f));
  context->drawRect(copy, kDrawFilled);
  context->setFont(m_buttonFont);
  context->setFontColor(withAlpha(Colors::buttonText(), can ? 1.0f : 0.4f));
  context->drawString(m_copyTitle.c_str(), copy, kCenterText);
  setDirty(false);
}

CMouseEventResult AddressBox::onMouseDown(CPoint &where, const CButtonState &buttons)
{
  if (!(buttons & kLButton) || m_address.empty() || !copyRect().pointInside(where)) return kMouseEventNotHandled;
  m_pressed = true;
  invalid();
  return kMouseEventHandled;
}

CMouseEventResult AddressBox::onMouseUp(CPoint &where, const CButtonState &)
{
  const bool clicked = m_pressed && copyRect().pointInside(where);
  m_pressed = false;
  invalid();
  if (clicked)
  {
    beginEdit();
    setValue(1.0f);
    valueChanged();
    setValue(0.0f);
    endEdit();
  }
  return kMouseEventHandled;
}

CMouseEventResult AddressBox::onMouseCancel()
{
  m_pressed = false;
  invalid();
  return kMouseEventHandled;
}

// ---------------------------------------------------------------------------

TextLinesView::TextLinesView(const CRect &size, CFontRef font, CFontRef boldFont)
  : CView(size), m_font(font), m_bold(boldFont)
{
}

void TextLinesView::setLines(const std::vector<Line> &lines)
{
  m_lines = lines;
  m_wrapped = false;
  invalid();
}

void TextLinesView::setViewSize(const CRect &rect, bool inv)
{
  CView::setViewSize(rect, inv);
  m_wrapped = false;
}

static size_t utf8_next(const std::string &s, size_t i)
{
  if (i >= s.size()) return s.size();
  i++;
  while (i < s.size() && ((unsigned char)s[i] & 0xc0) == 0x80) i++;
  return i;
}

void TextLinesView::wrap(CDrawContext *context)
{
  const CCoord width = getViewSize().getWidth() - 12; // margins and the scroll mark
  m_visual.clear();
  m_wrapWidth = width;
  m_wrapped = true;
  if (width < 20) return;

  for (const Line &line : m_lines)
  {
    Visual first;
    first.who = line.who;
    first.whoColor = line.whoColor;
    first.textColor = line.textColor;
    CCoord indent = 0;
    if (!line.who.empty())
    {
      context->setFont(m_bold);
      first.whoWidth = context->getStringWidth((line.who + " ").c_str());
      indent = std::min(first.whoWidth, width / 2);
    }
    context->setFont(m_font);

    // break the text into words, and the words into lines
    const std::string &t = line.text;
    std::string cur;
    CCoord avail = width - indent;
    bool isFirst = true;
    size_t i = 0;
    auto flush = [&]() {
      Visual v = isFirst ? first : Visual();
      if (!isFirst) v.textColor = line.textColor;
      v.text = cur;
      m_visual.push_back(v);
      cur.clear();
      isFirst = false;
      avail = width;
    };
    while (i < t.size())
    {
      size_t j = i;
      while (j < t.size() && t[j] != ' ' && t[j] != '\n') j++;
      std::string word = t.substr(i, j - i);
      const std::string tryLine = cur.empty() ? word : cur + " " + word;
      if (context->getStringWidth(tryLine.c_str()) <= avail) cur = tryLine;
      else
      {
        if (!cur.empty()) flush();
        // a word longer than the line is split where it fits
        while (!word.empty() && context->getStringWidth(word.c_str()) > avail)
        {
          size_t k = utf8_next(word, 0), fit = k;
          while (k < word.size())
          {
            const size_t n = utf8_next(word, k);
            if (context->getStringWidth(word.substr(0, n).c_str()) > avail) break;
            fit = k = n;
          }
          cur = word.substr(0, fit);
          word = word.substr(fit);
          flush();
        }
        cur = word;
      }
      if (j < t.size() && t[j] == '\n') flush();
      i = j + 1;
    }
    if (!cur.empty() || isFirst) flush();
  }
}

void TextLinesView::draw(CDrawContext *context)
{
  const CRect r = getViewSize();
  context->setFillColor(m_background);
  context->drawRect(r, kDrawFilled);

  if (!m_wrapped || m_wrapWidth != r.getWidth() - 12) wrap(context);

  const CCoord lineH = std::max<CCoord>(12, ceil(m_font->getSize() * 1.32));
  const int rows = std::max(1, (int)((r.getHeight() - 4) / lineH));
  const int total = (int)m_visual.size();
  const int maxScroll = std::max(0, total - rows);
  if (m_scroll > maxScroll) m_scroll = maxScroll;
  if (m_scroll < 0) m_scroll = 0;

  int first;
  if (m_startAtTop) first = m_scroll;
  else first = std::max(0, total - rows - m_scroll);
  const int last = std::min(total, first + rows);

  CRect oldClip;
  context->getClipRect(oldClip);
  CRect clip(r);
  clip.bound(oldClip);
  context->setClipRect(clip);

  // newest at the bottom: a log starts at the bottom when it doesn't fill the view
  CCoord y = r.top + 2;
  if (!m_startAtTop && total < rows) y = r.bottom - 2 - lineH * total;
  for (int i = first; i < last; i ++, y += lineH)
  {
    const Visual &v = m_visual[i];
    CCoord x = r.left + 4;
    if (!v.who.empty())
    {
      context->setFont(m_bold);
      context->setFontColor(v.whoColor);
      context->drawString(v.who.c_str(), CRect(x, y, r.right - 6, y + lineH), kLeftText);
      x += v.whoWidth;
    }
    context->setFont(m_font);
    context->setFontColor(v.textColor);
    context->drawString(v.text.c_str(), CRect(x, y, r.right - 6, y + lineH), kLeftText);
  }

  // where we are, when not everything fits
  if (total > rows)
  {
    const CCoord trackH = r.getHeight() - 4;
    const CCoord thumbH = std::max<CCoord>(12, trackH * rows / total);
    const double at = maxScroll ? (m_startAtTop ? (double)m_scroll / maxScroll : 1.0 - (double)m_scroll / maxScroll) : 1.0;
    const CCoord top = r.top + 2 + (trackH - thumbH) * at;
    context->setFillColor(Colors::separator());
    context->drawRect(CRect(r.right - 4, top, r.right - 2, top + thumbH), kDrawFilled);
  }
  context->setClipRect(oldClip);
  setDirty(false);
}

void TextLinesView::onMouseWheelEvent(MouseWheelEvent &event)
{
  if (event.deltaY == 0) return;
  double d = event.deltaY;
  if (event.flags & MouseWheelEvent::DirectionInvertedFromDevice) d = -d;
  int steps = (int)lround(d * 3);
  if (!steps) steps = d > 0 ? 1 : -1;
  m_scroll += m_startAtTop ? -steps : steps;
  if (m_scroll < 0) m_scroll = 0;
  invalid();
  event.consumed = true;
}

// ---------------------------------------------------------------------------

void TextShortcuts::attach(CFrame *frame)
{
  detach();
  if (!frame || !frame->getPlatformFrame()) return;
  m_frame = frame;
  frame->registerKeyboardHook(this);
  m_catcher.start(frame->getPlatformFrame()->getPlatformRepresentation());
}

void TextShortcuts::detach()
{
  m_catcher.stop();
  if (m_frame) m_frame->unregisterKeyboardHook(this);
  m_frame = nullptr;
}

void TextShortcuts::onKeyboardEvent(KeyboardEvent &event, CFrame *frame)
{
  if (event.type != EventType::KeyDown || !frame || !frame->getPlatformFrame()) return;
  // VSTGUI's Control is Cmd on macOS, its Super is Ctrl there
  const textkeys::Command command =
    textkeys::commandFor(textkeys::letterOf(event.character), event.modifiers.has(ModifierKey::Control),
                         event.modifiers.has(ModifierKey::Shift), event.modifiers.has(ModifierKey::Alt),
                         event.modifiers.has(ModifierKey::Super));
  if (command == textkeys::Command::None) return;
  // a text box being typed in (it has the system's text field)
  auto *edit = dynamic_cast<CTextEdit *>(frame->getFocusView());
  if (!edit || !edit->getPlatformTextEdit()) return;
  if (textfield::perform(frame->getPlatformFrame()->getPlatformRepresentation(), command)) event.consumed = true;
}

// ---------------------------------------------------------------------------

CTextLabel *makeLabel(const CRect &size, const std::string &text, CFontRef font, CColor color, CHoriTxtAlign align)
{
  CTextLabel *l = new CTextLabel(size, text.c_str());
  l->setFont(font);
  l->setFontColor(color);
  l->setBackColor(kTransparentCColor);
  l->setFrameColor(kTransparentCColor);
  l->setStyle(CParamDisplay::kNoFrame);
  l->setHoriAlign(align);
  l->setTextInset(CPoint(0, 0));
  l->setTextTruncateMode(CTextLabel::kTruncateTail);
  l->setMouseEnabled(false);
  return l;
}

CTextEdit *makeEdit(const CRect &size, IControlListener *listener, int32_t tag, CFontRef font, const char *placeholder)
{
  CTextEdit *e = new CTextEdit(size, listener, tag, "");
  e->setFont(font);
  e->setFontColor(Colors::value());
  e->setBackColor(Colors::box());
  e->setFrameColor(Colors::boxLine());
  e->setStyle(CParamDisplay::kRoundRectStyle);
  e->setRoundRectRadius(RADIUS);
  e->setHoriAlign(kLeftText);
  e->setTextInset(CPoint(5, 0));
  if (placeholder) e->setPlaceholderString(placeholder);
  return e;
}

void styleMenus(CFrame *frame, CFontRef font)
{
#if defined(__linux__)
  // macOS and Windows show their own menus; on Linux VSTGUI draws them
  if (!frame || !frame->getPlatformFrame()) return;
  GenericOptionMenuTheme theme;
  theme.font = font;
  theme.backgroundColor = CColor(0x26, 0x26, 0x26, 250);
  theme.selectedBackgroundColor = Colors::yellow();
  theme.textColor = Colors::value();
  theme.selectedTextColor = Colors::dark();
  theme.disabledTextColor = Colors::label();
  theme.titleTextColor = Colors::label();
  theme.separatorColor = Colors::separator();
  theme.inset = CPoint(4, 3);
  theme.menuAnimationTime = 0;
  frame->getPlatformFrame()->setupGenericOptionMenu(true, &theme);
#else
  (void)frame;
  (void)font;
#endif
}

} // namespace ui
} // namespace ninjam_vst3
