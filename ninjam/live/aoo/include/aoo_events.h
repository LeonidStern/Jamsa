/* Copyright (c) 2021 Christof Ressi
 * For information on usage and redistribution, and for a DISCLAIMER OF ALL
 * WARRANTIES, see the file, "LICENSE.txt," in this distribution.  */

/** \file
 * \brief AOO event types
 */

#pragma once

#include "aoo_config.h"
#include "aoo_defines.h"
#include "aoo_types.h"

AOO_PACK_BEGIN

/*--------------------------------------------*/

/** \brief AOO source/sink event types */
AOO_ENUM(AooEventType)
{
    /** generic error event */
    kAooEventError = 0,
    /*----------------------------------*/
    /*     AooSource/AooSink events     */
    /*----------------------------------*/
    /** AooSource: received ping from sink */
    kAooEventSinkPing,
    /** AooSink: received ping from source */
    kAooEventSourcePing,
    /** AooSource: invited by sink */
    kAooEventInvite,
    /** AooSource: uninvited by sink */
    kAooEventUninvite,
    /** AooSource: sink added */
    kAooEventSinkAdd,
    /** AooSource: sink removed */
    kAooEventSinkRemove,
    /** AooSink: source added */
    kAooEventSourceAdd,
    /** AooSink: source removed */
    kAooEventSourceRemove,
    /** AooSink: stream started */
    kAooEventStreamStart,
    /** AooSink: stream stopped */
    kAooEventStreamStop,
    /** AooSink: stream changed state */
    kAooEventStreamState,
    /** AooSink: stream time stamp */
    kAooEventStreamTime,
    /** AooSink: stream latency changed */
    kAooEventStreamLatency,
    /** AooSink: source format changed */
    kAooEventFormatChange,
    /** AooSink: invitation has been declined */
    kAooEventInviteDecline,
    /** AooSink: invitation timed out */
    kAooEventInviteTimeout,
    /** AooSink: uninvitation timed out */
    kAooEventUninviteTimeout,
    /** AooSink: buffer overrun */
    kAooEventBufferOverrun,
    /** AooSink: buffer underrun */
    kAooEventBufferUnderrun,
    /** AooSink: blocks had to be skipped/dropped */
    kAooEventBlockDrop,
    /** AooSink: blocks have been resent */
    kAooEventBlockResend,
    /** AooSink: empty blocks caused by source xrun */
    kAooEventBlockXRun,
    /** AooSource: frames have been resent */
    kAooEventFrameResend,
    /** start of user defined events (for custom AOO versions) */
    kAooEventCustom = 10000
};

/*--------------------------------------------*/

/** \brief common header of all event structs */
#define AOO_EVENT_HEADER            \
    /** \cond DO_NOT_DOCUMENT */    \
    AooEventType type;              \
    AooUInt32 structSize;           \
    /** \endcond */

/** \brief base event */
typedef struct AooEventBase
{
    AOO_EVENT_HEADER
} AooEventBase;

/** \brief error event */
typedef struct AooEventError
{
    AOO_EVENT_HEADER
    /** platform- or application-specific error code */
    AooInt32 errorCode;
    /** error message */
    const AooChar *errorMessage;
} AooEventError;

/*-------------------------------------------------*/
/*              AOO source/sink events             */
/*-------------------------------------------------*/

/** \brief generic source/sink event */
typedef struct AooEventEndpoint
{
    AOO_EVENT_HEADER
    AooEndpoint endpoint; /**< source/sink endpoint */
} AooEventEndpoint;

/** \brief received ping (reply) from source
 *
 * \details The effective network round trip time (RTT) can be
 * calculated with the formula `(t4 - t1) - (t3 - t2)`.
 *
 * \note The clocks on both machines can diverge significantly,
 * so the end-to-end delay can *not* be calculated with `(t2 - t1)`
 * resp. `(t4 - t3)`! It can only be estimated, e.g. with `RTT * 0.5`.
 *
 * The time offset between source and sink can be estimated with
 * `((t2 - t1) + (t3 - t4)) * 0.5`. Together with AooEventStreamTime
 * this can  be used to synchronize multiple streams from different
 * machines.
 *
 * Ideally, you would filter all these values to account for network jitter.
 * See also https://en.wikipedia.org/wiki/Network_Time_Protocol#Clock_synchronization_algorithm
 */
