/* Copyright (c) 2021 Christof Ressi
 * For information on usage and redistribution, and for a DISCLAIMER OF ALL
 * WARRANTIES, see the file, "LICENSE.txt," in this distribution.  */

/** \file
 * \brief AOO controls
 */

#pragma once

#include "aoo_config.h"

/** \cond DO_NOT_DOCUMENT */
#define AOO_ARG(x) ((void *)&x), sizeof(x)
/** \endcond */

/*------------------------------------------------------*/
/*               AOO source/sink controls               */
/*------------------------------------------------------*/

/** \brief AOO source/sink control constants
 *
 * These are passed to AooSource_control() resp. AooSink_control()
 * internally by helper functions and shouldn't be used directly.
 */
enum
{
    kAooCtlSetId = 0,
    kAooCtlGetId,
    kAooCtlSetFormat,
    kAooCtlGetFormat,
    kAooCtlReset,
    kAooCtlActivate,
    kAooCtlIsActive,
    kAooCtlSetLatency,
    kAooCtlGetLatency,
    kAooCtlSetBufferSize,
    kAooCtlGetBufferSize,
    kAooCtlReportXRun,
    kAooCtlSetResampleMethod,
    kAooCtlGetResampleMethod,
    kAooCtlSetDynamicResampling,
    kAooCtlGetDynamicResampling,
    kAooCtlGetRealSampleRate,
    kAooCtlSetDllBandwidth,
    kAooCtlGetDllBandwidth,
    kAooCtlResetDll,
    kAooCtlSetSinkChannelOffset,
    kAooCtlGetSinkChannelOffset,
    kAooCtlSetPacketSize,
    kAooCtlGetPacketSize,
    kAooCtlSetPingInterval,
    kAooCtlGetPingInterval,
    kAooCtlSetResendData,
    kAooCtlGetResendData,
    kAooCtlSetResendBufferSize,
    kAooCtlGetResendBufferSize,
    kAooCtlSetResendInterval,
    kAooCtlGetResendInterval,
    kAooCtlSetResendLimit,
    kAooCtlGetResendLimit,
    kAooCtlSetRedundancy,
    kAooCtlGetRedundancy,
    kAooCtlSetSourceTimeout,
    kAooCtlGetSourceTimeout,
    kAooCtlSetSinkTimeout,
    kAooCtlGetSinkTimeout,
    kAooCtlSetInviteTimeout,
    kAooCtlGetInviteTimeout,
    kAooCtlGetBufferFillRatio,
    kAooCtlSetBinaryFormat,
    kAooCtlGetBinaryFormat,
    kAooCtlSetStreamTimeSendInterval,
    kAooCtlGetStreamTimeSendInterval,
    kAooCtlSentinel
};

/* default values */

/** \brief default source send buffer size in seconds */
#ifndef AOO_SOURCE_BUFFER_SIZE
 #define AOO_SOURCE_BUFFER_SIZE 0.025
#endif

/** \brief default sink latency in seconds */
#ifndef AOO_SINK_LATENCY
 #define AOO_SINK_LATENCY 0.05
#endif

/** \brief use binary data message format by default */
#ifndef AOO_BINARY_FORMAT
 #define AOO_BINARY_FORMAT 1
#endif

/** \brief stream time send interval in seconds (0 = disabled) */
#ifndef AOO_STREAM_TIME_SEND_INTERVAL
#define AOO_STREAM_TIME_SEND_INTERVAL 1.0
#endif

/** \brief default resample mode*/
#ifndef AOO_RESAMPLE_MODE
#define AOO_RESAMPLE_MODE kAooResampleCubic
#endif

/** \brief enable/disable dynamic resampling by default */
#ifndef AOO_DYNAMIC_RESAMPLING
 #define AOO_DYNAMIC_RESAMPLING 0
#endif

/** \brief default time DLL filter bandwidth */
#ifndef AOO_DLL_BANDWIDTH
 #define AOO_DLL_BANDWIDTH 0.012
#endif

