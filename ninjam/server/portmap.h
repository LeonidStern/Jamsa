/*
    NINJAM Server - portmap.h

    NINJAM is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.
*/

/*

  Asks the router to forward the server's port to this machine (UPnP-IGD,
  then NAT-PMP), so people hosting from home don't have to set up port
  forwarding by hand. Runs on its own thread, renews the mapping before it
  expires, and removes it on shutdown.

  With Live, the UDP port of the same number is forwarded too. That is best
  effort: if the router won't forward UDP, TCP stays forwarded and the log has
  a warning (players outside then stay on intervals).

  Built only when the Makefile is run with PORTMAP=1 (needs miniupnpc and
  libnatpmp); otherwise Start() just logs that support isn't compiled in.

*/

#ifndef _PORTMAP_H_
#define _PORTMAP_H_

class PortMapper
{
  public:
    PortMapper();
    ~PortMapper(); // removes the mapping

    void Start(int port, bool udp=false); // non-blocking; replaces any mapping for a previous port.
                                          // udp: forward that UDP port too, for Live
    void Stop(); // blocks until the mapping is removed (bounded by router timeouts)

  private:
    struct State;
    State *m_state;
};

// true for addresses that can't be reached from the internet: RFC 1918, carrier-grade NAT (100.64/10), loopback, link-local
bool portmap_is_private_ipv4(const char *addr);

#endif
