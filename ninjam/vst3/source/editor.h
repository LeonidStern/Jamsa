/*
    NINJAM VST3 - editor.h
    Copyright (C) 2026 and onward, NINJAM contributors

    NINJAM is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.
*/

/*

  The editor, drawn with VSTGUI inside the host's window, in the look of
  Ableton Live's devices. It is laid out in code for 720 x 330 and can be
  resized: larger, everything scales with the window (and the panes take what
  the aspect ratio leaves); down to 640 x 300, the layout gets tighter. The
  size is saved with the plug-in's state.

  From the top: the header (name, server, connect), the transport (the room's
  BPM and BPI, which vote when changed; Sync with the host; the click; the
  beats of the interval), the mixer (your channel first, then the others) and
  the chat side by side, and the host bar (the built-in server and its
  addresses).

  It reads the Engine's snapshot 30 times a second and sends what the user
  does to the Engine, so it never waits for the network.

*/

#ifndef _NINJAM_VST3_EDITOR_H_
#define _NINJAM_VST3_EDITOR_H_

#include <string>
#include <vector>

#include "public.sdk/source/vst/vstguieditor.h"
#include "pluginterfaces/gui/iplugviewcontentscalesupport.h"

#include "engine.h"
#include "views.h"

namespace ninjam_vst3 {

class NinjamPlugin;

class NinjamEditor : public Steinberg::Vst::VSTGUIEditor,
                     public VSTGUI::IControlListener,
                     public Steinberg::IPlugViewContentScaleSupport
{
public:
  explicit NinjamEditor(NinjamPlugin *plugin);
  ~NinjamEditor() override;

  // VSTGUIEditor
  bool PLUGIN_API open(void *parent, const VSTGUI::PlatformType &platformType) override;
  void PLUGIN_API close() override;
  VSTGUI::CMessageResult notify(VSTGUI::CBaseObject *sender, const char *message) override;

  // IPlugView
  Steinberg::tresult PLUGIN_API isPlatformTypeSupported(Steinberg::FIDString type) override;
  Steinberg::tresult PLUGIN_API canResize() override { return Steinberg::kResultTrue; }
  Steinberg::tresult PLUGIN_API checkSizeConstraint(Steinberg::ViewRect *rect) override;
  Steinberg::tresult PLUGIN_API onSize(Steinberg::ViewRect *newSize) override;

  // IPlugViewContentScaleSupport (Windows and Linux hosts with display scaling)
  Steinberg::tresult PLUGIN_API setContentScaleFactor(ScaleFactor factor) override;

  // IControlListener
  void valueChanged(VSTGUI::CControl *control) override;

  DELEGATE_REFCOUNT(Steinberg::Vst::VSTGUIEditor)
  Steinberg::tresult PLUGIN_API queryInterface(const Steinberg::TUID iid, void **obj) override;

  // the size it is laid out for, the smallest, the largest (without the host's content scale)
  enum { DEFAULT_WIDTH = 720, DEFAULT_HEIGHT = 330, MIN_WIDTH = 640, MIN_HEIGHT = 300, MAX_WIDTH = 3000, MAX_HEIGHT = 2000 };
  // how much a window of this size scales the layout: 1 up to the default size, then as much as fits
  static double zoomFor(double width, double height);

private:
  struct Row // one remote channel in the mixer
  {
    std::string user; // name@address
    int channel = 0;
    ui::StripContainer *container = nullptr;
    VSTGUI::CTextLabel *name = nullptr;
    ui::FaderBox *volume = nullptr;
    ui::ArcKnob *pan = nullptr;
    ui::FlatButton *mute = nullptr, *solo = nullptr;
    ui::SegmentMeter *meter = nullptr;
  };
  struct Columns // where the mixer's columns are in a row of width w
  {
    VSTGUI::CCoord name0, name1, vol0, vol1, pan0, pan1, mute0, mute1, solo0, solo1, level0, level1;
  };
  static Columns columns(VSTGUI::CCoord w);

  void buildViews();
  void layout();
  void layoutHeader(VSTGUI::CCoord w);
  void layoutTransport(VSTGUI::CCoord w);
  void layoutMixer(const VSTGUI::CRect &pane);
  void layoutRow(Row &row, VSTGUI::CCoord width);
  void layoutHostBar(VSTGUI::CCoord w, VSTGUI::CCoord y);
  void layoutLicense();
  void update();
  void rebuildRows(const Snapshot &s);
  void updateRows(const Snapshot &s);
  void updateChat();
  void updateHeader(const Snapshot &s);
  void updateTransport(const Snapshot &s);
  void updateFooter(const Snapshot &s);
  void updateHostBar(const Snapshot &s);
  void updateLicense(const Snapshot &s);
  void syncSettings();
  void selectEntry(ui::MenuBox *menu, int index);
  void connectOrDisconnect();
  void sendChatLine();
  void copyAddress(ui::AddressBox *box);
  void setLogicalSize(VSTGUI::CCoord width, VSTGUI::CCoord height);
  void applyZoom();

  Engine &engine();

  NinjamPlugin *m_plugin;
  VSTGUI::CCoord m_width = DEFAULT_WIDTH, m_height = DEFAULT_HEIGHT; // the window, without the content scale
  double m_scale = 1.0; // the host's content scale
  double m_zoom = 1.0;  // the layout's, from the window's size (zoomFor)

  ui::Fonts m_fonts;

  VSTGUI::CViewContainer *m_root = nullptr;

  // header
  ui::StripContainer *m_header = nullptr;
  VSTGUI::CTextLabel *m_title = nullptr, *m_nameLabel = nullptr, *m_serverLabel = nullptr, *m_connText = nullptr;
  VSTGUI::CTextEdit *m_nameEdit = nullptr, *m_serverEdit = nullptr;
  ui::FlatButton *m_connect = nullptr;
  ui::Led *m_connLed = nullptr;

  // transport
  ui::StripContainer *m_transport = nullptr;
  VSTGUI::CTextLabel *m_bpmLabel = nullptr, *m_bpiLabel = nullptr, *m_syncText = nullptr, *m_beatBox = nullptr;
  ui::DragNumberBox *m_bpm = nullptr;
  ui::MenuBox *m_bpi = nullptr;
  ui::Strip *m_sep[3] = { nullptr, nullptr, nullptr };
  ui::FlatButton *m_sync = nullptr, *m_click = nullptr;
  ui::Led *m_syncLed = nullptr;
  ui::ArcKnob *m_clickVolume = nullptr;
  ui::BeatCells *m_beats = nullptr;

  // mixer
  ui::StripContainer *m_mixer = nullptr;
  VSTGUI::CTextLabel *m_colChannel = nullptr, *m_colVolume = nullptr, *m_colPan = nullptr, *m_colLevel = nullptr;
  ui::StripContainer *m_myRow = nullptr;
  ui::FlatButton *m_tx = nullptr, *m_in = nullptr;
  VSTGUI::CTextLabel *m_myName = nullptr;
  ui::FaderBox *m_inputVolume = nullptr;
  ui::MenuBox *m_codec = nullptr;
  ui::MenuBox *m_bitrate = nullptr;
  ui::SegmentMeter *m_myMeter = nullptr;
  VSTGUI::CScrollView *m_players = nullptr;
  VSTGUI::CTextLabel *m_noPlayers = nullptr, *m_footer = nullptr;
  std::vector<Row> m_rows;

  // chat
  ui::StripContainer *m_chatPane = nullptr;
  ui::TextLinesView *m_chat = nullptr;
  VSTGUI::CTextEdit *m_chatEdit = nullptr;

  // host bar
  ui::StripContainer *m_hostBar = nullptr;
  ui::FlatButton *m_host = nullptr;
  VSTGUI::CTextLabel *m_roomCount = nullptr, *m_hostHint = nullptr, *m_portText = nullptr;
  ui::AddressBox *m_wan = nullptr;
  ui::Led *m_portLed = nullptr;

  // the license, shown as a modal view while the server waits for an answer
  VSTGUI::CViewContainer *m_license = nullptr;
  VSTGUI::CViewContainer *m_licenseBox = nullptr;
  VSTGUI::CTextLabel *m_licenseTitle = nullptr;
  ui::TextLinesView *m_licenseText = nullptr;
  ui::FlatButton *m_accept = nullptr, *m_decline = nullptr;
  VSTGUI::ModalViewSessionID m_licenseSession = 0;
  bool m_licenseOpen = false;
  std::string m_licenseShown;

  // what the views show, to update them only on changes
  unsigned int m_usersRevision = ~0u, m_settingsRevision = ~0u;
  unsigned long long m_chatRevision = ~0ull;
  bool m_hostPending = false;   // the switch was used; wait for the engine
  double m_hostPendingSince = 0;
  bool m_refocusChat = false;
  bool m_hosting = false;       // what the host bar was laid out for
  bool m_showingOwnServer = false; // the server box shows the server this instance hosts
  bool m_syncing = false;       // setting controls from the engine: don't send them back
  ui::AddressBox *m_copied = nullptr; // says "Copied" until m_copiedUntil
  double m_copiedUntil = 0;
  std::string m_wanShown; // what the address box was laid out for
};

} // namespace ninjam_vst3

#endif