typedef struct AooEventSourcePing
{
    AOO_EVENT_HEADER
    AooEndpoint endpoint; /**< source endpoint */
    AooNtpTime t1; /**< sink send time */
    AooNtpTime t2; /**< source receive time */
    AooNtpTime t3; /**< source send time */
    AooNtpTime t4; /**< sink receive time */
} AooEventSourcePing;

/** \brief received ping (reply) from sink
 *
 *  \see AooEventSourcePing
 */
typedef struct AooEventSinkPing
{
    AOO_EVENT_HEADER
    AooEndpoint endpoint; /**< sink endpoint */
    AooNtpTime t1; /**< source send time */
    AooNtpTime t2; /**< sink receive time */
    AooNtpTime t3; /**< sink send time */
    AooNtpTime t4; /**< source receive time */
    float packetLoss; /**< packet loss percentage (0.0 - 1.0) */
} AooEventSinkPing;

/** \brief (AooSink) a new source has been added */
typedef AooEventEndpoint AooEventSourceAdd;

/** \brief (AooSink) a source has been removed */
typedef AooEventEndpoint AooEventSourceRemove;

/** \brief (AooSource) a sink has been added */
typedef AooEventEndpoint AooEventSinkAdd;

/** \brief (AooSource) a sink has been removed */
typedef AooEventEndpoint AooEventSinkRemove;

/** \brief (AooSink) buffer overrun occurred */
typedef AooEventEndpoint AooEventBufferOverrun;

/** \brief (AooSink) buffer underrun occurred */
typedef AooEventEndpoint AooEventBufferUnderrun;

/** \brief (AooSink) a new stream has started */
typedef struct AooEventStreamStart
{
    AOO_EVENT_HEADER
    AooEndpoint endpoint; /**< source endpoint */
    AooNtpTime tt; /**< stream start time (source-side) */
    const AooData *metadata; /**< optional stream metadata */
} AooEventStreamStart;

/** \brief (AooSink) a stream has stopped */
typedef AooEventEndpoint AooEventStreamStop;

/** \brief stream states */
AOO_ENUM(AooStreamState)
{
    /** stream is (temporarily) inactive */
    kAooStreamStateInactive = 0,
    /** stream is active */
    kAooStreamStateActive = 1,
    /** stream is buffering */
    kAooStreamStateBuffering = 2
};

/** \brief (AooSink) the stream state has changed */
typedef struct AooEventStreamState
{
    AOO_EVENT_HEADER
    AooEndpoint endpoint; /**< source endpoint */
    AooStreamState state; /**< new stream state */
    AooInt32 sampleOffset; /**< corresponding sample offset */
} AooEventStreamState;

/** \brief (AooSink) the source stream format has changed */
typedef struct AooEventFormatChange
{
    AOO_EVENT_HEADER
    AooEndpoint endpoint; /**< source endpoint */
    const AooFormat *format; /**< new stream format */
} AooEventFormatChange;

/** \brief (AooSink) the stream latency has changed */
typedef struct AooEventStreamLatency {
    AOO_EVENT_HEADER
    /** source endpoint */
    AooEndpoint endpoint;
    /** source reblock/resample latency + codec delay;
     *  typically constant for the duration of the stream */
    AooSeconds sourceLatency;
    /** sink reblock/resample latency + codec delay;
     *  typically constant for the duration of the stream */
    AooSeconds sinkLatency;
    /** jitter buffer latency;
     *  can change on overflow/underflow) */
    AooSeconds bufferLatency;
} AooEventStreamLatency;

/** \brief (AooSource) received invitation by sink */
typedef struct AooEventInvite
{
    AOO_EVENT_HEADER
    AooEndpoint endpoint; /**< sink endpoint */
    AooId token; /**< stream token (for AooSource::handleInvite) */
    const AooData *metadata; /**< optional metadata */
} AooEventInvite;

/** \brief (AooSource) received uninvitation by sink */
typedef struct AooEventUninvite
{
    AOO_EVENT_HEADER
    AooEndpoint endpoint; /**< sink endpoint */
    AooId token; /**< stream token (for AooSource::handleUninvite) */
} AooEventUninvite;

