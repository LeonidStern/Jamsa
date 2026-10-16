/*
    NINJAM Server - usercon.h
    Copyright (C) 2005-2007 Cockos Incorporated

    NINJAM is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    NINJAM is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with NINJAM; if not, write to the Free Software
    Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
*/

/*

  This header provides the class declarations for the User_Group (which the server uses)
  and other classes it is built upon. Additionally, it provides the abstract declaration
  of IUserInfoLookup, which the server needs to override to provide user authentication 
  lookup.

*/




#ifndef _USERCON_H_
#define _USERCON_H_


#include "../netmsg.h"
#include "../../WDL/wdlstring.h"
#include "../../WDL/sha.h"
#include "../../WDL/ptrlist.h"
#include "../mpb.h"

#define MAX_USER_CHANNELS 32
#define MAX_USERS 64
#define MAX_UPLOADS (MAX_USER_CHANNELS*2) // in-progress uploads per user (one interval per channel, plus overlap)
#define MAX_DOWNLOADS 128

#define PRIV_TOPIC 1
#define PRIV_CHATSEND 2
#define PRIV_BPM 4
#define PRIV_KICK 8
#define PRIV_RESERVE 16
#define PRIV_ALLOWMULTI 32 // allows multiple users by the same name (subsequent users append -X to them)
#define PRIV_HIDDEN 64   // hidden user, doesn't count for a slot, too
#define PRIV_VOTE 128
#define PRIV_SHOW_PRIVATE 256

#define MAX_BPM 400
#define MAX_BPI 64
#define MIN_BPM 40
#define MIN_BPI 2

class IUserInfoLookup // abstract base class, overridden by server
{
public:
  IUserInfoLookup() { is_status=0; user_valid=0; reqpass=1; privs=0; max_channels=0; }
  virtual ~IUserInfoLookup() { }

  virtual int Run()=0; // return 1 if run is complete, 0 if still needs to run more

  int user_valid; // 1 if valid
  int reqpass; // password required, 1 is default
  unsigned int privs;
  int max_channels;
  int is_status;
  unsigned char sha1buf_user[WDL_SHA1SIZE];

  WDL_String hostmask;
  WDL_String username; // can modify this to change the username


  unsigned char sha1buf_request[WDL_SHA1SIZE]; // don't use, internal for User_Connection
};


class User_Connection;

class User_Group
{
  public:
    User_Group();
    ~User_Group();

    void AddConnection(JNL_IConnection *con, int isres=0);

    int Run(); // return 1 if safe to sleep
    void SetConfig(int bpi, int bpm);
    void SetLicenseText(char *text) { m_licensetext.Set(text); }
    void Broadcast(Net_Message *msg, User_Connection *nosend=0);


    void SetLogDir(char *path); // NULL to not log

    // sends a message to the people subscribing to a channel of a user
    void BroadcastToSubs(Net_Message *msg, User_Connection *src, int channel);

    IUserInfoLookup *(*CreateUserLookup)(const char *username);

    void onChatMessage(User_Connection *con, mpb_chat_message *msg);

    // !codec [opus|flac|vorbis]: any player can switch the codec of the whole room
    // (not to Vorbis with RequireCodec)
    void onCodecCommand(User_Connection *con, const char *arg);

    // !bitrate [N]: any player can set the Opus bitrate everyone sends (MIN_OPUS_KBPS to
    // MAX_OPUS_KBPS kbps); it goes to clients with the codec mask
    void onBitrateCommand(User_Connection *con, const char *arg);
    enum { MIN_OPUS_KBPS = 32, MAX_OPUS_KBPS = 510 };

    // !live [on|off]: any player can turn the room's Live mode on or off
    void onLiveCommand(User_Connection *con, const char *arg);

    // tells the relay (g_live, liverelay.h) who plays Live in this room, and each Live
    // client its MESSAGE_SERVER_LIVE when that changes
    void SendLiveState();
    enum { MIN_LIVE_KBPS = 32, MAX_LIVE_KBPS = 510, MAX_LIVE_PEERS = 64 };

    // tells everyone else that this user (and their channels) are gone
    void NotifyUserLeft(User_Connection *p);

    // tells clients that negotiate codecs which one the room uses (and its Opus bitrate),
    // when that changes
    void SendCodecMask();

    // The codec (CLIENT_CAP_CODEC_* bit, 0 for Vorbis) everyone in the room uploads: the one
    // chosen with !codec if everyone can decode it, else the best one they all can, which
    // with RequireCodec is never Vorbis.
    // *cant_decode gets someone whose client can't decode the chosen one.
    int RoomCodec(User_Connection **cant_decode=NULL);


    WDL_PtrList<User_Connection> m_users;

    int m_max_users;
    int m_last_bpm, m_last_bpi;
    int m_keepalive;


    int m_voting_threshold; // can be 1-100, or >100 to disable
    int m_voting_timeout; // seconds

    int m_loopcnt;

    unsigned int m_run_robin;

    int m_allow_hidden_users;

