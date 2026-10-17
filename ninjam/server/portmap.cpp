/*
    NINJAM Server - portmap.cpp

    NINJAM is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.
*/

/*

  Router port mapping, see portmap.h.

  Two environment variables skip discovery, for routers that don't answer
  multicast and for the tests:
    NINJAM_UPNP_ROOTDESC   URL of the router's UPnP description (rootDesc.xml)
    NINJAM_NATPMP_GATEWAY  IPv4 address of the NAT-PMP gateway

*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "portmap.h"

void logText(const char *s, ...);

bool portmap_is_private_ipv4(const char *addr)
{
  unsigned int a,b,c,d;
  char extra;
  if (!addr || sscanf(addr,"%u.%u.%u.%u%c",&a,&b,&c,&d,&extra) != 4) return false;
  if (a>255 || b>255 || c>255 || d>255) return false;

  return a == 10 ||
         a == 127 ||
         a == 0 ||
         (a == 100 && b >= 64 && b < 128) || // carrier-grade NAT
         (a == 169 && b == 254) ||
         (a == 172 && b >= 16 && b < 32) ||
         (a == 192 && b == 168);
}

#ifndef NINJAM_PORTMAP

struct PortMapper::State { };

PortMapper::PortMapper() : m_state(NULL) { }
PortMapper::~PortMapper() { }
void PortMapper::Stop() { }

void PortMapper::Start(int port, bool udp)
{
  if (udp)
    logText("[portmap] this server was built without port mapping support (build with PORTMAP=1). "
            "Forward TCP port %d on your router to this machine by hand, and UDP port %d for Live.\n",port,port);
  else
    logText("[portmap] this server was built without port mapping support (build with PORTMAP=1). "
            "Forward TCP port %d on your router to this machine by hand.\n",port);
}

#else

#include <time.h>
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <sys/select.h>
#include <arpa/inet.h>
#endif

#include <thread>
#include <mutex>
#include <condition_variable>
#include <chrono>

#include <miniupnpc/miniupnpc.h>
#include <miniupnpc/upnpcommands.h>
#include <natpmp.h>

#define PORTMAP_DESC "NINJAM server"
#define PORTMAP_LEASE 3600 // seconds asked for; renewed at half of what the router grants
#define PORTMAP_PERMANENT_RECHECK 1800 // routers forget even permanent mappings when they reboot
#define PORTMAP_RETRY 300 // after a failure
#define UPNP_DISCOVER_MS 2000
#define NATPMP_WAIT_MS 3000

struct PortMapper::State
{
  std::thread thread;
  std::mutex mutex;
  std::condition_variable cv;
  bool stop;
  int port;
  bool udp; // UDP too, for Live

  // the active mapping
  enum { NONE, UPNP, NATPMP } method;
  struct UPNPUrls urls;
  struct IGDdatas igd;
  char lanaddr[64];
  bool upnp_permanent;
  natpmp_t natpmp;
  int natpmp_public_port;
  bool udp_mapped; // the router has a UDP forward of ours, to remove when done
  bool udp_ok; // and it works for Live: the same public port as TCP
  bool udp_warned; // the log said it doesn't

  State(int p, bool u) : stop(false), port(p), udp(u), method(NONE), upnp_permanent(false), natpmp_public_port(0),
                         udp_mapped(false), udp_ok(false), udp_warned(false)
  {
    memset(&urls,0,sizeof(urls));
    memset(&igd,0,sizeof(igd));
    memset(&natpmp,0,sizeof(natpmp));
    lanaddr[0]=0;
  }

  bool Stopping()
  {
    std::lock_guard<std::mutex> lock(mutex);
    return stop;
  }

  // returns true if asked to stop
  bool Sleep(int seconds)
  {
    std::unique_lock<std::mutex> lock(mutex);
    return cv.wait_for(lock,std::chrono::seconds(seconds),[this]{ return stop; });
  }

  void Run();
  int Map(); // returns the lease in seconds, 0 for permanent, -1 on failure
  void Unmap();

  bool UpnpFind();
  int UpnpAdd(bool quiet);
  bool NatpmpInit();
  int NatpmpWait(natpmpresp_t *resp, int timeout_ms, bool give_up_on_stop=true);
  int NatpmpAdd(bool quiet, uint32_t lifetime);

  void ReportAddress(const char *how, const char *ext, int ext_port);
  void ReportUdp(bool ok, const char *why);
};

void PortMapper::State::ReportAddress(const char *how, const char *ext, int ext_port)
{
  const char *proto = udp_ok ? "TCP and UDP" : "TCP";
  if (!ext || !ext[0])
  {
    logText("[portmap] %s: router forwards %s port %d to this machine, but did not say its public address\n",how,proto,ext_port);
  }
  else if (portmap_is_private_ipv4(ext))
  {
    logText("[portmap] %s: router forwards %s port %d to this machine, but the router's own internet address is %s, "
            "which is not public. Your internet provider probably uses carrier-grade NAT, or another router sits in "
            "front of this one, so people outside your network still can't connect. Ask your provider for a public "
            "IPv4 address, or run the server on a hosted machine.\n",how,proto,ext_port,ext);
  }
  else
  {
    logText("[portmap] %s: people can connect to %s:%d\n",how,ext,ext_port);
  }
}

// The UDP forward is best effort: TCP works without it. Its failure is a warning,
// not a [portmap] line, so the plug-in that hosts this server keeps showing the
// address people connect to. Said once, not at every renewal.
void PortMapper::State::ReportUdp(bool ok, const char *why)
{
  udp_ok = ok;
  if (ok) udp_warned = false;
  if (ok || udp_warned) return;
  udp_warned = true;
  logText("warning: Live: the router did not forward UDP port %d (%s), so players outside your network "
          "can't play Live with you; they still hear you through intervals\n",port,why);
}

bool PortMapper::State::UpnpFind()
{
  int r=0;
  const char *rootdesc=getenv("NINJAM_UPNP_ROOTDESC");
  if (rootdesc && *rootdesc)
  {
    r=UPNP_GetIGDFromUrl(rootdesc,&urls,&igd,lanaddr,sizeof(lanaddr));
  }
  else
  {
    // search for all types at once: upnpDiscover() tries them one after another, waiting the full delay for each
    static const char * const types[]={
      "urn:schemas-upnp-org:device:InternetGatewayDevice:1",
      "urn:schemas-upnp-org:device:InternetGatewayDevice:2",
      "urn:schemas-upnp-org:service:WANIPConnection:1",
      "urn:schemas-upnp-org:service:WANIPConnection:2",
      "urn:schemas-upnp-org:service:WANPPPConnection:1",
      NULL
    };
    int err=0;
    struct UPNPDev *devs=upnpDiscoverDevices(types,UPNP_DISCOVER_MS,NULL,NULL,0,0,2,&err,1);
    if (!devs) return false;

#if MINIUPNPC_API_VERSION >= 18
    char wanaddr[64];
    r=UPNP_GetValidIGD(devs,&urls,&igd,lanaddr,sizeof(lanaddr),wanaddr,sizeof(wanaddr));
    // 1 = connected IGD, 2 = connected but with a private address (reported later), higher = unusable
    if (r != 1 && r != 2)
#else
    r=UPNP_GetValidIGD(devs,&urls,&igd,lanaddr,sizeof(lanaddr));
    // 1 = connected IGD, higher = not connected or not an IGD
    if (r != 1)
#endif
    {
      if (r) FreeUPNPUrls(&urls);
      r=0;
    }
    freeUPNPDevlist(devs);
  }
  if (!r)
  {
    memset(&urls,0,sizeof(urls));
    return false;
  }
  return true;
}

// returns the lease in seconds, 0 for permanent, -1 on failure
int PortMapper::State::UpnpAdd(bool quiet)
{
  char portstr[16], lease[16];
  snprintf(portstr,sizeof(portstr),"%d",port);
  snprintf(lease,sizeof(lease),"%d",upnp_permanent ? 0 : PORTMAP_LEASE);

  int r=UPNP_AddPortMapping(urls.controlURL,igd.first.servicetype,portstr,portstr,lanaddr,PORTMAP_DESC,"TCP",NULL,lease);
  if (r == 725 && !upnp_permanent) // OnlyPermanentLeasesSupported
  {
    upnp_permanent=true;
    r=UPNP_AddPortMapping(urls.controlURL,igd.first.servicetype,portstr,portstr,lanaddr,PORTMAP_DESC,"TCP",NULL,"0");
  }
  if (r)
  {
    if (!quiet)
    {
      if (r == 718)
        logText("[portmap] UPnP: TCP port %d on the router is already forwarded to another machine\n",port);
      else
        logText("[portmap] UPnP: router refused to forward TCP port %d (error %d)\n",port,r);
    }
    return -1;
  }

  if (udp)
  {
    // best effort, with the lease TCP got: its failure never fails the TCP forward
    r=UPNP_AddPortMapping(urls.controlURL,igd.first.servicetype,portstr,portstr,lanaddr,PORTMAP_DESC,"UDP",NULL,
                          upnp_permanent ? "0" : lease);
    char why[96];
    if (r == 718) snprintf(why,sizeof(why),"UPnP: the router forwards it to another machine");
    else snprintf(why,sizeof(why),"UPnP error %d",r);
    if (!r) udp_mapped=true;
    ReportUdp(r == 0,why);
  }
  return upnp_permanent ? 0 : PORTMAP_LEASE;
}

bool PortMapper::State::NatpmpInit()
{
  const char *gw=getenv("NINJAM_NATPMP_GATEWAY");
  in_addr_t forced=0;
  if (gw && *gw)
  {
    forced=inet_addr(gw);
    if (forced == INADDR_NONE) return false;
  }
  return initnatpmp(&natpmp,forced?1:0,forced) == 0;
}

int PortMapper::State::NatpmpWait(natpmpresp_t *resp, int timeout_ms, bool give_up_on_stop)
{
  const time_t deadline=time(NULL)+(timeout_ms+999)/1000;
  int r;
  do
  {
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(natpmp.s,&fds);
    struct timeval tv;
    if (getnatpmprequesttimeout(&natpmp,&tv) < 0 || tv.tv_sec < 0)
    {
      tv.tv_sec=0;
      tv.tv_usec=0;
    }
    if (tv.tv_sec > 0 || tv.tv_usec > 250000) // wake up regularly to check for Stop()
    {
      tv.tv_sec=0;
      tv.tv_usec=250000;
    }
    select(natpmp.s+1,&fds,NULL,NULL,&tv);
    r=readnatpmpresponseorretry(&natpmp,resp);
  } while (r == NATPMP_TRYAGAIN && time(NULL) < deadline && !(give_up_on_stop && Stopping()));
  return r;
}

int PortMapper::State::NatpmpAdd(bool quiet, uint32_t lifetime)
{
  natpmpresp_t resp;
  int r=sendnewportmappingrequest(&natpmp,NATPMP_PROTOCOL_TCP,port,natpmp_public_port?natpmp_public_port:port,lifetime);
  if (r >= 0) r=NatpmpWait(&resp,NATPMP_WAIT_MS);
  if (r < 0 || resp.type != NATPMP_RESPTYPE_TCPPORTMAPPING)
  {
    if (!quiet)
    {
      if (r == NATPMP_TRYAGAIN)
        logText("[portmap] NAT-PMP: router did not answer\n");
      else
        logText("[portmap] NAT-PMP: router refused to forward TCP port %d (error %d)\n",port,r);
    }
    return -1;
  }
  natpmp_public_port=resp.pnu.newportmapping.mappedpublicport;

  if (udp)
  {
    // best effort, after TCP (libnatpmp does one request at a time). Live clients send
    // UDP to the port they reached TCP on, so it has to be TCP's public port.
    natpmpresp_t uresp;
    int u=sendnewportmappingrequest(&natpmp,NATPMP_PROTOCOL_UDP,port,natpmp_public_port,lifetime);
    if (u >= 0) u=NatpmpWait(&uresp,NATPMP_WAIT_MS);
    char why[96];
    bool ok=false;
    if (u < 0 || uresp.type != NATPMP_RESPTYPE_UDPPORTMAPPING)
    {
      if (u == NATPMP_TRYAGAIN) snprintf(why,sizeof(why),"NAT-PMP: the router did not answer");
      else snprintf(why,sizeof(why),"NAT-PMP error %d",u);
    }
    else
    {
      udp_mapped=true;
      ok=uresp.pnu.newportmapping.mappedpublicport == natpmp_public_port;
      snprintf(why,sizeof(why),"NAT-PMP: it gave public port %d, not %d",
               (int)uresp.pnu.newportmapping.mappedpublicport,natpmp_public_port);
    }
    ReportUdp(ok,why);
  }
  return (int)resp.pnu.newportmapping.lifetime;
}

int PortMapper::State::Map()
{
  if (method == UPNP) return UpnpAdd(false);
  if (method == NATPMP)
  {
    int r=NatpmpAdd(false,PORTMAP_LEASE);
    return r > 0 ? r : -1;
  }

  if (UpnpFind())
  {
    int lease=UpnpAdd(false);
    if (lease >= 0)
    {
      method=UPNP;
      char ext[64];
      ext[0]=0;
      if (UPNP_GetExternalIPAddress(urls.controlURL,igd.first.servicetype,ext)) ext[0]=0;
      ReportAddress("UPnP",ext,port);
      return lease;
    }
    FreeUPNPUrls(&urls);
    memset(&urls,0,sizeof(urls));
  }
  if (Stopping()) return -1;

  if (NatpmpInit())
  {
    natpmp_public_port=0;
    int lease=NatpmpAdd(true,PORTMAP_LEASE);
    if (lease > 0)
    {
      method=NATPMP;
      char ext[64];
      ext[0]=0;
      natpmpresp_t resp;
      if (sendpublicaddressrequest(&natpmp) >= 0 &&
          NatpmpWait(&resp,NATPMP_WAIT_MS) >= 0 &&
          resp.type == NATPMP_RESPTYPE_PUBLICADDRESS)
      {
        inet_ntop(AF_INET,&resp.pnu.publicaddress.addr,ext,sizeof(ext));
      }
      ReportAddress("NAT-PMP",ext,natpmp_public_port);
      return lease;
    }
    closenatpmp(&natpmp);
  }
  return -1;
}

void PortMapper::State::Unmap()
{
  // the UDP forward goes first and quietly; the line about TCP says whether it went too
  bool udp_removed=false;
  if (method == UPNP)
  {
    char portstr[16];
    snprintf(portstr,sizeof(portstr),"%d",port);
    if (udp_mapped) udp_removed=UPNP_DeletePortMapping(urls.controlURL,igd.first.servicetype,portstr,"UDP",NULL) == 0;
    int r=UPNP_DeletePortMapping(urls.controlURL,igd.first.servicetype,portstr,"TCP",NULL);
    if (r) logText("[portmap] UPnP: could not remove the forward for TCP port %d (error %d)\n",port,r);
    else logText("[portmap] UPnP: removed the forward for %s port %d\n",udp_removed ? "TCP and UDP" : "TCP",port);
    FreeUPNPUrls(&urls);
    memset(&urls,0,sizeof(urls));
  }
  else if (method == NATPMP)
  {
    natpmpresp_t resp;
    int r;
    if (udp_mapped)
    {
      r=sendnewportmappingrequest(&natpmp,NATPMP_PROTOCOL_UDP,port,natpmp_public_port,0);
      if (r >= 0) r=NatpmpWait(&resp,2000,false);
      udp_removed=r >= 0 && resp.type == NATPMP_RESPTYPE_UDPPORTMAPPING;
    }
    r=sendnewportmappingrequest(&natpmp,NATPMP_PROTOCOL_TCP,port,natpmp_public_port,0);
    if (r >= 0) r=NatpmpWait(&resp,2000,false); // usually called while stopping
    if (r < 0) logText("[portmap] NAT-PMP: could not remove the forward for TCP port %d (error %d)\n",port,r);
    else logText("[portmap] NAT-PMP: removed the forward for %s port %d\n",udp_removed ? "TCP and UDP" : "TCP",port);
    closenatpmp(&natpmp);
  }
  method=NONE;
  udp_mapped=false;
  udp_ok=false;
}

void PortMapper::State::Run()
{
  logText("[portmap] asking the router to forward %s port %d to this machine\n",udp ? "TCP and UDP" : "TCP",port);
  bool reported_failure=false;
  while (!Stopping())
  {
    int lease=Map();
    int wait;
    if (lease < 0 && Stopping()) break;
    if (lease < 0)
    {
      if (method != NONE)
      {
        logText("[portmap] lost the forward for TCP port %d, trying again\n",port);
        Unmap();
        continue;
      }
      if (!reported_failure)
      {
        char udp_too[64];
        udp_too[0]=0;
        if (udp) snprintf(udp_too,sizeof(udp_too),", and UDP port %d for Live",port);
        logText("[portmap] could not find a router that supports UPnP or NAT-PMP. Forward TCP port %d on your router "
                "to this machine by hand%s, or turn on UPnP in the router's settings. Will keep trying every %d minutes.\n",
                port,udp_too,PORTMAP_RETRY/60);
        reported_failure=true;
      }
      wait=PORTMAP_RETRY;
    }
    else
    {
      reported_failure=false;
      wait=lease > 0 ? lease/2 : PORTMAP_PERMANENT_RECHECK;
      if (wait < 30) wait=30;
    }
    if (Sleep(wait)) break;
  }
  Unmap();
}

PortMapper::PortMapper() : m_state(NULL) { }

PortMapper::~PortMapper()
{
  Stop();
}

void PortMapper::Start(int port, bool udp)
{
  Stop();
  m_state=new State(port,udp);
  State *s=m_state;
  m_state->thread=std::thread([s]{ s->Run(); });
}

void PortMapper::Stop()
{
  if (!m_state) return;
  {
    std::lock_guard<std::mutex> lock(m_state->mutex);
    m_state->stop=true;
  }
  m_state->cv.notify_all();
  m_state->thread.join();
  delete m_state;
  m_state=NULL;
}

#endif