/** \brief default ping interval in seconds */
#ifndef AOO_PING_INTERVAL
 #define AOO_PING_INTERVAL 1.0
#endif

/** \brief default resend buffer size in seconds */
#ifndef AOO_RESEND_BUFFER_SIZE
 #define AOO_RESEND_BUFFER_SIZE 1.0
#endif

/** \brief default send redundancy */
#ifndef AOO_SEND_REDUNDANCY
 #define AOO_SEND_REDUNDANCY 1
#endif

/** \brief enable/disable packet resending by default */
#ifndef AOO_RESEND_DATA
 #define AOO_RESEND_DATA 1
#endif

/** \brief default resend interval in seconds */
#ifndef AOO_RESEND_INTERVAL
 #define AOO_RESEND_INTERVAL 0.01
#endif

/** \brief default resend limit */
#ifndef AOO_RESEND_LIMIT
 #define AOO_RESEND_LIMIT 16
#endif

/* Jamsa: what a peer can make a source or a sink send. Everything a player
 * sends counts against its rate limit at our server, which leaves room for
 * about one more stream on top of the player's own; a peer that asks for
 * more must not use that room up, or the server drops the player's stream
 * for the whole room. */

/** \brief Jamsa: most resend requests in one message (a sink sends no
 * more, a source reads no more; a datagram has room for about 180) */
#ifndef AOO_MAX_DATA_REQUESTS
 #define AOO_MAX_DATA_REQUESTS 32
#endif

/** \brief Jamsa: resent packets per second for all sinks together, as a
 * share of the stream's packets per second, and the burst in packets */
#ifndef AOO_RESEND_SHARE
 #define AOO_RESEND_SHARE 0.5
#endif
#ifndef AOO_RESEND_BURST
 #define AOO_RESEND_BURST 32
#endif

/** \brief Jamsa: the same for any one sink */
#ifndef AOO_RESEND_SHARE_SINK
 #define AOO_RESEND_SHARE_SINK 0.25
#endif
#ifndef AOO_RESEND_BURST_SINK
 #define AOO_RESEND_BURST_SINK 16
#endif

/** \brief Jamsa: most data packets a sink keeps for one source until its
 * next process(): only process() empties that queue, so while the host
 * doesn't call it the packets would pile up without bound and the next
 * process() would work through all of them. 1024 is more than 2.5 s of
 * the densest stream we take (120-sample frames) and more than 6 times the
 * largest jitter buffer. */
#ifndef AOO_MAX_QUEUED_PACKETS
 #define AOO_MAX_QUEUED_PACKETS 1024
#endif

/** \brief Jamsa: at most one pong, one /start in answer to a request and one
 * /start request per peer in this many seconds (pings come every second) */
#ifndef AOO_REPLY_INTERVAL
 #define AOO_REPLY_INTERVAL 0.1
#endif

/** \brief default source timeout */
#ifndef AOO_SOURCE_TIMEOUT
 #define AOO_SOURCE_TIMEOUT 10.0
#endif

/** \brief default invite timeout */
#ifndef AOO_INVITE_TIMEOUT
 #define AOO_INVITE_TIMEOUT 1.0
#endif

/*------------------------------------------------------*/
/*               user defined controls                  */
/*------------------------------------------------------*/

/* User defined controls (for custom AOO versions)
 * must start from kAooCtlUserDefined, for example:
 *
 * enum MyAooControls
 * {
 *     kMyControl1 = kAooCtlUserDefined
 *     kMyControl2,
 *     kMyControl3,
 *     ...
 * };
 */

/** \brief offset for user defined controls */
#define kAooCtlUserDefined 10000

/** \brief Jamsa's controls (see ninjam/live/README.md) */
enum
{
    /** (AooSource) send the data packets once to a broadcast endpoint */
    kAooCtlSetBroadcastEndpoint = kAooCtlUserDefined,
    /** (AooSource) is broadcast mode on? */
    kAooCtlGetBroadcast,
    /** (AooSink) let a source (or all) time out on the next process() */
    kAooCtlRemoveSource
};