/** \brief (AooSink) invitation has been declined */
typedef AooEventEndpoint AooEventInviteDecline;

/** \brief (AooSink) invitation has timed out */
typedef AooEventEndpoint AooEventInviteTimeout;

/** \brief (AooSink) uninvitation has timed out */
typedef AooEventEndpoint AooEventUninviteTimeout;

/** \brief (AooSink) stream time event
 *
 * \details Both the source and local time are measured
 * in sample time, using the respective stream start time
 * as the reference point.
 *
 * The stream time can be used to synchronize multiple
 * streams from the same machine. If you know the actual
 * clock time offset, you can even synchronize streams
 * from different machines.
 * \see AooEventSourcePing and AooEventSinkPing.
 */
typedef struct AooEventStreamTime
{
    AOO_EVENT_HEADER
    AooEndpoint endpoint; /**< source endpoint */
    AooNtpTime sourceTime; /**< source time stamp (remote) */
    AooNtpTime sinkTime; /**< sink time stamp (local) */
    AooInt32 sampleOffset; /**< corresponding sample offset */
} AooEventStreamTime;

/** \brief (AooSink) generic stream diagnostic event */
typedef struct AooEventBlock
{
    AOO_EVENT_HEADER
    AooEndpoint endpoint; /**< source endpoint */
    AooInt32 count; /**< event count */
} AooEventBlock;

/** \brief (AooSink) blocks had to be skipped/dropped */
typedef AooEventBlock AooEventBlockDrop;

/** \brief (AooSink) blocks have been resent */
typedef AooEventBlock AooEventBlockResend;

/** \brief (AooSink) empty blocks caused by source xrun */
typedef AooEventBlock AooEventBlockXRun;

/** \brief (AooSource) frames have been resent */
typedef struct AooEventFrameResend
{
    AOO_EVENT_HEADER
    AooEndpoint endpoint; /**< sink end point */
    AooInt32 count;  /**< number of resent frames */
} AooEventFrameResend;

/*----------------------------------------------------*/

/** \brief union holding all possible events */
union AooEvent
{
    AooEventType type; /**< \brief the event type */
    AooEventBase base; /**< \brief base */
    AooEventError error; /**< \brief error */
    /* AOO source/sink events */
    AooEventEndpoint endpoint; /**< \brief endpoint */
    AooEventSourcePing sourcePing; /**< \brief source ping */
    AooEventSinkPing sinkPing; /**< \brief sink ping */
    AooEventInvite invite; /**< \brief invite */
    AooEventUninvite uninvite; /**< \brief uninvite */
    AooEventSinkAdd sinkAdd; /**< \brief sink added */
    AooEventSinkRemove sinkRemove; /**< \brief sink removed */
    AooEventSourceAdd sourceAdd; /**< \brief source added */
    AooEventSourceRemove sourceRemove;/**< \brief source removed */
    AooEventStreamStart streamStart; /**< \brief stream started */
    AooEventStreamStop streamStop; /**< \brief stream stopped */
    AooEventStreamState streamState; /**< \brief stream state changed */
    AooEventStreamTime streamTime; /**< \brief stream time */
    AooEventStreamLatency streamLatency; /**< \brief stream latency */
    AooEventFormatChange formatChange; /**< \brief format changed */
    AooEventInviteDecline inviteDecline; /**< \brief invitation declined */
    AooEventInviteTimeout inviteTimeout; /**< \brief invitation timed out */
    AooEventUninviteTimeout uninviteTimeout; /**< \brief uninvitation timed out */
    AooEventBufferOverrun bufferOverrrun; /**< \brief jitter buffer overrun */
    AooEventBufferUnderrun bufferUnderrun; /**< \brief jitter buffer underrun */
    AooEventBlockDrop blockDrop; /**< \brief block dropped */
    AooEventBlockResend blockResend; /**< \brief bock resent */
    AooEventBlockXRun blockXRun; /**< \brief empty block for source xrun */
    AooEventFrameResend frameResend; /**< \brief frames resent */
};

/*-------------------------------------------------*/

AOO_PACK_END
