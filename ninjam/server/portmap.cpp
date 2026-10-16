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

void PortMapper::Start(int port)
{
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

  // the active mapping
  enum { NONE, UPNP, NATPMP } method;
  struct UPNPUrls urls;
  struct IGDdatas igd;
  char lanaddr[64];
  bool upnp_permanent;
  natpmp_t natpmp;
  int natpmp_public_port;

  // what went wrong in the last Map() without a mapping, for the message after the first failure
  enum { UPNP_NO_ANSWER, UPNP_SEND_FAILED, UPNP_NOT_A_ROUTER, UPNP_REFUSED } upnp_problem;

  State(int p) : stop(false), port(p), method(NONE), upnp_permanent(false), natpmp_public_port(0), upnp_problem(UPNP_NO_ANSWER)
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
  void ReportFailure();
};

void PortMapper::State::ReportFailure()
{
  const int mins=PORTMAP_RETRY/60;
  switch (upnp_problem)
  {
    case UPNP_REFUSED: // UpnpAdd() said why
      logText("[portmap] the router would not forward TCP port %d. Forward it on your router to this machine by hand. "
              "Will keep trying every %d minutes.\n",port,mins);
    break;
    case UPNP_SEND_FAILED:
      logText("[portmap] could not ask the router: sending to the local network failed. On macOS, allow your music "
              "app in System Settings > Privacy & Security > Local Network, then restart it. Otherwise forward TCP "
              "port %d on your router to this machine by hand. Will keep trying every %d minutes.\n",port,mins);
    break;
    case UPNP_NOT_A_ROUTER:
      logText("[portmap] UPnP devices answered, but none of them is a router that forwards ports. If your Wi-Fi "
              "system runs in access point (bridge) mode, turn on UPnP on the router in front of it, or forward TCP "
              "port %d there to this machine by hand. Will keep trying every %d minutes.\n",port,mins);
    break;
    default:
      logText("[portmap] could not find a router that supports UPnP or NAT-PMP. Forward TCP port %d on your router "
              "to this machine by hand, or turn on UPnP in the router's settings. Will keep trying every %d minutes.\n",
              port,mins);
    break;
  }
}

void PortMapper::State::ReportAddress(const char *how, const char *ext, int ext_port)
{
  if (!ext || !ext[0])
  {
    logText("[portmap] %s: router forwards TCP port %d to this machine, but did not say its public address\n",how,ext_port);
  }
  else if (portmap_is_private_ipv4(ext))
  {
    logText("[portmap] %s: router forwards TCP port %d to this machine, but the router's own internet address is %s, "
            "which is not public. Your internet provider probably uses carrier-grade NAT, or another router sits in "
            "front of this one, so people outside your network still can't connect. Ask your provider for a public "
            "IPv4 address, or run the server on a hosted machine.\n",how,ext_port,ext);
  }
  else
  {
    logText("[portmap] %s: people can connect to %s:%d\n",how,ext,ext_port);
  }
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
    if (!devs)
    {
      upnp_problem = err == UPNPDISCOVER_SOCKET_ERROR ? UPNP_SEND_FAILED : UPNP_NO_ANSWER;
      return false;
    }

    // Take any router, whatever it says about its internet connection. The library calls a router
    // "not connected" when its own internet address is private, which is the usual case for a mesh
    // system (TP-Link Deco and the like) plugged into the provider's router: the mapping still
    // works on this router, and ReportAddress() explains the other one.
#if MINIUPNPC_API_VERSION >= 18
    char wanaddr[64];
    r=UPNP_GetValidIGD(devs,&urls,&igd,lanaddr,sizeof(lanaddr),wanaddr,sizeof(wanaddr));
    // 1 = connected IGD, 2 = connected with a private address, 3 = IGD not connected, 4 = not an IGD
    const int last_igd=3;
#else
    r=UPNP_GetValidIGD(devs,&urls,&igd,lanaddr,sizeof(lanaddr));
    // 1 = connected IGD, 2 = IGD not connected or with a private address, 3 = not an IGD
    const int last_igd=2;
#endif
    if (r < 1 || r > last_igd)
    {
      if (r > 0) FreeUPNPUrls(&urls);
      r=0;
      upnp_problem=UPNP_NOT_A_ROUTER;
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

  upnp_problem=UPNP_NO_ANSWER;
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
    upnp_problem=UPNP_REFUSED;
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
  if (method == UPNP)
  {
    char portstr[16];
    snprintf(portstr,sizeof(portstr),"%d",port);
    int r=UPNP_DeletePortMapping(urls.controlURL,igd.first.servicetype,portstr,"TCP",NULL);
    if (r) logText("[portmap] UPnP: could not remove the forward for TCP port %d (error %d)\n",port,r);
    else logText("[portmap] UPnP: removed the forward for TCP port %d\n",port);
    FreeUPNPUrls(&urls);
    memset(&urls,0,sizeof(urls));
  }
  else if (method == NATPMP)
  {
    natpmpresp_t resp;
    int r=sendnewportmappingrequest(&natpmp,NATPMP_PROTOCOL_TCP,port,natpmp_public_port,0);
    if (r >= 0) r=NatpmpWait(&resp,2000,false); // usually called while stopping
    if (r < 0) logText("[portmap] NAT-PMP: could not remove the forward for TCP port %d (error %d)\n",port,r);
    else logText("[portmap] NAT-PMP: removed the forward for TCP port %d\n",port);
    closenatpmp(&natpmp);
  }
  method=NONE;
}

void PortMapper::State::Run()
{
  logText("[portmap] asking the router to forward TCP port %d to this machine\n",port);
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
        ReportFailure();
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

void PortMapper::Start(int port)
{
  Stop();
  m_state=new State(port);
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