    int m_allow_codecs; // CLIENT_CAP_CODEC_* bits of the codecs this server relays
    int m_require_codec; // CLIENT_CAP_CODEC_* bit every client must play to log in (RequireCodec), 0 for none.
                         // With it, rooms never use Vorbis.
    int m_room_codec; // codec chosen with !codec, 0 for Vorbis
    int m_room_codec_announced; // codec the room was last told about, -1 while the room is empty
    int m_room_opus_kbps; // Opus bitrate everyone sends (OpusBitrate, then !bitrate), 0 for none
    bool m_room_opus_kbps_chosen; // set with !bitrate, so a config reload keeps it

    // Live mode
    bool m_allow_live; // the server has Live (Live yes, on or require)
    bool m_room_live; // the room's Live is on (Live on, then !live)
    bool m_room_live_chosen; // set with !live, so a config reload keeps it
    int m_live_kbps; // LiveKbps
    int m_live_frame; // LiveFrame
    int m_live_max_peers; // LiveMaxPeers: how many players can play Live in the room
    int m_live_port; // LivePort: the UDP port clients are told, 0 for the TCP one
    unsigned int m_live_room_id; // the room's id in the relay, never 0
    int m_live_gen; // counts the changes to the room's Live state
    // what the room's Live state was last worked out from
    unsigned int m_live_relay_gen;
    int m_live_users;
    int m_live_params[7];

#define LOBBY_ALLOW_CHAT 2
    int m_is_lobby_mode; // nonzero for lobby (and LOBBY_ALLOW_CHAT etc optional)

    WDL_String m_licensetext;
    WDL_String m_topictext;

    WDL_String m_logdir;

    WDL_FastString m_motdfile;

    FILE *m_logfp;

#ifdef _WIN32
    DWORD m_next_loop_time;
#else
    struct timeval m_next_loop_time;
#endif
};



class User_SubscribeMask
{
public:
  User_SubscribeMask()  {} 
  ~User_SubscribeMask() {}
  WDL_String username;
  unsigned int channelmask;
};

class User_Channel
{
public:
  User_Channel() { active=0; volume=0; panning=128; flags=0; }
  ~User_Channel() { }
  int active;
  WDL_String name;
  int volume;
  int panning;
  int flags;
};


class User_TransferState
{
public:
  User_TransferState() : fourcc(0), bytes_estimated(0), bytes_sofar(0), source_id(0), fp(0)
  { 
    time(&last_acttime);
    memset(guid,0,sizeof(guid));
  }
  ~User_TransferState() 
  { 
    if (fp) fclose(fp);
    fp=0;
  }

  time_t last_acttime;
  unsigned char guid[16];
  unsigned int fourcc;
  unsigned int bytes_estimated;

  unsigned int bytes_sofar;

  unsigned int source_id; // m_id of the User_Connection that is uploading this interval

  FILE *fp;
};


class User_Connection
{
  public:
    User_Connection(JNL_IConnection *con, User_Group *grp);
    ~User_Connection();

    int Run(User_Group *group, int *wantsleep=0); // returns 1 if disconnected, -1 if error in data. 0 if ok.
    void SendConfigChangeNotify(int bpm, int bpi);

    void Send(Net_Message *msg);

    int OnRunAuth(User_Group *group);

    void SendUserList(User_Group *group);
    void SendConnectInfo(User_Group *group);
    void SendAuthReply(User_Group *group); // sends "success"

    void SendPrivateModeStats(const char *req);
    void SendMOTDFile(User_Group *group);

    Net_Connection m_netcon;
    WDL_String m_username;
    
    // auth info
    time_t m_connect_time;
    int m_auth_state;      // 1 if authorized, 0 if not yet, -1 if auth pending
    unsigned char m_challenge[8];
    int m_clientcaps;
    int m_codec_mask_sent; // last MESSAGE_SERVER_CODEC_MASK sent, -1 for none
    int m_opus_kbps_sent; // the bitrate in it

    WDL_UINT64 m_live_key; // this connection's secret for Live datagrams, 0 until it needs one
    unsigned int m_live_room; // m_live_room_id of the room whose Live state it has, 0 for none
    int m_live_sent_gen; // that room's m_live_gen then
    WDL_HeapBuf m_live_sent; // the last MESSAGE_SERVER_LIVE it got, empty for none

    int m_auth_privs;

    int m_reserved;

    int m_max_channels;

    int m_vote_bpm;
    time_t m_vote_bpm_lasttime;
    int m_vote_bpi;
    time_t m_vote_bpi_lasttime;

    User_Channel m_channels[MAX_USER_CHANNELS];

    WDL_PtrList<User_SubscribeMask> m_sublist; // people+channels we subscribe to

    WDL_PtrList<User_TransferState> m_recvfiles;
    WDL_PtrList<User_TransferState> m_sendfiles;

    IUserInfoLookup *m_lookup;

    bool migrateToRoom(const char *p);
    WDL_FastString m_wants_group_migration; // set from lobby in private group mode

    unsigned int m_id; // unique per connection, never reused while the server runs
    time_t m_last_transfer_sweep;
    void SweepTransfers(time_t now);
};


const char *get_privatemode_stats(int privs, const char *req);

#endif//_USERCON_H_
