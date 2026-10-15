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

#include "vstgui/lib/cclipboard.h"
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

#include "plugin.h"

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
  kTagMetroVolume,
  kTagMetroMute,
  kTagChat,
  kTagSend,
  kTagHost,
  kTagCopy,
  kTagAccept,
  kTagDecline,
  kTagBitrate,
  kTagFollow,
  kTagRemote = 1000, // + row * 8 + field
};
enum { kRemoteVolume, kRemotePan, kRemoteMute, kRemoteSolo, kRemoteFields = 8 };

static const CCoord PAD = 12, GAP = 8, ROW_H = 30, SCROLLBAR_W = 10;

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

static void setButtonTitle(CTextButton *b, const char *title)
{
  if (b && b->getTitle() != title) b->setTitle(title);
}

static void setVisible(CView *v, bool on)
{
  if (v && v->isVisible() != on) v->setVisible(on);
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

NinjamEditor::NinjamEditor(NinjamPlugin *plugin)
  : VSTGUIEditor(static_cast<Steinberg::Vst::EditController *>(plugin)), m_plugin(plugin)
{
  const Settings s = engine().getSettings();
  if (s.editorWidth > 0 && s.editorHeight > 0)
  {
    m_width = std::min<CCoord>(MAX_WIDTH, std::max<CCoord>(MIN_WIDTH, s.editorWidth));
    m_height = std::min<CCoord>(MAX_HEIGHT, std::max<CCoord>(MIN_HEIGHT, s.editorHeight));
  }
  ViewRect r(0, 0, (int32)m_width, (int32)m_height);
  setRect(r);
  setIdleRate(33);

  const UTF8String face = kNormalFont ? kNormalFont->getName() : UTF8String("Arial");
  m_font = makeOwned<CFontDesc>(face, 13);
  m_bold = makeOwned<CFontDesc>(face, 13, kBoldFace);
  m_small = makeOwned<CFontDesc>(face, 11);
  m_smallBold = makeOwned<CFontDesc>(face, 11, kBoldFace);
  m_title = makeOwned<CFontDesc>(face, 20, kBoldFace);
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
  frame = new CFrame(CRect(0, 0, m_width, m_height), this);
  frame->setBackgroundColor(Colors::background());
  frame->enableTooltips(true);
  buildViews();
  if (m_scale != 1.0) frame->setZoom(m_scale);

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
  layout();
  syncSettings();
  m_usersRevision = m_settingsRevision = ~0u;
  m_chatRevision = ~0ull;
  m_bitrateState = -1;
  update();
  return true;
}

void PLUGIN_API NinjamEditor::close()
{
  if (frame)
  {
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
    frame->setZoom(m_scale);
    if (plugFrame) plugFrame->resizeView(this, &r);
  }
  return kResultTrue;
#endif
}

void NinjamEditor::setLogicalSize(CCoord width, CCoord height)
{
  m_width = std::max<CCoord>(1, width);
  m_height = std::max<CCoord>(1, height);
  engine().setEditorSize((int)lround(m_width), (int)lround(m_height));
  if (m_root) layout();
}

// ---------------------------------------------------------------------------
// views
// ---------------------------------------------------------------------------

void NinjamEditor::buildViews()
{
  const CRect z(0, 0, 10, 10);
  m_root = new CViewContainer(CRect(0, 0, m_width, m_height));
  m_root->setBackgroundColor(Colors::background());
  frame->addView(m_root);

  m_titleLabel = makeLabel(z, "NINJAM", m_title, Colors::text());
  m_root->addView(m_titleLabel);
  m_nameLabel = makeLabel(z, "Name", m_font, Colors::dim(), kRightText);
  m_root->addView(m_nameLabel);
  m_nameEdit = makeEdit(z, this, kTagName, "", m_font, "Your name");
  m_root->addView(m_nameEdit);
  m_serverLabel = makeLabel(z, "Server", m_font, Colors::dim(), kRightText);
  m_root->addView(m_serverLabel);
  m_serverEdit = makeEdit(z, this, kTagServer, "", m_font, "host:port");
  m_root->addView(m_serverEdit);
  m_connect = makeButton(z, this, kTagConnect, "Connect", false, m_bold);
  m_root->addView(m_connect);

  m_status = makeLabel(z, "Not connected", m_font, Colors::dim());
  m_root->addView(m_status);
  m_room = makeLabel(z, "", m_font, Colors::text(), kRightText);
  m_root->addView(m_room);
  m_interval = new IntervalView(z, m_small);
  m_root->addView(m_interval);

  // mixer: our channel, the metronome, the other players
  m_mixer = new PanelView(z, Colors::panel(), Colors::panelLine(), 6);
  m_root->addView(m_mixer);
  m_localHeader = makeLabel(z, "YOUR CHANNEL", m_smallBold, Colors::dim());
  m_mixer->addView(m_localHeader);
  m_codecLabel = makeLabel(z, "", m_small, Colors::dim(), kRightText);
  m_mixer->addView(m_codecLabel);
  m_transmit = makeButton(z, this, kTagTransmit, "Transmit", true, m_font);
  m_transmit->setTooltipText("Send your input to the other players");
  m_mixer->addView(m_transmit);
  m_localMeter = new MeterView(z);
  m_mixer->addView(m_localMeter);
  m_inputVolumeLabel = makeLabel(z, "Volume", m_small, Colors::dim(), kRightText);
  m_mixer->addView(m_inputVolumeLabel);
  m_inputVolume = makeSlider(z, this, kTagInputVolume, false);
  m_inputVolume->setTooltipText("The volume of what you send (double-click for 0 dB)");
  m_mixer->addView(m_inputVolume);
  m_monitor = makeButton(z, this, kTagMonitor, "Monitor", true, m_font);
  m_monitor->setTooltipText("Hear your input in the plug-in's output");
  m_mixer->addView(m_monitor);
  m_bitrateLabel = makeLabel(z, "Opus kbps", m_small, Colors::dim(), kRightText);
  m_mixer->addView(m_bitrateLabel);
  m_bitrate = new CSegmentButton(z, this, kTagBitrate);
  m_bitrate->setSelectionMode(CSegmentButton::SelectionMode::kSingle);
  for (int k : Settings::OPUS_KBPS)
  {
    CSegmentButton::Segment seg;
    seg.name = std::to_string(k).c_str();
    m_bitrate->addSegment(std::move(seg));
  }
  m_bitrate->setFont(m_small);
  m_bitrate->setRoundRadius(4);
  m_bitrate->setFrameWidth(1);
  m_bitrate->setFrameColor(Colors::panelLine());
  m_bitrate->setTextColor(Colors::text());
  m_bitrate->setTextColorHighlighted(CColor(255, 255, 255));
  m_bitrate->setGradient(VSTGUI::owned(CGradient::create(0, 1, CColor(60, 67, 78), CColor(48, 54, 63))));
  m_bitrate->setGradientHighlighted(VSTGUI::owned(CGradient::create(0, 1, CColor(52, 150, 106), CColor(40, 122, 86))));
  m_bitrate->setTooltipText("The bitrate you send in rooms that play Opus. A change applies from the next interval. "
                            "FLAC is lossless and doesn't use it.");
  m_mixer->addView(m_bitrate);
  m_bitrateNote = makeLabel(z, "", m_small, Colors::dim());
  m_mixer->addView(m_bitrateNote);

  m_metroHeader = makeLabel(z, "METRONOME AND TEMPO", m_smallBold, Colors::dim());
  m_mixer->addView(m_metroHeader);
  m_metroMute = makeButton(z, this, kTagMetroMute, "Mute", true, m_font);
  m_mixer->addView(m_metroMute);
  m_metroVolumeLabel = makeLabel(z, "Volume", m_small, Colors::dim(), kRightText);
  m_mixer->addView(m_metroVolumeLabel);
  m_metroVolume = makeSlider(z, this, kTagMetroVolume, false);
  m_metroVolume->setDefaultValue(gainToSlider(0.5f));
  m_mixer->addView(m_metroVolume);
  m_follow = makeButton(z, this, kTagFollow, "Follow Ableton's tempo", true, m_font);
  m_follow->setTooltipText("When you change Ableton's tempo, vote the room to it; and while Ableton plays, start the "
                           "intervals on its bars (a multiple of BPI beats from bar 1)");
  m_mixer->addView(m_follow);
  m_followStatus = makeLabel(z, "", m_small, Colors::dim());
  m_mixer->addView(m_followStatus);
  m_syncNote = makeLabel(z, "", m_small, Colors::warn());
  m_mixer->addView(m_syncNote);

  m_playersHeader = makeLabel(z, "PLAYERS", m_smallBold, Colors::dim());
  m_mixer->addView(m_playersHeader);
  m_colName = makeLabel(z, "Name", m_small, Colors::dim());
  m_mixer->addView(m_colName);
  m_colVolume = makeLabel(z, "Volume", m_small, Colors::dim(), kCenterText);
  m_mixer->addView(m_colVolume);
  m_colPan = makeLabel(z, "Pan", m_small, Colors::dim(), kCenterText);
  m_mixer->addView(m_colPan);
  m_colLevel = makeLabel(z, "M    S        Level", m_small, Colors::dim(), kRightText);
  m_mixer->addView(m_colLevel);
  m_players = new CScrollView(z, CRect(0, 0, 10, 10), CScrollView::kVerticalScrollbar | CScrollView::kDontDrawFrame | CScrollView::kAutoHideScrollbars);
  m_players->setBackgroundColor(Colors::field());
  m_players->setScrollbarWidth(SCROLLBAR_W);
  if (CScrollbar *sb = m_players->getVerticalScrollbar())
  {
    sb->setScrollerColor(Colors::panelLine());
    sb->setBackgroundColor(Colors::field());
    sb->setFrameColor(Colors::field());
  }
  m_mixer->addView(m_players);
  m_noPlayers = makeLabel(z, "", m_font, Colors::dim(), kCenterText);
  m_mixer->addView(m_noPlayers);

  // chat
  m_chatPanel = new PanelView(z, Colors::panel(), Colors::panelLine(), 6);
  m_root->addView(m_chatPanel);
  m_chatHeader = makeLabel(z, "CHAT", m_smallBold, Colors::dim());
  m_chatPanel->addView(m_chatHeader);
  m_chat = new TextLinesView(z, m_font, m_bold);
  m_chatPanel->addView(m_chat);
  m_chatEdit = makeEdit(z, this, kTagChat, "", m_font, "Chat, or !vote bpm 120, !codec flac, /me ...");
  m_chatPanel->addView(m_chatEdit);
  m_send = makeButton(z, this, kTagSend, "Send", false, m_font);
  m_chatPanel->addView(m_send);

  // the built-in server
  m_hostBar = new PanelView(z, Colors::panel(), Colors::panelLine(), 6);
  m_root->addView(m_hostBar);
  m_host = makeButton(z, this, kTagHost, "Host a server", true, m_bold);
  m_host->setTooltipText("Start a NINJAM server in this plug-in and join it");
  m_hostBar->addView(m_host);
  m_hostText = makeLabel(z, "", m_font, Colors::dim());
  m_hostBar->addView(m_hostText);
  m_copy = makeButton(z, this, kTagCopy, "Copy address", false, m_font);
  m_copy->setVisible(false);
  m_hostBar->addView(m_copy);
}

void NinjamEditor::layout()
{
  if (!m_root) return;
  // below the minimum size the editor is cut off rather than squeezed
  const CCoord w = std::max<CCoord>(m_width, MIN_WIDTH), h = std::max<CCoord>(m_height, MIN_HEIGHT);
  m_root->setViewSize(CRect(0, 0, m_width, m_height));
  m_root->setMouseableArea(CRect(0, 0, m_width, m_height));

  auto place = [](CView *v, const CRect &r) {
    v->setViewSize(r);
    v->setMouseableArea(r);
  };

  // top bar
  CCoord y = PAD;
  const CCoord rowH = 28;
  place(m_titleLabel, CRect(PAD, y - 2, PAD + 100, y + rowH));
  place(m_nameLabel, CRect(PAD + 100, y, PAD + 146, y + rowH));
  place(m_nameEdit, CRect(PAD + 152, y, PAD + 292, y + rowH));
  place(m_serverLabel, CRect(PAD + 298, y, PAD + 348, y + rowH));
  const CCoord connectW = 112;
  const CRect connect(w - PAD - connectW, y, w - PAD, y + rowH);
  place(m_connect, connect);
  place(m_serverEdit, CRect(PAD + 354, y, connect.left - GAP, y + rowH));

  y += rowH + 8;
  place(m_status, CRect(PAD, y, w - PAD - 330, y + 20));
  place(m_room, CRect(w - PAD - 320, y, w - PAD, y + 20));
  y += 26;
  place(m_interval, CRect(PAD, y, w - PAD, y + 26));
  y += 26 + 10;

  // host bar at the bottom, mixer and chat between
  const CCoord hostH = 38;
  const CRect hostBar(PAD, h - PAD - hostH, w - PAD, h - PAD);
  const CCoord mainBottom = hostBar.top - GAP;
  const CCoord mixerW = std::max<CCoord>(450, floor((w - 2 * PAD - GAP) * 0.54));
  place(m_mixer, CRect(PAD, y, PAD + mixerW, mainBottom));
  place(m_chatPanel, CRect(PAD + mixerW + GAP, y, w - PAD, mainBottom));
  place(m_hostBar, hostBar);

  // mixer
  {
    const CCoord mw = mixerW, mh = mainBottom - y, ip = 10;
    place(m_localHeader, CRect(ip, 8, mw / 2, 24));
    place(m_codecLabel, CRect(mw / 2, 8, mw - ip, 24));
    CCoord ry = 28;
    const CRect vol(mw - ip - 150, ry + 4, mw - ip, ry + 22);
    const CRect volLabel(vol.left - 58, ry, vol.left - 6, ry + 26);
    place(m_transmit, CRect(ip, ry, ip + 96, ry + 26));
    place(m_monitor, CRect(ip + 96 + GAP, ry, ip + 2 * 96 + GAP, ry + 26));
    place(m_inputVolumeLabel, volLabel);
    place(m_inputVolume, vol);
    ry += 32;
    const CCoord segW = 6 * 42;
    place(m_bitrateLabel, CRect(ip, ry, ip + 70, ry + 22));
    place(m_bitrate, CRect(ip + 76, ry, ip + 76 + segW, ry + 22));
    place(m_bitrateNote, CRect(ip + 76 + segW + GAP, ry, mw - ip, ry + 22));
    ry += 30;
    place(m_localMeter, CRect(ip, ry, mw - ip, ry + 12));

    ry += 22;
    place(m_metroHeader, CRect(ip, ry, mw - ip, ry + 16));
    ry += 20;
    place(m_metroMute, CRect(ip, ry, ip + 96, ry + 26));
    place(m_metroVolumeLabel, CRect(volLabel.left, ry, volLabel.right, ry + 26));
    place(m_metroVolume, CRect(vol.left, ry + 4, vol.right, ry + 22));
    ry += 32;
    place(m_follow, CRect(ip, ry, ip + 180, ry + 26));
    place(m_followStatus, CRect(ip + 180 + GAP, ry, mw - ip, ry + 26));
    ry += 28;
    place(m_syncNote, CRect(ip, ry, mw - ip, ry + 16));

    ry += 24;
    place(m_playersHeader, CRect(ip, ry, mw - ip, ry + 16));
    ry += 18;
    const CRect players(ip, ry + 16, mw - ip, std::max(ry + 16 + ROW_H, mh - ip));
    // column titles line up with the rows (see layoutRow)
    const CCoord rw = players.getWidth() - SCROLLBAR_W;
    const CCoord meterR = rw - 6, soloR = meterR - 64 - 8, muteR = soloR - 26 - 4, panR = muteR - 26 - 8, volR = panR - 70 - 8;
    place(m_colName, CRect(players.left + 8, ry, players.left + volR - 120 - 8, ry + 16));
    place(m_colVolume, CRect(players.left + volR - 120, ry, players.left + volR, ry + 16));
    place(m_colPan, CRect(players.left + panR - 70, ry, players.left + panR, ry + 16));
    place(m_colLevel, CRect(players.left + muteR - 30, ry, players.left + meterR, ry + 16));
    place(m_players, players);
    place(m_noPlayers, CRect(players.left + 10, players.top + 20, players.right - 10, players.top + 50));

    CCoord rowY = 0;
    for (Row &row : m_rows)
    {
      place(row.container, CRect(0, rowY, rw, rowY + ROW_H));
      layoutRow(row, rw);
      rowY += ROW_H;
    }
    m_players->setContainerSize(CRect(0, 0, rw, std::max(rowY, players.getHeight())), true);
  }

  // chat
  {
    const CRect cp = m_chatPanel->getViewSize();
    const CCoord cw = cp.getWidth(), ch = cp.getHeight(), ip = 10;
    place(m_chatHeader, CRect(ip, 8, cw - ip, 24));
    place(m_chatEdit, CRect(ip, ch - ip - 28, cw - ip - 72 - GAP, ch - ip));
    place(m_send, CRect(cw - ip - 72, ch - ip - 28, cw - ip, ch - ip));
    place(m_chat, CRect(ip, 28, cw - ip, ch - ip - 28 - GAP));
  }

  // host bar
  {
    const CCoord hw = hostBar.getWidth();
    place(m_host, CRect(5, 5, 155, hostH - 5));
    const CRect copy(hw - 5 - 116, 5, hw - 5, hostH - 5);
    place(m_copy, copy);
    place(m_hostText, CRect(165, 0, (m_copy->isVisible() ? copy.left : hw) - GAP, hostH));
  }

  layoutLicense();
  m_root->invalid();
}

void NinjamEditor::layoutRow(Row &row, CCoord rw)
{
  const CCoord meterR = rw - 6, soloR = meterR - 64 - 8, muteR = soloR - 26 - 4, panR = muteR - 26 - 8, volR = panR - 70 - 8;
  auto place = [](CView *v, const CRect &r) {
    v->setViewSize(r);
    v->setMouseableArea(r);
  };
  place(row.name, CRect(8, 0, volR - 120 - 8, ROW_H));
  place(row.volume, CRect(volR - 120, 7, volR, ROW_H - 7));
  place(row.pan, CRect(panR - 70, 7, panR, ROW_H - 7));
  place(row.mute, CRect(muteR - 26, 4, muteR, ROW_H - 4));
  place(row.solo, CRect(soloR - 26, 4, soloR, ROW_H - 4));
  place(row.meter, CRect(meterR - 64, 9, meterR, ROW_H - 9));
}

void NinjamEditor::layoutLicense()
{
  if (!m_license) return;
  const CCoord w = m_width, h = m_height;
  m_license->setViewSize(CRect(0, 0, w, h));
  m_license->setMouseableArea(CRect(0, 0, w, h));
  const CCoord bw = std::min<CCoord>(w - 60, 700), bh = std::min<CCoord>(h - 60, 480);
  const CRect box(floor((w - bw) / 2), floor((h - bh) / 2), floor((w - bw) / 2) + bw, floor((h - bh) / 2) + bh);
  m_licenseBox->setViewSize(box);
  m_licenseBox->setMouseableArea(box);
  m_licenseTitle->setViewSize(CRect(16, 12, bw - 16, 36));
  m_licenseText->setViewSize(CRect(16, 42, bw - 16, bh - 16 - 30 - 12));
  const CRect decline(bw - 16 - 110, bh - 16 - 30, bw - 16, bh - 16);
  m_decline->setViewSize(decline);
  m_decline->setMouseableArea(decline);
  const CRect accept(decline.left - 10 - 140, decline.top, decline.left - 10, decline.bottom);
  m_accept->setViewSize(accept);
  m_accept->setMouseableArea(accept);
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

  // connection
  setLabel(m_status, s.status, true);
  setLabelColor(m_status, s.state == ConnState::Connected ? Colors::accent()
                          : s.state == ConnState::Connecting ? Colors::warn()
                          : s.state == ConnState::Failed ? Colors::error() : Colors::dim());
  const bool online = s.state == ConnState::Connected || s.state == ConnState::Connecting;
  setButtonTitle(m_connect, online ? "Disconnect" : "Connect");

  std::string room;
  if (s.state == ConnState::Connected && s.bpi > 0)
  {
    char buf[128];
    snprintf(buf, sizeof(buf), "%.0f BPM  \xc2\xb7  %d BPI  \xc2\xb7  room codec %s", s.bpm, s.bpi, s.roomCodec.c_str());
    room = buf;
  }
  setLabel(m_room, room);
  m_interval->setPosition(s.intervalPos, s.intervalLength, s.state == ConnState::Connected ? s.intervalBeats : 0);
  // NINJAM's intervals advance with the audio; without it nobody hears us and we hear nobody
  m_interval->setMessage(s.state == ConnState::Connected && !s.audioRunning
                         ? "The host is not running the plug-in's audio: turn the plug-in on, or start the audio engine" : "");

  if (s.state != ConnState::Connected) setLabel(m_codecLabel, "");
  else if (!s.canSend) setLabel(m_codecLabel, "you can't send here (no Opus)", true);
  else setLabel(m_codecLabel, "you send " + codecFromFourcc(s.uploadCodec));
  setLabelColor(m_codecLabel, s.state == ConnState::Connected && !s.canSend ? Colors::warn() : Colors::dim());
  m_localMeter->setLevels(s.localPeak[0], s.localPeak[1]);
  updateFollow(s);

  // FLAC is lossless: the bitrate is for Opus rooms
  const int bitrateState = s.state == ConnState::Connected && s.roomCodec == "FLAC" ? 1 : 0;
  if (bitrateState != m_bitrateState)
  {
    m_bitrateState = bitrateState;
    m_bitrate->setMouseEnabled(bitrateState == 0);
    m_bitrate->setAlphaValue(bitrateState == 0 ? 1.0f : 0.35f);
    setLabel(m_bitrateNote, bitrateState ? "lossless (FLAC)" : "");
  }

  if (s.usersRevision != m_usersRevision)
  {
    rebuildRows(s);
    m_usersRevision = s.usersRevision;
  }
  updateRows(s);

  int channels = 0;
  for (auto &u : s.users) channels += (int)u.channels.size();
  setLabel(m_playersHeader, s.users.empty() ? std::string("PLAYERS") : "PLAYERS (" + std::to_string(s.users.size()) + ")");
  setLabel(m_noPlayers, s.state != ConnState::Connected ? "Connect to a server to hear the other players."
                        : channels ? "" : "Nobody else is here yet.");
  setVisible(m_noPlayers, channels == 0);

  updateChat();
  updateHostBar(s);
  updateLicense(s);

  if (m_refocusChat)
  {
    m_refocusChat = false;
    if (!m_licenseOpen) frame->setFocusView(m_chatEdit);
  }
}

void NinjamEditor::updateFollow(const Snapshot &s)
{
  const std::string &host = s.hostLabel;
  std::string text;
  CColor color = Colors::dim();
  if (!s.followTempo) text = "Off";
  else if (s.state != ConnState::Connected) text = "Starts when you're in a room";
  else
    switch (s.barState)
    {
      case BarSync::NoPosition: text = host + " doesn't say where its bars are"; break;
      case BarSync::Stopped: text = "Lines up with " + host + "'s bars when it plays"; break;
      case BarSync::OtherTempo:
        text = "Bars line up when " + host + "'s tempo is the room's";
        color = Colors::warn();
        break;
      case BarSync::Lining: text = "Lining up with " + host + "'s bars at the next interval"; break;
      case BarSync::Aligned:
        text = "Intervals start on " + host + "'s bars";
        color = Colors::accent();
        break;
      default: text = s.audioRunning ? "" : "Waits for the audio"; break;
    }
  setLabel(m_followStatus, text, true);
  setLabelColor(m_followStatus, color);
  setLabel(m_syncNote, s.syncNote, true);
}

void NinjamEditor::syncSettings()
{
  const Settings st = engine().getSettings();
  if (!m_nameEdit->isEditing() && m_nameEdit->getText() != st.userName.c_str()) m_nameEdit->setText(st.userName.c_str());
  if (!m_serverEdit->isEditing() && m_serverEdit->getText() != st.server.c_str()) m_serverEdit->setText(st.server.c_str());
  setControlValue(m_transmit, st.transmit ? 1.0f : 0.0f);
  setControlValue(m_monitor, st.monitor ? 1.0f : 0.0f);
  setControlValue(m_metroMute, st.metronomeMute ? 1.0f : 0.0f);
  setControlValue(m_follow, st.followHostTempo ? 1.0f : 0.0f);
  setControlValue(m_inputVolume, gainToSlider(st.inputVolume));
  setControlValue(m_metroVolume, gainToSlider(st.metronomeVolume));
  for (int i = 0; i < Settings::NUM_OPUS_KBPS; i ++)
    if (Settings::OPUS_KBPS[i] == st.opusKbps && m_bitrate->getSelectedSegment() != (uint32_t)i)
    {
      m_syncing = true;
      m_bitrate->setSelectedSegment((uint32_t)i);
      m_syncing = false;
    }
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
      row.container = new CViewContainer(z);
      row.container->setBackgroundColor(index & 1 ? CColor(25, 28, 32) : Colors::field());
      std::string label = u.displayName;
      if (!c.name.empty()) label += "  \xc2\xb7  " + c.name;
      row.name = makeLabel(z, label.c_str(), m_font, Colors::text());
      row.name->setTooltipText(u.name.c_str());
      row.container->addView(row.name);
      const int32_t tag = kTagRemote + index * kRemoteFields;
      row.volume = makeSlider(z, this, tag + kRemoteVolume, false);
      row.container->addView(row.volume);
      row.pan = makeSlider(z, this, tag + kRemotePan, true);
      row.container->addView(row.pan);
      row.mute = makeButton(z, this, tag + kRemoteMute, "M", true, m_smallBold);
      row.mute->setTooltipText("Mute");
      row.container->addView(row.mute);
      row.solo = makeButton(z, this, tag + kRemoteSolo, "S", true, m_smallBold);
      row.solo->setTooltipText("Solo");
      row.container->addView(row.solo);
      row.meter = new MeterView(z);
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
      row.meter->setLevels(c.peak[0], c.peak[1]);
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
      case ChatLine::Action: v.who = "* " + l.who; v.whoColor = Colors::warn(); v.textColor = Colors::warn(); break;
      case ChatLine::Private: v.who = l.who.compare(0, 3, "-> ") == 0 ? l.who : l.who + " (private)"; v.whoColor = Colors::server(); break;
      case ChatLine::Topic: v.textColor = Colors::topic(); break;
      case ChatLine::Server: v.textColor = Colors::server(); break;
      case ChatLine::Error: v.textColor = Colors::error(); break;
      default: v.textColor = Colors::dim(); break;
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

  std::string text, address;
  CColor color = Colors::dim();
  if (s.hosting)
  {
    text = "Port " + std::to_string(s.hostPort) + "  \xc2\xb7  " + std::to_string(s.hostUsers) + (s.hostUsers == 1 ? " player" : " players") + "  \xc2\xb7  ";
    if (!s.hostPublicAddress.empty())
    {
      address = s.hostPublicAddress;
      text += "Others join at " + address;
      color = Colors::accent();
    }
    else if (s.hostPortmapPending)
    {
      address = s.hostLocalAddress;
      text += "Asking your router to open the port...";
      if (!address.empty()) text += "  Local network: " + address;
      color = Colors::warn();
    }
    else
    {
      address = s.hostLocalAddress;
      text += "Local network only" + (address.empty() ? std::string() : ", at " + address);
      std::string why = s.hostNote; // the port mapper's last word; its first sentence fits here
      const size_t dot = why.find(". ");
      if (dot != std::string::npos) why.resize(dot);
      while (!why.empty() && why.back() == '.') why.pop_back();
      if (!why.empty()) text += ": " + why;
      color = Colors::warn();
    }
  }
  else if (s.hostBusy)
  {
    text = m_host->getValue() > 0.5f ? "Starting the server..." : "Stopping the server...";
  }
  else if (s.hostedElsewherePort)
  {
    text = "Another NINJAM plug-in in this program hosts a server: connect to localhost:" + std::to_string(s.hostedElsewherePort) + ".";
  }
  else
  {
    text = "Anyone can join your server without an account; one vote changes BPM or BPI, !codec switches the codec.";
  }
  setLabel(m_hostText, text, true);
  setLabelColor(m_hostText, color);

  m_hostAddress = address;
  const bool showCopy = !address.empty();
  if (m_copy->isVisible() != showCopy)
  {
    m_copy->setVisible(showCopy);
    layout();
  }
}

void NinjamEditor::updateLicense(const Snapshot &s)
{
  if (s.licensePending && !m_licenseOpen)
  {
    const CRect z(0, 0, 10, 10);
    m_license = new CViewContainer(z);
    m_license->setBackgroundColor(CColor(0, 0, 0, 150));
    m_licenseBox = new PanelView(z, Colors::panel(), Colors::accent(), 8);
    m_license->addView(m_licenseBox);
    m_licenseTitle = makeLabel(z, "This server asks you to accept its license", m_bold, Colors::text());
    m_licenseBox->addView(m_licenseTitle);
    m_licenseText = new TextLinesView(z, m_font, m_bold);
    m_licenseText->setStartAtTop(true);
    m_licenseBox->addView(m_licenseText);
    m_accept = makeButton(z, this, kTagAccept, "Accept and join", false, m_bold);
    m_licenseBox->addView(m_accept);
    m_decline = makeButton(z, this, kTagDecline, "Decline", false, m_font);
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

void NinjamEditor::copyHostAddress()
{
  if (!m_hostAddress.empty()) CClipboard::setString(m_hostAddress.c_str());
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
    case kTagFollow: engine().setFollowHostTempo(v > 0.5f); return;
    case kTagMonitor: engine().setMonitor(v > 0.5f); return;
    case kTagBitrate:
      if (m_bitrate->getSelectedSegment() < (uint32_t)Settings::NUM_OPUS_KBPS)
        engine().setOpusKbps(Settings::OPUS_KBPS[m_bitrate->getSelectedSegment()]);
      return;
    case kTagInputVolume: engine().setInputVolume(sliderToGain(v)); return;
    case kTagMetroVolume:
    case kTagMetroMute:
      engine().setMetronome(sliderToGain(m_metroVolume->getValue()), m_metroMute->getValue() > 0.5f);
      return;
    case kTagChat:
      if (m_chatEdit->bWasReturnPressed) sendChatLine();
      return;
    case kTagSend:
      if (v > 0.5f) sendChatLine();
      return;
    case kTagHost:
      m_hostPending = true;
      m_hostPendingSince = now_seconds();
      engine().setHosting(v > 0.5f);
      return;
    case kTagCopy:
      if (v > 0.5f) copyHostAddress();
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
