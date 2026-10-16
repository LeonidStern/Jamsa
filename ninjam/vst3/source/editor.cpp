/*
    NINJAM VST3 - editor.cpp
    Copyright (C) 2026 and onward, NINJAM contributors

    NINJAM is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.
*/

#include "editor.h"

#include <math.h>
#include <string.h>
#include <algorithm>
#include <chrono>
#include <vector>

#include "vstgui/lib/cvstguitimer.h"

#if SMTG_OS_LINUX
#include "public.sdk/source/vst/vstgui_linux_runloop_support.h"
#include "vstgui/lib/platform/linux/linuxfactory.h"
#include "vstgui/lib/platform/platform_x11.h"
#include "vstgui/lib/platform/platformfactory.h"
#include "vstgui/lib/platform/linux/x11platform.h"
#include <cairo/cairo-xcb.h>
#include <xcb/xcb.h>
#endif

#include "clipboard.h"
#include "plugin.h"
#include "uitext.h"
#include "version.h"

using namespace VSTGUI;
using namespace Steinberg;

namespace ninjam_vst3 {

using namespace ui;

enum Tags
{
  kTagName = 1,
  kTagServer,
  kTagConnect,
  kTagTransmit,
  kTagInputVolume,
  kTagMonitor,
  kTagClickVolume,
  kTagClick,
  kTagChat,
  kTagHost,
  kTagCopyWan,
  kTagAccept,
  kTagDecline,
  kTagBitrate,
  kTagCodec,
  kTagSync,
  kTagBpm,
  kTagBpi,
  kTagRemote = 1000, // + row * 8 + field
};
enum { kRemoteVolume, kRemotePan, kRemoteMute, kRemoteSolo, kRemoteFields = 8 };

// the layout, at the default size (logical pixels)
static const CCoord PAD = 3, GAP = 3;            // around the window, between the rows and panes
static const CCoord HEADER_H = 24, TRANSPORT_H = 26, HOSTBAR_H = 24;
static const CCoord CHAT_W = 250;                // the chat pane; the mixer takes the rest
static const CCoord ITEM_H = 18;                 // value boxes and buttons in the strips
static const CCoord ROW_H = 22, ROW_GAP = 2;     // the mixer's rows
static const CCoord COLS_H = 12, FOOTER_H = 12;  // the mixer's column titles, its footer line

static double now_seconds()
{
  using namespace std::chrono;
  return duration<double>(steady_clock::now().time_since_epoch()).count();
}

static void setLabel(CTextLabel *label, const std::string &text, bool tooltip = false)
{
  if (!label || label->getText() == text.c_str()) return;
  label->setText(text.c_str());
  if (tooltip) label->setTooltipText(text.c_str()); // the whole text, when the label cuts it off
}

static void setTooltip(CView *v, const std::string &text)
{
  if (!v) return;
  // the tooltip the view has now (a C string; CView's getAttribute<T> copies sizeof(T) bytes,
  // so it is read as bytes)
  std::string old;
  uint32_t size = 0, got = 0;
  if (v->getAttributeSize(kCViewTooltipAttribute, size) && size > 0)
  {
    std::vector<char> buf(size);
    if (v->getAttribute(kCViewTooltipAttribute, size, buf.data(), got)) old.assign(buf.data(), strnlen(buf.data(), got));
  }
  if (old != text) v->setTooltipText(text.empty() ? nullptr : text.c_str());
}

static void setLabelColor(CTextLabel *label, const CColor &c)
{
  if (label && label->getFontColor() != c)
  {
    label->setFontColor(c);
    label->invalid();
  }
}

static void setControlValue(CControl *c, float v)
{
  if (!c || c->isEditing()) return;
  if (fabsf(c->getValue() - v) > 0.0001f)
  {
    c->setValue(v);
    c->invalid();
  }
}

static void setVisible(CView *v, bool on)
{
  if (v && v->isVisible() != on) v->setVisible(on);
}

static void place(CView *v, const CRect &r)
{
  if (!v) return;
  v->setViewSize(r);
  v->setMouseableArea(r);
}

#if SMTG_OS_LINUX
// Our open X11 frames. VSTGUI disconnects from the X server when the last one closes, but
// cairo keeps its state for that connection (its fonts hold on to it): a later connection
// that gets the same address would reuse it, and cairo aborts ("_get_screen_index"), as when
// a host closes the editor and opens it again. So cairo's device for the connection is
// finished before the last frame closes.
static int g_openX11Frames = 0;
static bool g_runLoopFromPlugFrame = false; // VSTGUI's run loop is the one of an editor's plug frame

static void finishCairoDevice()
{
  xcb_connection_t *c = VSTGUI::X11::RunLoop::instance().getXcbConnection();
  if (!c || xcb_connection_has_error(c)) return;
  const xcb_screen_t *screen = xcb_setup_roots_iterator(xcb_get_setup(c)).data;
  if (!screen) return;
  xcb_visualtype_t *visual = nullptr;
  for (auto d = xcb_screen_allowed_depths_iterator(screen); d.rem && !visual; xcb_depth_next(&d))
    for (auto v = xcb_depth_visuals_iterator(d.data); v.rem; xcb_visualtype_next(&v))
      if (v.data->visual_id == screen->root_visual)
      {
        visual = v.data;
        break;
      }
  if (!visual) return;
  cairo_surface_t *s = cairo_xcb_surface_create(c, screen->root, visual, 1, 1);
  if (cairo_device_t *device = cairo_surface_get_device(s)) cairo_device_finish(device);
  cairo_surface_destroy(s);
}
#endif

static std::string codecFromFourcc(const std::string &f)
{
  if (f == "FLAC") return "FLAC";
  if (f == "OPUS") return "Opus";
  if (f == "OGGv") return "Vorbis";
  return f;
}

double NinjamEditor::zoomFor(double width, double height)
{
  return std::max(1.0, std::min(width / DEFAULT_WIDTH, height / DEFAULT_HEIGHT));
}

NinjamEditor::NinjamEditor(NinjamPlugin *plugin)
  : VSTGUIEditor(static_cast<Steinberg::Vst::EditController *>(plugin)), m_plugin(plugin)
{
  const Settings s = engine().getSettings();
  if (s.editorWidth > 0 && s.editorHeight > 0)
  {
    m_width = std::min<CCoord>(MAX_WIDTH, std::max<CCoord>(MIN_WIDTH, s.editorWidth));
    m_height = std::min<CCoord>(MAX_HEIGHT, std::max<CCoord>(MIN_HEIGHT, s.editorHeight));
  }
  m_zoom = zoomFor(m_width, m_height);
  ViewRect r(0, 0, (int32)m_width, (int32)m_height);
  setRect(r);
  setIdleRate(33);
  m_fonts.make();
}

NinjamEditor::~NinjamEditor()
{
}

Engine &NinjamEditor::engine()
{
  return m_plugin->engine();
}

tresult PLUGIN_API NinjamEditor::queryInterface(const TUID iid, void **obj)
{
  QUERY_INTERFACE(iid, obj, IPlugViewContentScaleSupport::iid, IPlugViewContentScaleSupport)
  return VSTGUIEditor::queryInterface(iid, obj);
}

tresult PLUGIN_API NinjamEditor::isPlatformTypeSupported(FIDString type)
{
#if SMTG_OS_LINUX
  // X11 only: hosts that offer Wayland offer X11 too
  if (type && strcmp(type, kPlatformTypeX11EmbedWindowID) != 0) return kResultFalse;
#endif
  return VSTGUIEditor::isPlatformTypeSupported(type);
}

bool PLUGIN_API NinjamEditor::open(void *parent, const PlatformType &platformType)
{
  if (frame) return false;
  m_zoom = zoomFor(m_width, m_height);
  frame = new CFrame(CRect(0, 0, m_width / m_zoom, m_height / m_zoom), this);
  frame->setBackgroundColor(Colors::window());
  frame->enableTooltips(true);
  buildViews();
  if (m_scale * m_zoom != 1.0) frame->setZoom(m_scale * m_zoom);

  IPlatformFrameConfig *config = nullptr;
#if SMTG_OS_LINUX
  // VSTGUI's X11 frame needs the host's run loop, which the host gives with the plug frame.
  // It may last only as long as the host's editor windows (JUCE hosts), so it's taken from
  // the plug frame whenever no other editor is open, and let go when the last one closes.
  X11::FrameConfig x11config;
  if (auto linuxFactory = getPlatformFactory().asLinuxFactory())
  {
    if (plugFrame && (g_openX11Frames == 0 || !linuxFactory->getRunLoop()) &&
        Steinberg::Linux::setupVSTGUIRunloop(plugFrame))
      g_runLoopFromPlugFrame = true;
    x11config.runLoop = linuxFactory->getRunLoop();
  }
  if (!x11config.runLoop)
  {
    frame->forget();
    frame = nullptr;
    return false; // without a run loop the frame can't draw
  }
  config = &x11config;
#endif
  if (!frame->open(parent, platformType, config))
  {
    frame->forget();
    frame = nullptr;
    return false;
  }
#if SMTG_OS_LINUX
  g_openX11Frames ++;
#endif
  m_textShortcuts.attach(frame);
  styleMenus(frame, m_fonts.menu);
  layout();
  syncSettings();
  m_usersRevision = m_settingsRevision = ~0u;
  m_chatRevision = ~0ull;
  update();
  return true;
}

void PLUGIN_API NinjamEditor::close()
{
  if (frame)
  {
    m_textShortcuts.detach();
    // the frame removes (and releases) all views, the license view too
    CFrame *f = frame;
    frame = nullptr;
#if SMTG_OS_LINUX
    const bool last = --g_openX11Frames == 0;
    if (last) finishCairoDevice();
#endif
    f->close();
#if SMTG_OS_LINUX
    // VSTGUIEditor::removed() stopped the idle timer before this, and the frame is gone
    if (last && g_runLoopFromPlugFrame)
    {
      if (auto linuxFactory = getPlatformFactory().asLinuxFactory()) linuxFactory->setRunLoop(nullptr);
      g_runLoopFromPlugFrame = false;
    }
#endif
  }
  m_root = nullptr;
  m_rows.clear();
  m_license = m_licenseBox = nullptr;
  m_licenseTitle = nullptr;
  m_licenseText = nullptr;
  m_accept = m_decline = nullptr;
  m_licenseOpen = false;
  m_licenseShown.clear();
  m_copied = nullptr;
}

CMessageResult NinjamEditor::notify(CBaseObject *sender, const char *message)
{
  if (message == CVSTGUITimer::kMsgTimer && frame) update();
  return VSTGUIEditor::notify(sender, message);
}

// ---------------------------------------------------------------------------
// size
// ---------------------------------------------------------------------------

tresult PLUGIN_API NinjamEditor::checkSizeConstraint(ViewRect *rect)
{
  if (!rect) return kInvalidArgument;
  const double s = m_scale;
  const int32 w = std::min((int32)lround(MAX_WIDTH * s), std::max((int32)lround(MIN_WIDTH * s), rect->getWidth()));
  const int32 h = std::min((int32)lround(MAX_HEIGHT * s), std::max((int32)lround(MIN_HEIGHT * s), rect->getHeight()));
  rect->right = rect->left + w;
  rect->bottom = rect->top + h;
  return kResultTrue;
}

tresult PLUGIN_API NinjamEditor::onSize(ViewRect *newSize)
{
  if (!newSize) return kInvalidArgument;
  const tresult r = VSTGUIEditor::onSize(newSize);
  setLogicalSize(newSize->getWidth() / m_scale, newSize->getHeight() / m_scale);
  return r;
}

tresult PLUGIN_API NinjamEditor::setContentScaleFactor(ScaleFactor factor)
{
#if SMTG_OS_MACOS
  (void)factor;
  return kResultFalse; // macOS scales by itself
#else
  if (!(factor > 0.25f && factor < 8.0f)) return kInvalidArgument;
  if (fabs(factor - m_scale) < 0.001) return kResultTrue;
  m_scale = factor;
  ViewRect r(0, 0, (int32)lround(m_width * m_scale), (int32)lround(m_height * m_scale));
  setRect(r);
  if (frame)
  {
    frame->setZoom(m_scale * m_zoom);
    if (plugFrame) plugFrame->resizeView(this, &r);
  }
  return kResultTrue;
#endif
}

// The frame's views are laid out in logical pixels (720 x 330 at the default size); the
// frame's transform scales them to the window: the host's content scale times m_zoom.
void NinjamEditor::applyZoom()
{
  if (!frame) return;
  const double t = m_scale * m_zoom;
  // setZoom() keeps the frame's logical size and changes its size in pixels; here the pixels
  // are the window's, so the transform changes first and setZoom() keeps the size
  frame->setTransform(CGraphicsTransform().scale(t, t));
  frame->setZoom(t);
}

void NinjamEditor::setLogicalSize(CCoord width, CCoord height)
{
  m_width = std::max<CCoord>(1, width);
  m_height = std::max<CCoord>(1, height);
  engine().setEditorSize((int)lround(m_width), (int)lround(m_height));
  const double zoom = zoomFor(m_width, m_height);
  if (fabs(zoom - m_zoom) > 1e-6)
  {
    m_zoom = zoom;
    applyZoom();
  }
  if (m_root) layout();
}

// ---------------------------------------------------------------------------
// views
// ---------------------------------------------------------------------------

static CTextLabel *makeValueLabel(const CRect &size, CFontRef font)
{
  // text in a value box that can't be edited ("3 / 16")
  CTextLabel *l = makeLabel(size, "", font, Colors::value(), kCenterText);
  l->setBackColor(Colors::box());
  l->setFrameColor(Colors::boxLine());
  l->setStyle(CParamDisplay::kRoundRectStyle);
  l->setRoundRectRadius(2);
  return l;
}

void NinjamEditor::buildViews()
{
  const CRect z(0, 0, 10, 10);
  const Fonts &f = m_fonts;
  m_root = new CViewContainer(CRect(0, 0, m_width / m_zoom, m_height / m_zoom));
  m_root->setBackgroundColor(Colors::window());
  frame->addView(m_root);

  // header: Jamsa  Name [..]  Server [..]  Connect          (o) Connected · Opus
  m_header = new StripContainer(z, Colors::strip());
  m_root->addView(m_header);
  m_title = makeLabel(z, NINJAM_VST3_NAME, f.bold, CColor(0xe8, 0xe8, 0xe8));
  m_header->addView(m_title);
  m_nameLabel = makeLabel(z, "Name", f.label, Colors::label());
  m_header->addView(m_nameLabel);
  m_nameEdit = makeEdit(z, this, kTagName, f.value, "Your name");
  m_header->addView(m_nameEdit);
  m_serverLabel = makeLabel(z, "Server", f.label, Colors::label());
  m_header->addView(m_serverLabel);
  m_serverEdit = makeEdit(z, this, kTagServer, f.value, "host:port");
  m_header->addView(m_serverEdit);
  m_connect = new FlatButton(z, this, kTagConnect, "Connect", FlatButton::Push, f.button);
  m_header->addView(m_connect);
  m_connLed = new Led(z);
  m_header->addView(m_connLed);
  m_connText = makeLabel(z, "", f.value, Colors::status(), kRightText);
  m_header->addView(m_connText);

  // transport: BPM [120.00]  BPI [16]  | Sync (o) Live 120.00 | Click (knob) | beats  [3 / 16]
  m_transport = new StripContainer(z, Colors::strip());
  m_root->addView(m_transport);
  m_bpmLabel = makeLabel(z, "BPM", f.label, Colors::label());
  m_transport->addView(m_bpmLabel);
  m_bpm = new DragNumberBox(z, this, kTagBpm, f.value);
  m_bpm->dragMap = [](int start, double dy, bool fine) { return uitext::dragBpm(start, dy, fine); };
  m_bpm->format = [](int v) { return uitext::bpmBoxText(v); };
  m_bpm->setTooltipText("The room's tempo: drag up or down, or double-click and type, to vote for another");
  m_transport->addView(m_bpm);
  m_bpiLabel = makeLabel(z, "BPI", f.label, Colors::label());
  m_transport->addView(m_bpiLabel);
  m_bpi = new MenuBox(z, this, kTagBpi, f.value);
  for (int b : uitext::BPI_CHOICES) m_bpi->addEntry(std::to_string(b).c_str());
  m_bpi->setTooltipText("Beats per interval: pick one to vote for it");
  m_transport->addView(m_bpi);
  for (auto &sep : m_sep)
  {
    sep = new Strip(z, Colors::separator());
    m_transport->addView(sep);
  }
  m_sync = new FlatButton(z, this, kTagSync, "Sync", FlatButton::Toggle, f.button);
  m_transport->addView(m_sync);
  m_syncLed = new Led(z);
  m_transport->addView(m_syncLed);
  m_syncText = makeLabel(z, "", f.value, Colors::status());
  m_transport->addView(m_syncText);
  m_click = new FlatButton(z, this, kTagClick, "Click", FlatButton::Toggle, f.button);
  m_click->setTooltipText("The metronome: the first beat of each interval higher");
  m_transport->addView(m_click);
  m_clickVolume = new ArcKnob(z, this, kTagClickVolume, false);
  m_clickVolume->setDefaultValue(gainToSlider(0.5f));
  m_clickVolume->setTooltipText("The click's volume (double-click for the default)");
  m_transport->addView(m_clickVolume);
  m_beats = new BeatCells(z);
  m_transport->addView(m_beats);
  m_beatBox = makeValueLabel(z, f.value);
  m_transport->addView(m_beatBox);

  // mixer: the column titles, your channel, the others, a line at the bottom
  m_mixer = new StripContainer(z, Colors::pane());
  m_root->addView(m_mixer);
  m_colChannel = makeLabel(z, "CHANNEL", f.small, Colors::label());
  m_mixer->addView(m_colChannel);
  m_colVolume = makeLabel(z, "VOLUME", f.small, Colors::label());
  m_mixer->addView(m_colVolume);
  m_colPan = makeLabel(z, "PAN", f.small, Colors::label(), kCenterText);
  m_mixer->addView(m_colPan);
  m_colLevel = makeLabel(z, "LEVEL", f.small, Colors::label());
  m_mixer->addView(m_colLevel);

  m_myRow = new StripContainer(z, Colors::myRow(), 1);
  m_mixer->addView(m_myRow);
  m_tx = new FlatButton(z, this, kTagTransmit, "TX", FlatButton::Toggle, f.tiny);
  m_tx->setOnColors(Colors::orange(), Colors::orangeLine());
  m_tx->setTooltipText("Transmit: send your input to the room");
  m_myRow->addView(m_tx);
  m_in = new FlatButton(z, this, kTagMonitor, "IN", FlatButton::Toggle, f.tiny);
  m_in->setTooltipText("Monitor: hear your input in the plug-in's output");
  m_myRow->addView(m_in);
  m_myName = makeLabel(z, "", f.value, Colors::value());
  m_myRow->addView(m_myName);
  m_inputVolume = new FaderBox(z, this, kTagInputVolume, f.small);
  m_inputVolume->format = [](float v) { return uitext::dbText(sliderToGain(v)); };
  m_inputVolume->setDefaultValue(gainToSlider(1.0f));
  m_inputVolume->setTooltipText("The volume of what you send (double-click for 0 dB)");
  m_myRow->addView(m_inputVolume);
  m_codec = new MenuBox(z, this, kTagCodec, f.small);
  m_codec->addEntry("Opus"); // the order of CODEC_CHOICES
  m_codec->addEntry("FLAC");
  m_myRow->addView(m_codec);
  m_bitrate = new MenuBox(z, this, kTagBitrate, f.small);
  for (int k : Settings::OPUS_KBPS) m_bitrate->addEntry(uitext::kbpsText(k).c_str());
  m_myRow->addView(m_bitrate);
  m_myMeter = new SegmentMeter(z);
  m_myRow->addView(m_myMeter);

  m_players = new CScrollView(z, CRect(0, 0, 10, 10),
                              CScrollView::kVerticalScrollbar | CScrollView::kDontDrawFrame | CScrollView::kAutoHideScrollbars |
                              CScrollView::kOverlayScrollbars);
  m_players->setBackgroundColor(kTransparentCColor);
  m_players->setScrollbarWidth(5);
  if (CScrollbar *sb = m_players->getVerticalScrollbar())
  {
    sb->setScrollerColor(Colors::separator());
    sb->setBackgroundColor(kTransparentCColor);
    sb->setFrameColor(kTransparentCColor);
  }
  m_mixer->addView(m_players);
  m_noPlayers = makeLabel(z, "", f.label, Colors::label(), kCenterText);
  m_mixer->addView(m_noPlayers);
  m_footer = makeLabel(z, "", f.label, Colors::label());
  m_mixer->addView(m_footer);

  // chat
  m_chatPane = new StripContainer(z, Colors::pane());
  m_root->addView(m_chatPane);
  m_chat = new TextLinesView(z, f.chat, f.chatBold);
  m_chatPane->addView(m_chat);
  m_chatEdit = makeEdit(z, this, kTagChat, f.chat, "Chat, !vote bpm 120, /me \xe2\x80\xa6");
  m_chatPane->addView(m_chatEdit);

  // host bar: Host  3 in room  WAN [..][Copy]                          (o) Port open
  m_hostBar = new StripContainer(z, Colors::strip());
  m_root->addView(m_hostBar);
  m_host = new FlatButton(z, this, kTagHost, "Host", FlatButton::Toggle, f.button);
  m_host->setTooltipText("Host a NINJAM server in this plug-in, and join it");
  m_hostBar->addView(m_host);
  m_roomCount = makeLabel(z, "", f.value, Colors::status());
  m_hostBar->addView(m_roomCount);
  m_hostHint = makeLabel(z, "", f.label, Colors::label());
  m_hostBar->addView(m_hostHint);
  m_wan = new AddressBox(z, this, kTagCopyWan, f, "WAN");
  m_wan->setTooltipText("Where players on the internet join, once the port is forwarded to this computer");
  m_hostBar->addView(m_wan);
  m_portLed = new Led(z);
  m_hostBar->addView(m_portLed);
  m_portText = makeLabel(z, "", f.value, Colors::status(), kRightText);
  m_hostBar->addView(m_portText);
}

// the mixer's columns in a row of width w: CHANNEL, VOLUME (the rest), PAN, M, S, LEVEL
NinjamEditor::Columns NinjamEditor::columns(CCoord w)
{
  Columns c;
  const CCoord g = 5;
  c.name0 = 4;
  c.name1 = 100;
  c.level1 = w - 4;
  c.level0 = c.level1 - 54;
  c.solo1 = c.level0 - g;
  c.solo0 = c.solo1 - 18;
  c.mute1 = c.solo0 - g;
  c.mute0 = c.mute1 - 18;
  c.pan1 = c.mute0 - g;
  c.pan0 = c.pan1 - 26;
  c.vol1 = c.pan0 - g;
  c.vol0 = c.name1 + g;
  if (c.vol1 - c.vol0 < 50)
  {
    // a very narrow mixer: the names give way
    c.vol0 = c.vol1 - 50;
    c.name1 = std::max<CCoord>(c.name0 + 20, c.vol0 - g);
  }
  return c;
}

void NinjamEditor::layout()
{
  if (!m_root) return;
  const CCoord W = std::max<CCoord>(m_width / m_zoom, MIN_WIDTH), H = std::max<CCoord>(m_height / m_zoom, MIN_HEIGHT);
  place(m_root, CRect(0, 0, m_width / m_zoom, m_height / m_zoom));

  const CCoord transportY = PAD + HEADER_H + GAP, bodyY = transportY + TRANSPORT_H + GAP;
  const CCoord hostY = H - PAD - HOSTBAR_H, bodyBottom = hostY - GAP;
  place(m_header, CRect(PAD, PAD, W - PAD, PAD + HEADER_H));
  place(m_transport, CRect(PAD, transportY, W - PAD, transportY + TRANSPORT_H));
  place(m_mixer, CRect(PAD, bodyY, W - PAD - CHAT_W - GAP, bodyBottom));
  place(m_chatPane, CRect(W - PAD - CHAT_W, bodyY, W - PAD, bodyBottom));
  place(m_hostBar, CRect(PAD, hostY, W - PAD, hostY + HOSTBAR_H));

  layoutHeader(W - 2 * PAD);
  layoutTransport(W - 2 * PAD);
  layoutMixer(m_mixer->getViewSize());
  {
    const CCoord cw = CHAT_W, ch = bodyBottom - bodyY;
    place(m_chat, CRect(PAD, PAD, cw - PAD, ch - PAD - ITEM_H - 2));
    place(m_chatEdit, CRect(PAD, ch - PAD - ITEM_H, cw - PAD, ch - PAD));
  }
  layoutHostBar(W - 2 * PAD, hostY);
  layoutLicense();
  m_root->invalid();
}

void NinjamEditor::layoutHeader(CCoord w)
{
  const CCoord y0 = (HEADER_H - ITEM_H) / 2, y1 = y0 + ITEM_H;
  CCoord x = 4;
  auto label = [&](CTextLabel *l, CCoord padLeft) {
    const CCoord tw = textWidth(l->getFont(), l->getText().getString());
    place(l, CRect(x + padLeft, 0, x + padLeft + tw + 1, HEADER_H));
    x += padLeft + tw + 1 + 4;
  };
  label(m_title, 4);
  x += 4;
  label(m_nameLabel, 4);
  place(m_nameEdit, CRect(x, y0, x + 70, y1));
  x += 70 + 4;
  label(m_serverLabel, 4);
  const CCoord connectW = std::max(textWidth(m_fonts.button, "Disconnect"), textWidth(m_fonts.button, "Connect")) + 12;
  // the connection's state on the right: as much of it as there is room for
  const CCoord stateW = std::min<CCoord>(200, textWidth(m_fonts.value, m_connText->getText().getString()) + 2);
  const CCoord serverW = std::max<CCoord>(90, std::min<CCoord>(150, w - 4 - (x + 4 + connectW + 12 + 11 + std::max<CCoord>(60, stateW))));
  place(m_serverEdit, CRect(x, y0, x + serverW, y1));
  x += serverW + 4;
  place(m_connect, CRect(x, y0, x + connectW, y1));
  x += connectW + 12;
  const CCoord textLeft = std::max(x + 11, w - 4 - stateW);
  place(m_connText, CRect(textLeft, 0, w - 4, HEADER_H));
  place(m_connLed, CRect(textLeft - 11, 0, textLeft - 2, HEADER_H));
}

void NinjamEditor::layoutTransport(CCoord w)
{
  const CCoord y0 = (TRANSPORT_H - ITEM_H) / 2, y1 = y0 + ITEM_H;
  CCoord x = 4;
  auto label = [&](CTextLabel *l) {
    const CCoord tw = textWidth(l->getFont(), l->getText().getString());
    place(l, CRect(x + 4, 0, x + 4 + tw + 1, TRANSPORT_H));
    x += 4 + tw + 1 + 4;
  };
  auto sep = [&](Strip *s) {
    place(s, CRect(x + 3, (TRANSPORT_H - 16) / 2, x + 4, (TRANSPORT_H - 16) / 2 + 16));
    x += 7 + 4;
  };
  label(m_bpmLabel);
  const CCoord bpmW = textWidth(m_fonts.value, "888.88") + 10;
  place(m_bpm, CRect(x, y0, x + bpmW, y1));
  x += bpmW + 4;
  label(m_bpiLabel);
  const CCoord bpiW = textWidth(m_fonts.value, "64") + 22;
  place(m_bpi, CRect(x, y0, x + bpiW, y1));
  x += bpiW + 4;
  sep(m_sep[0]);
  const CCoord syncW = m_sync->preferredWidth();
  place(m_sync, CRect(x, y0, x + syncW, y1));
  x += syncW + 4;
  place(m_syncLed, CRect(x, 0, x + 7, TRANSPORT_H));
  x += 7 + 4;
  // room for the host's name and a tempo, so the row doesn't move when they change
  const CCoord syncTextW = textWidth(m_fonts.value, "Host 888.88") + 2;
  place(m_syncText, CRect(x, 0, x + syncTextW, TRANSPORT_H));
  x += syncTextW + 4;
  sep(m_sep[1]);
  const CCoord clickW = m_click->preferredWidth();
  place(m_click, CRect(x, y0, x + clickW, y1));
  x += clickW + 4;
  place(m_clickVolume, CRect(x, (TRANSPORT_H - 18) / 2, x + 18, (TRANSPORT_H - 18) / 2 + 18));
  x += 18 + 4;
  sep(m_sep[2]);
  const CCoord beatBoxW = textWidth(m_fonts.value, "88 / 88") + 10;
  place(m_beatBox, CRect(w - 4 - beatBoxW, y0, w - 4, y1));
  place(m_beats, CRect(x, 0, std::max(x + 16, w - 4 - beatBoxW - 4), TRANSPORT_H));
}

void NinjamEditor::layoutRow(Row &row, CCoord rw)
{
  const Columns c = columns(rw);
  place(row.name, CRect(c.name0, 0, c.name1, ROW_H));
  place(row.volume, CRect(c.vol0, 4, c.vol1, ROW_H - 4));
  const CCoord pc = floor((c.pan0 + c.pan1) / 2);
  place(row.pan, CRect(pc - 9, 2, pc + 9, 20));
  place(row.mute, CRect(c.mute0, 2, c.mute1, 20));
  place(row.solo, CRect(c.solo0, 2, c.solo1, 20));
  place(row.meter, CRect(c.level0, 0, c.level1, ROW_H));
}

void NinjamEditor::layoutMixer(const CRect &pane)
{
  const CCoord pw = pane.getWidth(), ph = pane.getHeight();
  const CCoord rw = pw - 2 * PAD; // a row's width
  const Columns c = columns(rw);
  const CCoord x0 = PAD;
  place(m_colChannel, CRect(x0 + c.name0, PAD, x0 + c.name1, PAD + COLS_H));
  place(m_colVolume, CRect(x0 + c.vol0, PAD, x0 + c.vol1, PAD + COLS_H));
  place(m_colPan, CRect(x0 + c.pan0 - 4, PAD, x0 + c.pan1 + 4, PAD + COLS_H));
  place(m_colLevel, CRect(x0 + c.level0, PAD, x0 + c.level1, PAD + COLS_H));

  // your channel: TX IN name | volume | the room's codec and bitrate | meter
  CCoord y = PAD + COLS_H + 2;
  place(m_myRow, CRect(x0, y, x0 + rw, y + ROW_H));
  {
    CCoord x = c.name0;
    const CCoord txW = textWidth(m_fonts.tiny, "TX") + 6, inW = textWidth(m_fonts.tiny, "IN") + 6;
    place(m_tx, CRect(x, 4, x + txW, ROW_H - 4));
    x += txW + 2;
    place(m_in, CRect(x, 4, x + inW, ROW_H - 4));
    x += inW + 4;
    // the two boxes end where the solo buttons do, each as wide as its longest text (a room the
    // plug-in can't send in plays Vorbis); MenuBox draws 4 + the text + 3 + its arrow (7) + 4
    auto boxW = [&](std::initializer_list<const char *> texts) {
      CCoord w = 0;
      for (const char *t : texts) w = std::max(w, textWidth(m_fonts.small, t));
      return w + 18;
    };
    const CCoord codecW = boxW({ "Opus", "FLAC", "Vorbis" }), bitrateW = boxW({ "510k" });
    const CCoord bitrate0 = c.solo1 - bitrateW, codec0 = bitrate0 - 4 - codecW;
    // the volume makes room for them, and keeps the others' 50 px at least: in a narrow mixer
    // it starts further left, and the name gives way
    const CCoord vol1 = std::min(c.vol1, codec0 - 5), vol0 = std::max(x + 10, std::min(c.vol0, vol1 - 50));
    place(m_myName, CRect(x, 0, std::max(x + 10, std::min(c.name1, vol0 - 5)), ROW_H));
    place(m_inputVolume, CRect(vol0, 4, std::max(vol0 + 30, vol1), ROW_H - 4));
    place(m_codec, CRect(codec0, 2, bitrate0 - 4, ROW_H - 2));
    place(m_bitrate, CRect(bitrate0, 2, c.solo1, ROW_H - 2));
    place(m_myMeter, CRect(c.level0, 0, c.level1, ROW_H));
  }
  y += ROW_H + ROW_GAP;

  // the others, then the footer
  const CCoord footerTop = ph - PAD - FOOTER_H;
  const CRect players(x0, y, x0 + rw, std::max(y + ROW_H, footerTop - 2));
  place(m_players, players);
  place(m_noPlayers, CRect(players.left + 6, players.top + 4, players.right - 6, players.top + 4 + ROW_H));
  place(m_footer, CRect(x0 + 4, footerTop, x0 + rw - 4, footerTop + FOOTER_H));

  CCoord rowY = 0;
  for (Row &row : m_rows)
  {
    place(row.container, CRect(0, rowY, rw, rowY + ROW_H));
    layoutRow(row, rw);
    rowY += ROW_H + ROW_GAP;
  }
  m_players->setContainerSize(CRect(0, 0, rw, std::max(rowY, players.getHeight())), true);
}

void NinjamEditor::layoutHostBar(CCoord w, CCoord)
{
  const CCoord y0 = (HOSTBAR_H - ITEM_H) / 2, y1 = y0 + ITEM_H;
  CCoord x = 4;
  const CCoord hostW = m_host->preferredWidth();
  place(m_host, CRect(x, y0, x + hostW, y1));
  x += hostW + 6;

  // the port's state on the right
  const CCoord portW = std::min<CCoord>(220, textWidth(m_fonts.value, m_portText->getText().getString()) + 2);
  const CCoord portLeft = w - 4 - portW;
  place(m_portText, CRect(portLeft, 0, w - 4, HOSTBAR_H));
  place(m_portLed, CRect(portLeft - 11, 0, portLeft - 2, HOSTBAR_H));

  if (m_hosting)
  {
    const CCoord countW = textWidth(m_fonts.value, m_roomCount->getText().getString()) + 2;
    place(m_roomCount, CRect(x, 0, x + countW, HOSTBAR_H));
    x += countW + 8;
    const CCoord wanW = std::min(m_wan->preferredWidth(), std::max<CCoord>(80, portLeft - 15 - x));
    place(m_wan, CRect(x, y0, x + wanW, y1));
  }
  else place(m_hostHint, CRect(x, 0, w - 4, HOSTBAR_H));
}

void NinjamEditor::layoutLicense()
{
  if (!m_license) return;
  const CCoord w = m_width / m_zoom, h = m_height / m_zoom;
  place(m_license, CRect(0, 0, w, h));
  const CCoord bw = std::min<CCoord>(w - 20, 560), bh = std::min<CCoord>(h - 20, 300);
  const CRect box(floor((w - bw) / 2), floor((h - bh) / 2), floor((w - bw) / 2) + bw, floor((h - bh) / 2) + bh);
  place(m_licenseBox, box);
  place(m_licenseTitle, CRect(8, 4, bw - 8, 22));
  place(m_licenseText, CRect(8, 24, bw - 8, bh - 8 - ITEM_H - 6));
  const CCoord declineW = m_decline->preferredWidth() + 8, acceptW = m_accept->preferredWidth() + 8;
  const CRect decline(bw - 8 - declineW, bh - 8 - ITEM_H, bw - 8, bh - 8);
  place(m_decline, decline);
  place(m_accept, CRect(decline.left - 6 - acceptW, decline.top, decline.left - 6, decline.bottom));
  m_license->invalid();
}

// ---------------------------------------------------------------------------
// keeping the views up to date
// ---------------------------------------------------------------------------

void NinjamEditor::update()
{
  if (!frame || !m_root) return;
  Snapshot s;
  engine().getSnapshot(s);

  if (s.settingsRevision != m_settingsRevision) syncSettings();
  m_settingsRevision = s.settingsRevision;

  updateHeader(s);
  updateTransport(s);

  // your channel
  const bool connected = s.state == ConnState::Connected;
  {
    std::string name = connected ? s.userName : engine().getSettings().userName;
    if (name.empty()) name = "You";
    setLabel(m_myName, name, true);
    m_myMeter->setPeak(std::max(s.localPeak[0], s.localPeak[1]));
    // the room's codec: picking one switches everyone (!codec), so only in a room
    m_codec->setEnabled(connected);
    m_codec->setShown(connected && !s.roomCodec.empty() ? s.roomCodec : "\xe2\x80\x94");
    setTooltip(m_codec, connected ? "The room's codec: pick Opus or FLAC to switch everyone (FLAC is lossless and needs "
                                    "several times the bandwidth)"
                                  : "The room's codec, Opus or FLAC: in a room, pick one to switch everyone");
    selectEntry(m_codec, s.roomCodec == "FLAC" ? 1 : connected && s.roomCodec == "Opus" ? 0 : -1);

    // the bitrate: the room's (picking one changes it for everyone), else your own; FLAC is lossless,
    // and in a room the plug-in can't send in it means nothing
    const bool flac = connected && s.roomCodec == "FLAC", mute = connected && !s.canSend;
    m_bitrate->setEnabled(!flac && !mute);
    m_bitrate->setShown(flac || mute ? "\xe2\x80\x94" : uitext::kbpsText(s.sendKbps));
    if (flac) setTooltip(m_bitrate, "The room plays FLAC, which is lossless: the bitrate is for when it plays Opus");
    else if (mute) setTooltip(m_bitrate, "This room plays " + s.roomCodec + ": the plug-in can't send here");
    else if (s.roomOpusKbps > 0)
      setTooltip(m_bitrate, "The room's Opus bitrate: pick one to change it for everyone; it applies from the next interval");
    else if (connected)
      setTooltip(m_bitrate, "The Opus bitrate you send: this server doesn't set one for the room");
    else setTooltip(m_bitrate, "Your Opus bitrate: the room's when you host, and what you send where the server doesn't set one");
    {
      int index = -1;
      for (int i = 0; i < Settings::NUM_OPUS_KBPS; i ++)
        if (Settings::OPUS_KBPS[i] == s.sendKbps) index = i;
      selectEntry(m_bitrate, index);
    }
    std::string tx = "Transmit: send your input to the room";
    if (connected && !s.canSend) tx += " (this room plays " + s.roomCodec + ": the plug-in can't send here)";
    else if (connected && !s.uploadCodec.empty()) tx += " (you send " + codecFromFourcc(s.uploadCodec) + ")";
    setTooltip(m_tx, tx);
  }

  if (s.usersRevision != m_usersRevision)
  {
    rebuildRows(s);
    m_usersRevision = s.usersRevision;
  }
  updateRows(s);
  int channels = 0;
  for (auto &u : s.users) channels += (int)u.channels.size();
  setLabel(m_noPlayers, !connected ? "Connect to a server, or host one, to hear the other players"
                        : channels ? "" : "Nobody else is here yet");
  setVisible(m_noPlayers, channels == 0);
  updateFooter(s);

  updateChat();
  updateHostBar(s);
  updateLicense(s);

  // "Copied" for a moment
  if (m_copied && now_seconds() > m_copiedUntil)
  {
    m_copied->setCopyTitle("Copy");
    m_copied = nullptr;
  }

  if (m_refocusChat)
  {
    m_refocusChat = false;
    if (!m_licenseOpen) frame->setFocusView(m_chatEdit);
  }
}

void NinjamEditor::updateHeader(const Snapshot &s)
{
  const bool online = s.state == ConnState::Connected || s.state == ConnState::Connecting;
  m_connect->setTitle(online ? "Disconnect" : "Connect");
  std::string text;
  CColor led = Colors::buttonOff();
  bool lit = false;
  switch (s.state)
  {
    case ConnState::Connected:
      text = "Connected" + (s.roomCodec.empty() ? std::string() : " \xc2\xb7 " + s.roomCodec);
      led = Colors::green();
      lit = true;
      break;
    case ConnState::Connecting:
      text = "Connecting\xe2\x80\xa6";
      led = Colors::yellow();
      lit = true;
      break;
    case ConnState::Failed:
      text = s.status;
      led = Colors::red();
      lit = true;
      break;
    default: text = "Not connected"; break;
  }
  if (m_connText->getText() != text.c_str())
  {
    setLabel(m_connText, text);
    layoutHeader(m_header->getViewSize().getWidth());
  }
  setTooltip(m_connText, s.status);
  m_connLed->setColor(led, lit);

  // hosting: the server box shows where the plug-in is connected (its own server) and can't
  // be edited; the server you join otherwise comes back when hosting stops
  const bool ownServer = s.hosting && online && !s.server.empty();
  if (!m_serverEdit->isEditing())
  {
    if (ownServer != m_showingOwnServer)
    {
      m_showingOwnServer = ownServer;
      m_serverEdit->setMouseEnabled(!ownServer);
      if (!ownServer) m_serverEdit->setText(engine().getSettings().server.c_str());
    }
    if (ownServer && m_serverEdit->getText() != s.server.c_str()) m_serverEdit->setText(s.server.c_str());
  }
}

void NinjamEditor::updateTransport(const Snapshot &s)
{
  const bool connected = s.state == ConnState::Connected;
  // the room's BPM and BPI: changing them votes, so only in a room
  m_bpm->setEnabled(connected);
  m_bpi->setEnabled(connected);
  m_bpm->setShown(connected ? (int)lround(s.bpm) : 120, uitext::bpmBoxText(connected ? s.bpm : 0));
  m_bpi->setShown(connected && s.bpi > 0 ? std::to_string(s.bpi) : "\xe2\x80\x94");
  {
    int index = -1;
    for (int i = 0; i < uitext::NUM_BPI_CHOICES; i ++)
      if (uitext::BPI_CHOICES[i] == s.bpi) index = i;
    if (index >= 0 && m_bpi->getCurrentIndex() != index)
    {
      m_syncing = true;
      m_bpi->setValue((float)index); // (setCurrent() leaves the value, which the menu checks, as it was)
      m_syncing = false;
    }
  }

  // Sync: the host's tempo, green when it is the room's, amber when not; "Link" while the host
  // follows the room through Link
  const std::string chip = s.linkLeads ? "Link" : s.hostChip.empty() ? "Host" : s.hostChip;
  setLabel(m_syncText, chip + " " + uitext::bpmBoxText(s.hostTempo));
  const bool known = s.hostTempo > 0 && connected && s.bpm > 0;
  const bool differ = known && fabs(s.hostTempo - s.bpm) > BarSync::SAME_TEMPO;
  m_syncLed->setColor(!known ? Colors::buttonOff() : differ ? Colors::yellow() : Colors::green(), known);
  setTooltip(m_syncText, !s.syncNote.empty() ? s.syncNote
                         : s.hostTempo > 0 ? s.hostLabel + "'s tempo" + (s.linkLeads ? ", which follows the room through Link" : "")
                         : s.hostLabel + " doesn't say its tempo (or isn't running the audio)");
  setTooltip(m_sync, syncTooltip(s.hostLabel, s.linkState, s.linkPeers));

  // the beat of the interval that plays; without the host's audio nothing plays (and NJClient's
  // BPI is still the one it started with), so the room's BPI and no beat
  const bool playing = connected && s.audioRunning && s.intervalBeats > 0 && s.intervalLength > 0;
  const int beats = playing ? s.intervalBeats : connected ? s.bpi : 0;
  m_beats->setPosition(playing ? s.intervalPos : 0, playing ? s.intervalLength : 0, beats);
  setLabel(m_beatBox, uitext::beatText(playing ? s.intervalPos : 0, playing ? s.intervalLength : 0, beats));
}

// the mixer's bottom line: the most important of what the editor has to say
void NinjamEditor::updateFooter(const Snapshot &s)
{
  const bool connected = s.state == ConnState::Connected;
  const std::string host = s.hostLabel.empty() ? "The host" : s.hostLabel;
  std::string text;
  bool warn = false;
  if (connected && !s.audioRunning)
  {
    // NINJAM's intervals advance with the audio; without it nobody hears us and we hear nobody
    text = "No audio from " + host + ": turn the plug-in on, or start the audio engine";
    warn = true;
  }
  else if (connected && !s.canSend)
  {
    text = "The room plays " + s.roomCodec + ": the plug-in can't send here, and plays it as silence";
    warn = true;
  }
  else if (connected && s.sendPaused)
  {
    text = host + " is stopped, so you send nothing: press play to send";
    warn = true;
  }
  else if (!s.syncNote.empty())
  {
    text = s.syncNote;
    warn = true;
  }
  else if (s.nextIntervalPpq >= 0)
    text = nextIntervalText(s.hostChip, s.nextIntervalPpq, s.timeSigNum, s.timeSigDen) + (s.linkLeads ? " (Link)" : "");
  else if (connected)
  {
    if (!s.followTempo) text = "Sync is off: the intervals keep their own time";
    else if (s.linkLeads) text = host + " follows the room through Link: its tempo, and its bars on the intervals";
    else
      switch (s.barState)
      {
        case BarSync::NoPosition: text = host + " doesn't say where its bars are"; break;
        case BarSync::Stopped: text = "The intervals line up with " + host + "'s bars when it plays"; break;
        case BarSync::OtherTempo: text = "The intervals line up with " + host + "'s bars when its tempo is the room's"; break;
        default: break;
      }
  }
  setLabel(m_footer, text, true);
  setLabelColor(m_footer, warn ? Colors::yellow() : Colors::label());
}

void NinjamEditor::syncSettings()
{
  const Settings st = engine().getSettings();
  if (!m_nameEdit->isEditing() && m_nameEdit->getText() != st.userName.c_str()) m_nameEdit->setText(st.userName.c_str());
  if (!m_serverEdit->isEditing() && !m_showingOwnServer && m_serverEdit->getText() != st.server.c_str())
    m_serverEdit->setText(st.server.c_str());
  setControlValue(m_tx, st.transmit ? 1.0f : 0.0f);
  setControlValue(m_in, st.monitor ? 1.0f : 0.0f);
  setControlValue(m_click, st.metronomeMute ? 0.0f : 1.0f);
  setControlValue(m_sync, st.followHostTempo ? 1.0f : 0.0f);
  setControlValue(m_inputVolume, gainToSlider(st.inputVolume));
  setControlValue(m_clickVolume, gainToSlider(st.metronomeVolume));
}

// checks entry `index` of a menu (none for -1) without it counting as a pick
void NinjamEditor::selectEntry(ui::MenuBox *menu, int index)
{
  if (index < 0 || menu->getCurrentIndex() == index) return;
  m_syncing = true;
  menu->setValue((float)index); // (setCurrent() leaves the value, which the menu checks, as it was)
  m_syncing = false;
}

void NinjamEditor::rebuildRows(const Snapshot &s)
{
  m_players->removeAll();
  m_rows.clear();
  int index = 0;
  for (const RemoteUserView &u : s.users)
    for (const RemoteChannelView &c : u.channels)
    {
      Row row;
      row.user = u.name;
      row.channel = c.index;
      const CRect z(0, 0, 10, 10);
      row.container = new StripContainer(z, Colors::row(), 1);
      std::string label = u.displayName;
      if (!c.name.empty()) label += " \xc2\xb7 " + c.name;
      row.name = makeLabel(z, label, m_fonts.value, Colors::value());
      row.name->setTooltipText(u.name.c_str());
      row.container->addView(row.name);
      const int32_t tag = kTagRemote + index * kRemoteFields;
      row.volume = new FaderBox(z, this, tag + kRemoteVolume, m_fonts.small);
      row.volume->format = [](float v) { return uitext::dbText(sliderToGain(v)); };
      row.volume->setDefaultValue(gainToSlider(1.0f));
      row.container->addView(row.volume);
      row.pan = new ArcKnob(z, this, tag + kRemotePan, true);
      row.pan->setTooltipText("Pan (double-click for the middle)");
      row.container->addView(row.pan);
      row.mute = new FlatButton(z, this, tag + kRemoteMute, "M", FlatButton::Toggle, m_fonts.label);
      row.mute->setTooltipText("Mute");
      row.container->addView(row.mute);
      row.solo = new FlatButton(z, this, tag + kRemoteSolo, "S", FlatButton::Toggle, m_fonts.label);
      row.solo->setOnColors(Colors::blue(), Colors::blueLine());
      row.solo->setTooltipText("Solo");
      row.container->addView(row.solo);
      row.meter = new SegmentMeter(z);
      row.container->addView(row.meter);
      m_players->addView(row.container);
      m_rows.push_back(row);
      index++;
    }
  layout();
}

void NinjamEditor::updateRows(const Snapshot &s)
{
  size_t i = 0;
  for (const RemoteUserView &u : s.users)
    for (const RemoteChannelView &c : u.channels)
    {
      if (i >= m_rows.size()) return;
      Row &row = m_rows[i++];
      if (row.user != u.name || row.channel != c.index) continue;
      setControlValue(row.volume, gainToSlider(c.volume));
      setControlValue(row.pan, (c.pan + 1.0f) * 0.5f);
      setControlValue(row.mute, c.mute ? 1.0f : 0.0f);
      setControlValue(row.solo, c.solo ? 1.0f : 0.0f);
      row.meter->setPeak(std::max(c.peak[0], c.peak[1]));
    }
}

void NinjamEditor::updateChat()
{
  std::vector<ChatLine> lines;
  const unsigned long long rev = engine().getChat(lines, 500);
  if (rev == m_chatRevision) return;
  m_chatRevision = rev;

  std::vector<TextLinesView::Line> out;
  out.reserve(lines.size());
  for (const ChatLine &l : lines)
  {
    TextLinesView::Line v;
    v.text = l.text;
    v.textColor = Colors::text();
    switch (l.kind)
    {
      case ChatLine::Message: v.who = l.who; v.whoColor = Colors::blue(); break;
      case ChatLine::Action: v.who = "* " + l.who; v.whoColor = Colors::blue(); v.textColor = Colors::status(); break;
      case ChatLine::Private: v.who = l.who.compare(0, 3, "-> ") == 0 ? l.who : l.who + " (private)"; v.whoColor = Colors::purple(); break;
      case ChatLine::Topic: v.textColor = Colors::blue(); break;
      case ChatLine::Server: v.textColor = Colors::purple(); break; // the server, votes
      case ChatLine::Error: v.textColor = Colors::red(); break;
      default: v.textColor = Colors::label(); break; // joins, leaves, the plug-in's own notes
    }
    out.push_back(v);
  }
  m_chat->setLines(out);
}

void NinjamEditor::updateHostBar(const Snapshot &s)
{
  if (m_hostPending)
  {
    const bool done = !s.hostBusy && (s.hosting == (m_host->getValue() > 0.5f) || now_seconds() - m_hostPendingSince > 3.0);
    if (done) m_hostPending = false;
  }
  if (!m_hostPending) setControlValue(m_host, s.hosting ? 1.0f : 0.0f);

  bool relayout = s.hosting != m_hosting;
  m_hosting = s.hosting;
  setVisible(m_roomCount, s.hosting);
  setVisible(m_wan, s.hosting);
  setVisible(m_hostHint, !s.hosting);

  std::string port, tip;
  CColor portColor = Colors::status(), led = Colors::buttonOff();
  bool lit = false;
  if (s.hosting)
  {
    const std::string count = std::to_string(s.hostUsers) + " in room";
    if (m_roomCount->getText() != count.c_str())
    {
      setLabel(m_roomCount, count);
      relayout = true;
    }
    // the router's word for the public address, else a "what's my IP" service's
    const bool forwarded = !s.hostPublicAddress.empty();
    const std::string wan = forwarded ? s.hostPublicAddress : s.hostLookedUpAddress;
    m_wan->setAddress(wan, s.hostLookupPending || s.hostPortmapPending ? "looking it up\xe2\x80\xa6" : "not known");
    if (wan != m_wanShown)
    {
      m_wanShown = wan;
      relayout = true;
    }
    const std::string portNo = std::to_string(s.hostPort);
    const std::string here = s.hostLocalAddress.empty() ? std::string("this computer")
                                                        : "this computer (" + s.hostLocalAddress.substr(0, s.hostLocalAddress.rfind(':')) + ")";
    if (forwarded || s.hostJoinedFromInternet)
    {
      port = "Port open";
      portColor = led = Colors::green();
      lit = true;
      tip = forwarded ? "Your router forwards port " + portNo + ": players on the internet join at " + wan
                      : "A player joined from the internet, so port " + portNo + " is open" +
                          (wan.empty() ? std::string() : ": players join at " + wan);
    }
    else if (s.hostPortmapPending)
    {
      port = "Asking the router\xe2\x80\xa6";
      tip = "Asking your router to forward port " + portNo;
    }
    else
    {
      // the router didn't open it: maybe it is forwarded by hand, which nothing here can see
      port = "Forward port " + portNo;
      portColor = led = Colors::yellow();
      lit = true;
      tip = "Your router didn't open port " + portNo + " for Jamsa. Forward port " + portNo + " (TCP) to " + here +
            " in your router's settings" + (wan.empty() ? std::string() : ", and players on the internet join at " + wan) +
            ". If it's forwarded already, they can join now.";
      if (!s.hostNote.empty()) tip += " (The router said: " + s.hostNote + ")";
    }
  }
  else
  {
    std::string hint;
    if (s.hostBusy) hint = m_host->getValue() > 0.5f ? "Starting the server\xe2\x80\xa6" : "Stopping the server\xe2\x80\xa6";
    else if (s.hostedElsewherePort)
      hint = "Another Jamsa here hosts a server: join localhost:" + std::to_string(s.hostedElsewherePort);
    else hint = "Host a server in this plug-in: anyone can join without an account, and one vote changes BPM or BPI";
    setLabel(m_hostHint, hint, true);
  }
  if (m_portText->getText() != port.c_str())
  {
    setLabel(m_portText, port);
    relayout = true;
  }
  setLabelColor(m_portText, portColor);
  setTooltip(m_portText, tip);
  setTooltip(m_portLed, tip);
  m_portLed->setColor(led, lit);
  setVisible(m_portLed, s.hosting);
  if (relayout) layoutHostBar(m_hostBar->getViewSize().getWidth(), 0);
}

void NinjamEditor::updateLicense(const Snapshot &s)
{
  if (s.licensePending && !m_licenseOpen)
  {
    const CRect z(0, 0, 10, 10);
    m_license = new CViewContainer(z);
    m_license->setBackgroundColor(CColor(0, 0, 0, 150));
    m_licenseBox = new StripContainer(z, Colors::strip());
    m_license->addView(m_licenseBox);
    m_licenseTitle = makeLabel(z, "This server asks you to accept its license", m_fonts.bold, Colors::value());
    m_licenseBox->addView(m_licenseTitle);
    m_licenseText = new TextLinesView(z, m_fonts.chat, m_fonts.chatBold);
    m_licenseText->setStartAtTop(true);
    m_licenseText->setBackground(Colors::pane());
    m_licenseBox->addView(m_licenseText);
    m_accept = new FlatButton(z, this, kTagAccept, "Accept and join", FlatButton::Push, m_fonts.button);
    m_licenseBox->addView(m_accept);
    m_decline = new FlatButton(z, this, kTagDecline, "Decline", FlatButton::Push, m_fonts.button);
    m_licenseBox->addView(m_decline);
    layoutLicense();
    if (auto id = frame->beginModalViewSession(m_license))
    {
      m_licenseSession = *id;
      m_licenseOpen = true;
    }
    else
    {
      m_license->forget();
      m_license = nullptr;
      return;
    }
    m_licenseShown.clear();
  }
  if (m_licenseOpen && s.licensePending && s.licenseText != m_licenseShown)
  {
    m_licenseShown = s.licenseText;
    std::vector<TextLinesView::Line> lines;
    TextLinesView::Line l;
    l.text = s.licenseText;
    l.textColor = Colors::text();
    lines.push_back(l);
    m_licenseText->setLines(lines);
    m_licenseText->scrollToStart();
  }
  if (!s.licensePending && m_licenseOpen)
  {
    frame->endModalViewSession(m_licenseSession); // removes and releases the view
    m_licenseOpen = false;
    m_license = m_licenseBox = nullptr;
    m_licenseTitle = nullptr;
    m_licenseText = nullptr;
    m_accept = m_decline = nullptr;
  }
}

// ---------------------------------------------------------------------------
// what the user does
// ---------------------------------------------------------------------------

void NinjamEditor::connectOrDisconnect()
{
  Snapshot s;
  engine().getSnapshot(s);
  if (s.state == ConnState::Connected || s.state == ConnState::Connecting)
  {
    engine().disconnect();
    return;
  }
  engine().connect(m_serverEdit->getText().getString(), m_nameEdit->getText().getString());
}

void NinjamEditor::sendChatLine()
{
  const std::string text = m_chatEdit->getText().getString();
  if (text.find_first_not_of(" \t") == std::string::npos) return;
  engine().sendChat(text);
  m_chatEdit->setText("");
  m_refocusChat = true;
}

void NinjamEditor::copyAddress(AddressBox *box)
{
  if (!box || box->address().empty()) return;
  const bool ok = clipboard::setText(box->address());
  box->setCopyTitle(ok ? "Copied" : "Failed");
  setTooltip(box, ok ? "Copied " + box->address() : "There is no clipboard here: the address is " + box->address());
  if (m_copied && m_copied != box) m_copied->setCopyTitle("Copy");
  m_copied = box;
  m_copiedUntil = now_seconds() + 1.5;
}

void NinjamEditor::valueChanged(CControl *control)
{
  const int32_t tag = control->getTag();
  const float v = control->getValue();
  if (m_syncing) return;
  switch (tag)
  {
    case kTagName:
      engine().setUserName(m_nameEdit->getText().getString());
      return;
    case kTagServer:
      engine().setServerText(m_serverEdit->getText().getString());
      if (m_serverEdit->bWasReturnPressed)
      {
        Snapshot s;
        engine().getSnapshot(s);
        if (s.state != ConnState::Connected && s.state != ConnState::Connecting) connectOrDisconnect();
      }
      return;
    case kTagConnect:
      if (v > 0.5f) connectOrDisconnect();
      return;
    case kTagTransmit: engine().setTransmit(v > 0.5f); return;
    case kTagSync: engine().setFollowHostTempo(v > 0.5f); return;
    case kTagMonitor: engine().setMonitor(v > 0.5f); return;
    case kTagBitrate:
    {
      const int i = m_bitrate->getCurrentIndex();
      if (i >= 0 && i < Settings::NUM_OPUS_KBPS) engine().setOpusKbps(Settings::OPUS_KBPS[i]);
      return;
    }
    case kTagCodec:
    {
      const int i = m_codec->getCurrentIndex();
      Snapshot s;
      engine().getSnapshot(s);
      const char *codec = i == 1 ? "FLAC" : "Opus";
      if (i >= 0 && s.state == ConnState::Connected && s.roomCodec != codec) engine().setRoomCodec(i == 1 ? "flac" : "opus");
      return;
    }
    case kTagInputVolume: engine().setInputVolume(sliderToGain(v)); return;
    case kTagClickVolume:
    case kTagClick:
      engine().setMetronome(sliderToGain(m_clickVolume->getValue()), m_click->getValue() < 0.5f);
      return;
    case kTagBpm:
    {
      // a vote, dragged to or typed (with Return)
      int bpm = 0;
      if (m_bpm->committedByDrag()) bpm = m_bpm->draggedValue();
      else if (!m_bpm->bWasReturnPressed || !uitext::parseBpm(m_bpm->getText().getString(), &bpm)) return;
      Snapshot s;
      engine().getSnapshot(s);
      if (s.state == ConnState::Connected && bpm != (int)lround(s.bpm)) engine().vote("bpm", bpm);
      return;
    }
    case kTagBpi:
    {
      const int i = m_bpi->getCurrentIndex();
      Snapshot s;
      engine().getSnapshot(s);
      if (i >= 0 && i < uitext::NUM_BPI_CHOICES && s.state == ConnState::Connected && uitext::BPI_CHOICES[i] != s.bpi)
        engine().vote("bpi", uitext::BPI_CHOICES[i]);
      return;
    }
    case kTagChat:
      if (m_chatEdit->bWasReturnPressed) sendChatLine();
      return;
    case kTagHost:
      m_hostPending = true;
      m_hostPendingSince = now_seconds();
      engine().setHosting(v > 0.5f);
      return;
    case kTagCopyWan:
      if (v > 0.5f) copyAddress(m_wan);
      return;
    case kTagAccept:
      if (v > 0.5f) engine().answerLicense(true);
      return;
    case kTagDecline:
      if (v > 0.5f) engine().answerLicense(false);
      return;
    default: break;
  }

  if (tag >= kTagRemote)
  {
    const size_t index = (size_t)((tag - kTagRemote) / kRemoteFields);
    const int field = (tag - kTagRemote) % kRemoteFields;
    if (index >= m_rows.size()) return;
    const Row &row = m_rows[index];
    switch (field)
    {
      case kRemoteVolume: engine().setRemote(row.user, row.channel, Engine::RemoteVolume, sliderToGain(v)); break;
      case kRemotePan: engine().setRemote(row.user, row.channel, Engine::RemotePan, v * 2.0f - 1.0f); break;
      case kRemoteMute: engine().setRemote(row.user, row.channel, Engine::RemoteMute, v > 0.5f ? 1.0f : 0.0f); break;
      case kRemoteSolo: engine().setRemote(row.user, row.channel, Engine::RemoteSolo, v > 0.5f ? 1.0f : 0.0f); break;
    }
  }
}

} // namespace ninjam_vst3
