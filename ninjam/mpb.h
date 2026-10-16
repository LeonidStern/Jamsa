/*
    NINJAM - mpb.h
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

  This header provides message-type defines, as well as Message Parser and Builder (mpb_)
  classes for constructing and parsing Net_Messages.

*/


#ifndef _MPB_H_
#define _MPB_H_ // mpb.h, message parsing and building


#include "netmsg.h"


#define PROTO_VER_MIN 0x00020000
#define PROTO_VER_MAX 0x0002ffff
#define PROTO_VER_CUR 0x00020000


#define MESSAGE_SERVER_AUTH_CHALLENGE 0x00

class mpb_server_auth_challenge
{
  public:
    mpb_server_auth_challenge() : server_caps(0), license_agreement(0), protocol_version(0) { memset(challenge,0,sizeof(challenge)); }
    ~mpb_server_auth_challenge() { }

    int parse(Net_Message *msg); // return 0 on success
    Net_Message *build();


    // public data
    unsigned char challenge[8];
    int server_caps; // low bit is license agreement, bits 8-16 are keepalive
    const char *license_agreement;
    int protocol_version; // version should be 1 to start.
};

#define MESSAGE_SERVER_AUTH_REPLY 0x01

class mpb_server_auth_reply
{
  public:
    mpb_server_auth_reply() : flag(0), errmsg(0), maxchan(32) { }
    ~mpb_server_auth_reply() { }

    int parse(Net_Message *msg); // return 0 on success
    Net_Message *build();


    // public data
    char flag;  // low bit is success bit
    const char *errmsg; // if success bit is set, and this is also set, then it is the effective username of the client
    char maxchan;
};



#define MESSAGE_SERVER_CONFIG_CHANGE_NOTIFY 0x02

class mpb_server_config_change_notify
{
  public:
    mpb_server_config_change_notify() : beats_minute(120), beats_interval(32) { }
    ~mpb_server_config_change_notify() { }

    int parse(Net_Message *msg); // return 0 on success
    Net_Message *build();


    // public data
    int beats_minute;  //bpm
    int beats_interval;  // beats/interval
};



#define MESSAGE_SERVER_USERINFO_CHANGE_NOTIFY 0x03

class mpb_server_userinfo_change_notify
{
  public:
    mpb_server_userinfo_change_notify() : m_intmsg(0) { }
    ~mpb_server_userinfo_change_notify() { }

    int parse(Net_Message *msg); // return 0 on success
    Net_Message *build(); // if you call build_add_rec at all, you must do delete x->build(); to avoid a mem leak.

    // public accessors
    // pan is -128..127
    // volume is dB gain, so 0=0dB, 10=1dB, -30=-3 dB, etc
    // flags, &1 = no default subscribe, &2=instamode
    void build_add_rec(int isActive, int channelid, short volume, int pan, int flags, const char *username, const char *chname);
    int parse_get_rec(int offs, int *isActive, int *channelid, short *volume, int *pan, int *flags, const char **username, const char **chname); // returns offset of next item on success, or <= 0 if out of items

   private:

     Net_Message *m_intmsg;
};


#define MESSAGE_SERVER_DOWNLOAD_INTERVAL_BEGIN 0x04
class mpb_server_download_interval_begin
{
  public:
    mpb_server_download_interval_begin() : estsize(0), fourcc(0), chidx(0), username(0) { memset(guid,0,sizeof(guid)); }
    ~mpb_server_download_interval_begin() { }

    int parse(Net_Message *msg); // return 0 on success
    Net_Message *build();

    // public data
    unsigned char guid[16];
    int estsize;
    unsigned int fourcc;
    int chidx;       // only 1 byte
    const char *username;
};


#define MESSAGE_SERVER_DOWNLOAD_INTERVAL_WRITE 0x05
class mpb_server_download_interval_write
{
  public:
    mpb_server_download_interval_write() : flags(0), audio_data(0), audio_data_len(0) { memset(guid,0,sizeof(guid)); }
    ~mpb_server_download_interval_write() { }

