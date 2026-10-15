/*
    NINJAM VST3 - editor.h
    Copyright (C) 2026 and onward, NINJAM contributors

    NINJAM is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.
*/

/*

  The editor, drawn with VSTGUI inside the host's window. It is laid out in
  code and can be resized; the size is saved with the plug-in's state.

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

  enum { DEFAULT_WIDTH = 960, DEFAULT_HEIGHT = 640, MIN_WIDTH = 780, MIN_HEIGHT = 520, MAX_WIDTH = 3000, MAX_HEIGHT = 2000 };

private:
  struct Row // one remote channel in the mixer
  {
    std::string user; // name@address
    int channel = 0;
    VSTGUI::CViewContainer *container = nullptr;
    VSTGUI::CTextLabel *name = nullptr;
    VSTGUI::CSlider *volume = nullptr, *pan = nullptr;
    VSTGUI::CTextButton *mute = nullptr, *solo = nullptr;
    ui::MeterView *meter = nullptr;
  };

  void buildViews();
  void layout();
  void layoutRow(Row &row, VSTGUI::CCoord width);
  void layoutLicense();
  void update();
  void rebuildRows(const Snapshot &s);
  void updateRows(const Snapshot &s);
  void updateChat();
  void updateHostBar(const Snapshot &s);
  void updateLicense(const Snapshot &s);
  void syncSettings();
  void connectOrDisconnect();
  void sendChatLine();
  void copyHostAddress();
  void setLogicalSize(VSTGUI::CCoord width, VSTGUI::CCoord height);

  Engine &engine();

  NinjamPlugin *m_plugin;
  VSTGUI::CCoord m_width = DEFAULT_WIDTH, m_height = DEFAULT_HEIGHT; // without the content scale
  double m_scale = 1.0;

  VSTGUI::SharedPointer<VSTGUI::CFontDesc> m_font, m_bold, m_small, m_smallBold, m_title;

  VSTGUI::CViewContainer *m_root = nullptr;
  VSTGUI::CTextLabel *m_titleLabel = nullptr, *m_nameLabel = nullptr, *m_serverLabel = nullptr;
  VSTGUI::CTextEdit *m_nameEdit = nullptr, *m_serverEdit = nullptr;
  VSTGUI::CTextButton *m_connect = nullptr;
  VSTGUI::CTextLabel *m_status = nullptr, *m_room = nullptr;
  ui::IntervalView *m_interval = nullptr;

  VSTGUI::CViewContainer *m_mixer = nullptr;
  VSTGUI::CTextLabel *m_localHeader = nullptr, *m_metroHeader = nullptr, *m_playersHeader = nullptr;
  VSTGUI::CTextButton *m_transmit = nullptr, *m_monitor = nullptr, *m_metroMute = nullptr;
  VSTGUI::CSlider *m_inputVolume = nullptr, *m_metroVolume = nullptr;
  VSTGUI::CTextLabel *m_inputVolumeLabel = nullptr, *m_metroVolumeLabel = nullptr, *m_codecLabel = nullptr;
  VSTGUI::CTextLabel *m_bitrateLabel = nullptr, *m_bitrateNote = nullptr;
  VSTGUI::CSegmentButton *m_bitrate = nullptr; // Opus kbps, Settings::OPUS_KBPS
  ui::MeterView *m_localMeter = nullptr;
  VSTGUI::CTextLabel *m_colName = nullptr, *m_colVolume = nullptr, *m_colPan = nullptr, *m_colLevel = nullptr;
  VSTGUI::CScrollView *m_players = nullptr;
  VSTGUI::CTextLabel *m_noPlayers = nullptr;
  std::vector<Row> m_rows;

  VSTGUI::CViewContainer *m_chatPanel = nullptr;
  VSTGUI::CTextLabel *m_chatHeader = nullptr;
  ui::TextLinesView *m_chat = nullptr;
  VSTGUI::CTextEdit *m_chatEdit = nullptr;
  VSTGUI::CTextButton *m_send = nullptr;

  VSTGUI::CViewContainer *m_hostBar = nullptr;
  VSTGUI::CTextButton *m_host = nullptr, *m_copy = nullptr;
  VSTGUI::CTextLabel *m_hostText = nullptr;

  // the license, shown as a modal view while the server waits for an answer
  VSTGUI::CViewContainer *m_license = nullptr;
  VSTGUI::CViewContainer *m_licenseBox = nullptr;
  VSTGUI::CTextLabel *m_licenseTitle = nullptr;
  ui::TextLinesView *m_licenseText = nullptr;
  VSTGUI::CTextButton *m_accept = nullptr, *m_decline = nullptr;
  VSTGUI::ModalViewSessionID m_licenseSession = 0;
  bool m_licenseOpen = false;
  std::string m_licenseShown;

  // what the views show, to update them only on changes
  unsigned int m_usersRevision = ~0u, m_settingsRevision = ~0u;
  unsigned long long m_chatRevision = ~0ull;
  std::string m_hostAddress;                // what Copy copies
  bool m_hostPending = false;               // the switch was used; wait for the engine
  double m_hostPendingSince = 0;
  bool m_refocusChat = false;
  bool m_rowsEmpty = true;
  bool m_syncing = false;    // setting controls from the engine: don't send them back
  int m_bitrateState = -1;   // what the bitrate picker shows: 0 Opus, 1 FLAC (lossless)
};

} // namespace ninjam_vst3

#endif
