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

namespace ninjam_vst3 {
namespace ui {

float gainToSlider(float gain)
{
  if (!(gain > 0.0f)) return 0.0f;
  return std::min(1.0f, sqrtf(gain * 0.5f));
}

static CColor mix(const CColor &a, const CColor &b, float t)
{
  return CColor((uint8_t)(a.red + (b.red - a.red) * t), (uint8_t)(a.green + (b.green - a.green) * t),
                (uint8_t)(a.blue + (b.blue - a.blue) * t), 255);
}

static void fillRoundRect(CDrawContext *context, const CRect &r, CCoord radius, const CColor &fill, const CColor *frame)
{
  SharedPointer<CGraphicsPath> path = owned(context->createRoundRectGraphicsPath(r, radius));
  if (!path)
  {
    context->setFillColor(fill);
    context->drawRect(r, kDrawFilled);
    return;
  }
  context->setFillColor(fill);
  context->drawGraphicsPath(path, CDrawContext::kPathFilled);
  if (frame)
  {
    context->setFrameColor(*frame);
    context->setLineWidth(1);
    context->drawGraphicsPath(path, CDrawContext::kPathStroked);
  }
}

// ---------------------------------------------------------------------------

void MeterView::setLevels(float left, float right)
{
  if (fabsf(left - m_level[0]) < 0.0005f && fabsf(right - m_level[1]) < 0.0005f) return;
  m_level[0] = left;
  m_level[1] = right;
  invalid();
}

void MeterView::draw(CDrawContext *context)
{
  const CRect r = getViewSize();
  context->setFillColor(Colors::field());
  context->drawRect(r, kDrawFilled);

  const CCoord gap = 2, h = (r.getHeight() - gap * 3) / 2;
  for (int c = 0; c < 2; c ++)
  {
    const float lin = m_level[c];
    const float db = lin > 0.0f ? 20.0f * log10f(lin) : -100.0f;
    const float pos = std::min(1.0f, std::max(0.0f, (db + 60.0f) / 66.0f));
    if (pos <= 0) continue;
    CRect bar(r.left + gap, r.top + gap + c * (h + gap), r.left + gap + (r.getWidth() - 2 * gap) * pos, r.top + gap + c * (h + gap) + h);
    const CColor col = db > 0.0f ? Colors::error() : db > -9.0f ? Colors::warn() : Colors::accent();
    context->setFillColor(col);
    context->drawRect(bar, kDrawFilled);
  }
  // 0 dB mark
  const CCoord x = r.left + gap + (r.getWidth() - 2 * gap) * (60.0 / 66.0);
  context->setFrameColor(Colors::panelLine());
  context->setLineWidth(1);
  context->drawLine(CPoint(x, r.top), CPoint(x, r.bottom));
  setDirty(false);
}

// ---------------------------------------------------------------------------

void IntervalView::setPosition(int pos, int length, int bpi)
{
  if (pos == m_pos && length == m_length && bpi == m_bpi) return;
  m_pos = pos;
  m_length = length;
  m_bpi = bpi;
  invalid();
}

void IntervalView::setMessage(const std::string &text)
{
  if (text == m_message) return;
  m_message = text;
  invalid();
}

void IntervalView::draw(CDrawContext *context)
{
  const CRect r = getViewSize();
  context->setFillColor(Colors::field());
  context->drawRect(r, kDrawFilled);

  const int bpi = m_bpi > 0 && m_bpi <= 512 ? m_bpi : 0;
  if (!bpi || m_length <= 0 || !m_message.empty())
  {
    context->setFont(m_font);
    context->setFontColor(m_message.empty() ? Colors::dim() : Colors::warn());
    context->drawString(m_message.empty() ? "Interval" : m_message.c_str(), r, kCenterText);
    setDirty(false);
    return;
  }
  const double frac = std::min(1.0, std::max(0.0, (double)m_pos / m_length));
  const int beat = std::min(bpi - 1, (int)(frac * bpi));
  const CCoord cellW = r.getWidth() / bpi;
  const CCoord gap = cellW > 6 ? 1 : 0;
  for (int b = 0; b < bpi; b ++)
  {
    CRect cell(r.left + b * cellW + gap, r.top + 1, r.left + (b + 1) * cellW - gap, r.bottom - 1);
    CColor col = Colors::panel();
    if (b < beat) col = mix(Colors::accent(), Colors::panel(), 0.45f);
    else if (b == beat) col = b == 0 ? Colors::warn() : Colors::accent();
    context->setFillColor(col);
    context->drawRect(cell, kDrawFilled);
  }
  if (cellW >= 14)
  {
    context->setFont(m_font);
    for (int b = 0; b < bpi; b ++)
    {
      CRect cell(r.left + b * cellW, r.top, r.left + (b + 1) * cellW, r.bottom);
      context->setFontColor(b == beat ? Colors::background() : Colors::dim());
      char buf[8];
      snprintf(buf, sizeof(buf), "%d", b + 1);
      context->drawString(buf, cell, kCenterText);
    }
  }
  setDirty(false);
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
  const CCoord width = getViewSize().getWidth() - 14; // margins and the scroll mark
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
  context->setFillColor(Colors::field());
  context->drawRect(r, kDrawFilled);

  if (!m_wrapped || m_wrapWidth != r.getWidth() - 14) wrap(context);

  const CCoord lineH = std::max<CCoord>(14, m_font->getSize() * 1.45);
  const int rows = std::max(1, (int)((r.getHeight() - 8) / lineH));
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

  CCoord y = r.top + 4;
  for (int i = first; i < last; i ++, y += lineH)
  {
    const Visual &v = m_visual[i];
    CCoord x = r.left + 6;
    if (!v.who.empty())
    {
      context->setFont(m_bold);
      context->setFontColor(v.whoColor);
      context->drawString(v.who.c_str(), CRect(x, y, r.right - 8, y + lineH), kLeftText);
      x += v.whoWidth;
    }
    context->setFont(m_font);
    context->setFontColor(v.textColor);
    context->drawString(v.text.c_str(), CRect(x, y, r.right - 8, y + lineH), kLeftText);
  }

  // where we are, when not everything fits
  if (total > rows)
  {
    const CCoord trackH = r.getHeight() - 4;
    const CCoord thumbH = std::max<CCoord>(16, trackH * rows / total);
    const double at = maxScroll ? (m_startAtTop ? (double)m_scroll / maxScroll : 1.0 - (double)m_scroll / maxScroll) : 1.0;
    const CCoord top = r.top + 2 + (trackH - thumbH) * at;
    context->setFillColor(Colors::panelLine());
    context->drawRect(CRect(r.right - 5, top, r.right - 2, top + thumbH), kDrawFilled);
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

void PanelView::drawBackgroundRect(CDrawContext *context, const CRect &)
{
  // drawn in the container's own coordinates
  CRect r(0, 0, getViewSize().getWidth(), getViewSize().getHeight());
  context->setDrawMode(kAntiAliasing);
  if (m_line.alpha) fillRoundRect(context, r.inset(0.5, 0.5), m_radius, m_fill, &m_line);
  else fillRoundRect(context, r, m_radius, m_fill, nullptr);
}

// ---------------------------------------------------------------------------

CSlider *makeSlider(const CRect &size, IControlListener *listener, int32_t tag, bool fromCenter)
{
  CSlider *s = new CSlider(size, listener, tag, 0, (int32_t)size.getWidth(), nullptr, nullptr);
  s->setDrawStyle(CSlider::kDrawFrame | CSlider::kDrawBack | CSlider::kDrawValue | (fromCenter ? CSlider::kDrawValueFromCenter : 0));
  s->setBackColor(Colors::field());
  s->setFrameColor(Colors::panelLine());
  s->setValueColor(fromCenter ? Colors::blue() : Colors::accent());
  s->setFrameWidth(1);
  s->setMin(0.0f);
  s->setMax(1.0f);
  s->setDefaultValue(fromCenter ? 0.5f : gainToSlider(1.0f));
  s->setWheelInc(0.02f);
  return s;
}

CTextButton *makeButton(const CRect &size, IControlListener *listener, int32_t tag, const char *title, bool onOff, CFontRef font)
{
  CTextButton *b = new CTextButton(size, listener, tag, title, onOff ? CTextButton::kOnOffStyle : CTextButton::kKickStyle);
  b->setFont(font);
  b->setRoundRadius(4);
  b->setFrameColor(Colors::panelLine());
  b->setFrameColorHighlighted(Colors::accent());
  b->setTextColor(Colors::text());
  b->setTextColorHighlighted(CColor(255, 255, 255));
  SharedPointer<CGradient> normal = owned(CGradient::create(0, 1, CColor(60, 67, 78), CColor(48, 54, 63)));
  SharedPointer<CGradient> lit = owned(CGradient::create(0, 1, CColor(52, 150, 106), CColor(40, 122, 86)));
  b->setGradient(normal);
  b->setGradientHighlighted(lit);
  return b;
}

CTextLabel *makeLabel(const CRect &size, const char *text, CFontRef font, CColor color, CHoriTxtAlign align)
{
  CTextLabel *l = new CTextLabel(size, text);
  l->setFont(font);
  l->setFontColor(color);
  l->setBackColor(kTransparentCColor);
  l->setFrameColor(kTransparentCColor);
  l->setStyle(CParamDisplay::kNoFrame);
  l->setHoriAlign(align);
  l->setTextTruncateMode(CTextLabel::kTruncateTail);
  l->setMouseEnabled(false);
  return l;
}

CTextEdit *makeEdit(const CRect &size, IControlListener *listener, int32_t tag, const char *text, CFontRef font, const char *placeholder)
{
  CTextEdit *e = new CTextEdit(size, listener, tag, text);
  e->setFont(font);
  e->setFontColor(Colors::text());
  e->setBackColor(Colors::field());
  e->setFrameColor(Colors::panelLine());
  e->setStyle(CParamDisplay::kRoundRectStyle);
  e->setRoundRectRadius(3);
  e->setHoriAlign(kLeftText);
  e->setTextInset(CPoint(6, 0));
  if (placeholder) e->setPlaceholderString(placeholder);
  return e;
}

} // namespace ui
} // namespace ninjam_vst3