    int parse(Net_Message *msg); // return 0 on success
    Net_Message *build();

    // public data
    unsigned char guid[16]; // transfer id
    char flags; // & 1 = end

    const void *audio_data;
    int audio_data_len; // not encoded in, just used internally
};




// Codec negotiation. Optional: servers and clients that don't know it ignore it.
//
// A client that can decode codecs besides Vorbis sets their CLIENT_CAP_CODEC_* bits
// in the client_caps of MESSAGE_CLIENT_AUTH_USER. A server that supports negotiation
// then sends that client MESSAGE_SERVER_CODEC_MASK: codecs everyone in the room can
// decode (and the server allows), and sends it again whenever that changes.
// Clients that didn't set any codec bits never get this message.
//
// ninjamsrv puts just one codec in the mask, the one the room uses, so that everyone
// uploads the same one. Any player can switch it by typing !codec opus, !codec flac
// or !codec vorbis in the chat; if someone in the room can't decode that codec, the
// room uses the best one everyone can until they leave. A client shows the room's
// codec from the mask.
//
// The mask may also carry the room's Opus bitrate (kbps), which ninjamsrv sets with
// OpusBitrate in its config and any player can change with !bitrate N. Clients that know
// it send Opus at that bitrate; older ones read only the codecs and send their own.
//
// A client may upload an interval in one of these codecs only while the latest mask it
// received includes it, and must choose the codec at the start of each interval.
// Without a mask (older servers never send one) it uploads Vorbis.
// The server never forwards an interval to someone whose client_caps lack its codec:
// that subscriber gets a silent interval (zero GUID) instead.
#define CLIENT_CAP_CODEC_OPUS 0x100 // fourcc 'OPUS': Ogg Opus (RFC 7845), one Ogg stream per interval
#define CLIENT_CAP_CODEC_FLAC 0x200 // fourcc 'FLAC': FLAC, lossless
#define CLIENT_CAP_CODECS 0xff00    // bits 8-15 are reserved for codecs

// Bits 16 and up are for features (bit 0 is the license agreement; bits 1-7 are left
// alone, because other clients have used some of them). There are no server_caps bits
// for features: a client learns what the server can do from the messages it gets.
#define CLIENT_CAP_LIVE 0x10000     // Live mode: plays with the room in real time over UDP
                                    // (see MESSAGE_SERVER_LIVE)

#define NJ_FOURCC(a,b,c,d) ((unsigned int)(a) | ((unsigned int)(b)<<8) | ((unsigned int)(c)<<16) | ((unsigned int)(d)<<24))

// the client_caps bit needed to receive intervals of this type, or 0 if every client gets them (Vorbis, video)
static inline int mpb_codec_cap_for_fourcc(unsigned int fourcc)
{
  if (fourcc == NJ_FOURCC('O','P','U','S')) return CLIENT_CAP_CODEC_OPUS;
  if (fourcc == NJ_FOURCC('F','L','A','C')) return CLIENT_CAP_CODEC_FLAC;
  return 0;
}

#define MESSAGE_SERVER_CODEC_MASK 0x06
class mpb_server_codec_mask
{
  public:
    mpb_server_codec_mask() : codecs(0), opus_kbps(0) { }
    ~mpb_server_codec_mask() { }

    int parse(Net_Message *msg); // return 0 on success
    Net_Message *build();

    // public data
    int codecs; // CLIENT_CAP_CODEC_* bits; 0 = Vorbis only
    int opus_kbps; // the room's Opus bitrate in kbps; 0 = none (each client sends its own).
                   // Sent only when nonzero, after the codecs; older clients ignore it.
};



