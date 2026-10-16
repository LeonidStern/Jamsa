/*
    NINJAM Server - liverelay.h

    NINJAM is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.
*/

/*

  Live mode's UDP relay. Players whose clients can play Live send their audio in
  UDP datagrams to the server's port (the TCP port's number), and the relay
  forwards each one to the other Live players in the sender's room. It never looks
  inside the audio. The datagrams are described in ../mpb.h, next to
  MESSAGE_SERVER_LIVE, which tells each client its id and key.

  The relay runs on its own thread with its own table of players, so forwarding
  never waits for the server's main loop. The main thread tells it who is where
  (SetPeer, RemovePeer, SetRoomLive) and asks it who has been heard from lately
  (PeerBound); Generation() changes whenever that might have changed. The relay
  thread never touches the server's users or rooms.

  A datagram is forwarded only when it carries the sender's key and, with
  SetStrictAddress (the default), comes from the address of the sender's TCP
  connection; then only to players in the same room who may send and have been
  heard from in the last 20 seconds, never to an address taken from the packet.
  Each sender may send at most twice the room's Live bitrate plus 8 KB/s, and
  twice the room's frame rate plus 50 datagrams/s, with a quarter second of burst.

*/

#ifndef _LIVERELAY_H_
#define _LIVERELAY_H_

#include "../../WDL/wdltypes.h"

class LiveRelay
{
  public:
    LiveRelay();
    ~LiveRelay(); // closes

    // All of these are for the server's main thread.

    // Opens the UDP port (all interfaces) and starts the relay thread. Forgets every
    // player and room. Returns false if the port can't be opened, e.g. it is in use.
    // Each player is answered from the address it sends to, where the system can say
    // which that is (Linux, macOS, BSD, Windows); elsewhere a machine with more than one
    // address answers from the one its routing table picks.
    bool Open(int port);
    void Close(); // stops the thread and closes the port; forgets everyone
    bool IsOpen() const;
    int Port() const; // while open

    // the address check, on by default; kept across Open and Close
    void SetStrictAddress(bool strict);

    // Adds or updates a player. tcp_ip is the address of its TCP connection, in network
    // byte order. Players who may not send neither send nor receive Live audio, but
    // their HELLOs are answered.
    void SetPeer(unsigned int id, unsigned int room_id, unsigned int tcp_ip, WDL_UINT64 key, bool may_send);
    void RemovePeer(unsigned int id);

    // whether the room's Live is on, and its bitrate (kbps) and frame size (samples at
    // 48 kHz), which set how much each sender may send
    void SetRoomLive(unsigned int room_id, bool on, int live_kbps, int frame_samples);
    void RemoveRoom(unsigned int room_id);

    // true while the relay has an address for this player that it heard from in the
    // last 20 seconds
    bool PeerBound(unsigned int id);

    // changes whenever a player becomes bound or stops being bound, and on Open and Close
    unsigned int Generation() const;

    // fills key with 8 bytes from the system's random source; false if it can't
    static bool RandomKey(WDL_UINT64 *key);

  private:
    struct State;
    State *m_state;
};

extern LiveRelay g_live;

#endif