// Live mode. Optional: servers and clients that don't know it ignore it.
//
// While a room's Live is on, players whose clients can do it hear each other in real
// time: each one sends its audio in UDP datagrams to the server it is connected to, and
// the server forwards them to the other Live players in the room. Intervals keep flowing
// as before, for everyone else and for archiving.
//
// A client that can play Live sets CLIENT_CAP_LIVE in its client_caps. A server with
// Live sends it MESSAGE_SERVER_LIVE once it has logged in (never in a lobby), and again
// whenever what it says to that client changes. Other clients never get it.
//
// Little-endian. A newer version may make the fixed part longer: a parser reads the
// fields it knows and finds the list of peers at fixed_len, which runs to the end.
//
//   off size
//   0   u8   version          1
//   1   u8   fixed_len        32
//   2   u8   flags            LIVE_FLAG_*
//   3   u8   reserved, 0
//   4   u16  udp_port         0 = the port number the client connected to over TCP
//   6   u16  live_kbps        the room's Live Opus bitrate (kbps)
//   8   u32  peer_id          the client's own id on the server (never 0)
//   12  u64  key              a secret for this connection, only ever sent to its owner
//   20  u16  frame_samples    the room's Opus frame size at 48 kHz: 120, 240 or 480
//   22  u16  max_peers        how many players can play Live in a room
//   24  u32  reserved, 0
//   28  u32  reserved, 0
//   32  the other players in the room who may send, each: u32 peer_id,
//       u8 peer_flags (LIVE_PEER_*), the user name, NUL-terminated
//
// UDP datagrams, little-endian, never longer than 1500 bytes. Client to server:
//   0   'N' 'L'
//   2   u8   version          1
//   3   u8   type             1 = HELLO, 2 = DATA
//   4   u32  sender_id        the peer_id the client was given
//   8   u64  key              the key it was given
//   16  u32  dest_id          DATA: 0 = every other Live player in the room, else one
//                             player's peer_id. HELLO: 0
//   20  HELLO: u64 timestamp (any value; the server echoes it), then 4 zero bytes, so a
//       HELLO is 32 bytes. DATA: the audio packet, 1 to 1452 bytes, forwarded as is.
// Server to client:
//   0   'N' 'L', u8 version 1, u8 type (1 = HELLO_ACK, 2 = DATA)
//   4   u32  from_id          DATA: the sender's peer_id. HELLO_ACK: 0
//   8   HELLO_ACK: the 8 timestamp bytes of the HELLO. DATA: the audio packet.
// A client sends a HELLO once it gets LIVE_FLAG_UDP_OPEN, and then every 2 seconds,
// which keeps its router's mapping open; the server drops a player it hasn't heard
// from in 20 seconds. Datagrams that are malformed, carry the wrong key or come from
// another address than the player's TCP connection (unless the server's config says
// LiveStrictAddress no) are dropped without an answer.
#define MESSAGE_SERVER_LIVE 0x07
class mpb_server_live
{
  public:
    mpb_server_live() : version(LIVE_VERSION), flags(0), udp_port(0), live_kbps(0), peer_id(0), key(0),
                        frame_samples(0), max_peers(0), m_peers(0), m_peers_len(0) { }
    ~mpb_server_live() { }

    enum { LIVE_VERSION = 1, LIVE_FIXED_LEN = 32 };
    enum
    {
      LIVE_FLAG_ROOM_ON = 1,  // the room's Live is on
      LIVE_FLAG_UDP_OPEN = 2, // the server takes Live datagrams
      LIVE_FLAG_MAY_SEND = 4, // this client is one of the room's Live players
    };
    enum { LIVE_PEER_BOUND = 1 }; // the server has heard from this peer over UDP lately

    int parse(Net_Message *msg); // return 0 on success; msg must outlive parse_get_peer calls
    Net_Message *build();

    void build_add_peer(unsigned int id, int peer_flags, const char *name);
    // offs starts at 0; returns the offset of the next peer, or <= 0 when there are no more
    int parse_get_peer(int offs, unsigned int *id, int *peer_flags, const char **name);

    // public data
    int version;
    int flags; // LIVE_FLAG_*
    int udp_port;
    int live_kbps;
    unsigned int peer_id;
    WDL_UINT64 key;
    int frame_samples;
    int max_peers;

  private:
    WDL_HeapBuf m_build_peers; // the peer list being built
    const unsigned char *m_peers; // the parsed one
    int m_peers_len;
};



#define MESSAGE_CLIENT_AUTH_USER 0x80
class mpb_client_auth_user
{
  public:
    mpb_client_auth_user() : client_caps(0), client_version(0), username(0) { memset(passhash,0,sizeof(passhash)); }
    ~mpb_client_auth_user() { }

    int parse(Net_Message *msg); // return 0 on success
    Net_Message *build();


    // public data
    unsigned char passhash[20];
    int client_caps; // low bit is agreeing to license, then CLIENT_CAP_* bits
    int client_version; // client version, always sent (the second bit of caps was once
                        // meant to say so, but nothing checks it)
    const char *username;
};



#define MESSAGE_CLIENT_SET_USERMASK 0x81
class mpb_client_set_usermask
{
  public:
    mpb_client_set_usermask() : m_intmsg(0) { }
    ~mpb_client_set_usermask() { }

    int parse(Net_Message *msg); // return 0 on success
    Net_Message *build();


    void build_add_rec(const char *username, unsigned int chflags);
    int parse_get_rec(int offs, const char **username, unsigned int *chflags); // returns offset of next item on success, or <= 0 if out of items

   private:

     Net_Message *m_intmsg;
};

#define MESSAGE_CLIENT_SET_CHANNEL_INFO 0x82
class mpb_client_set_channel_info
{
  public:
    mpb_client_set_channel_info() : mpisize(4), m_intmsg(0) { }
    ~mpb_client_set_channel_info() { }

    int parse(Net_Message *msg); // return 0 on success
    Net_Message *build();


    // pan is -128..127
    // volume is dB gain, so 0=0dB, 10=1dB, -30=-3 dB, etc
    // flags, &1 = no default subscribe, &2=instamode, &4=session mode, 0x80=filler (inactive)
    void build_add_rec(const char *chname, short volume, int pan, int flags);
    int parse_get_rec(int offs, const char **chname, short *volume, int *pan, int *flags); // returns offset of next item on success, or <= 0 if out of items

    int mpisize;

   private:

     Net_Message *m_intmsg;
};


#define MESSAGE_CLIENT_UPLOAD_INTERVAL_BEGIN 0x83
class mpb_client_upload_interval_begin
{
  public:
    mpb_client_upload_interval_begin() : estsize(0), fourcc(0), chidx(0){ memset(guid,0,sizeof(guid)); }
    ~mpb_client_upload_interval_begin() { }

    int parse(Net_Message *msg); // return 0 on success
    Net_Message *build();

    // public data
    unsigned char guid[16];
    int estsize;
    unsigned int fourcc;
    int chidx;       // only 1 byte
};



// this uses the exact same message format as the server version
#define MESSAGE_CLIENT_UPLOAD_INTERVAL_WRITE 0x84
class mpb_client_upload_interval_write
{
  public:
    mpb_client_upload_interval_write() : flags(0), audio_data(0), audio_data_len(0) { memset(guid,0,sizeof(guid)); }
    ~mpb_client_upload_interval_write() { }

    int parse(Net_Message *msg); // return 0 on success
    Net_Message *build();

    // public data
    unsigned char guid[16];
    char flags; // & 1 = end

    const void *audio_data;
    int audio_data_len; // not encoded in, just used internally
};


#define MESSAGE_CHAT_MESSAGE 0xC0
class mpb_chat_message
{
  public:
    mpb_chat_message() { memset(parms,0,sizeof(parms)); }
    ~mpb_chat_message() { }

    int parse(Net_Message *msg); // return 0 on success
    Net_Message *build();

    const char *parms[5];

    // currently defined client->server commands:
    // MSG <text>   - sends a message to everybody
    // PRIVMSG username <text>   - sends a private message to username
    // TOPIC <topic>   - set server topic (need permissions)

    // and server->client commands:
    // MSG <username> <text>   - a message from username
    // PRIVMSG username <text>   - a private message from username
    // TOPIC <topic>   - server topic change
};




#endif//_MPB_H_
