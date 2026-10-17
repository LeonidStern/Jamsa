/* Copyright (c) 2021 Christof Ressi
 * For information on usage and redistribution, and for a DISCLAIMER OF ALL
 * WARRANTIES, see the file, "LICENSE.txt," in this distribution.  */

/** \file
 * \brief compile time configuration
 */

#pragma once

/*---------- global compile time settings ----------*/

/** \brief internal networking support (AooClient and AooServer): not available in this copy */
#ifndef AOO_NET
# define AOO_NET 0
#endif
#if AOO_NET
# error "AOO_NET is not supported (net sources were removed)"
#endif

/** \brief audio sample size in bits */
#ifndef AOO_SAMPLE_SIZE
#define AOO_SAMPLE_SIZE 32
#endif

/** \brief max. log level
 * (Jamsa: Error by default, so that nothing a peer sends gets logged, see
 * README.md; upstream's default is Warning) */
#ifndef AOO_LOG_LEVEL
#define AOO_LOG_LEVEL kAooLogLevelError
#endif

/** \brief custom allocator support  */
#ifndef AOO_CUSTOM_ALLOCATOR
# define AOO_CUSTOM_ALLOCATOR 0
#endif

/** \brief IPv6 support */
#ifndef AOO_USE_IPV6
#define AOO_USE_IPV6 1
#endif

/** \brief default UDP packet size */
#ifndef AOO_PACKET_SIZE
# define AOO_PACKET_SIZE 512
#endif

/** \brief max. UDP packet size */
#ifndef AOO_MAX_PACKET_SIZE
# define AOO_MAX_PACKET_SIZE 4096
#endif

#if AOO_PACKET_SIZE > AOO_MAX_PACKET_SIZE
#error "AOO_PACKET_SIZE must not exceed AOO_MAX_PACKET_SIZE"
#endif

/** \brief max. size of a block's data (encoded audio and stream messages)
 * that a sink accepts; the block is assembled on the audio thread's stack.
 * Jamsa: 8 KB (was 64 KB). One Opus packet is at most 1275 bytes, and the
 * glue takes no block longer than 10 ms: a peer's 64 KB block only took 64 KB
 * of a host's audio thread stack (some give a plug-in 64..128 KB). */
#ifndef AOO_MAX_BLOCK_DATA_SIZE
# define AOO_MAX_BLOCK_DATA_SIZE 8192
#endif

/** \brief max. number of channels of a stream that a sink accepts */
#ifndef AOO_MAX_STREAM_CHANNELS
# define AOO_MAX_STREAM_CHANNELS 8
#endif

/** \brief max. bytes of stream messages a sink keeps scheduled per source
 * (they are allocated on the audio thread: keep this well below the RT memory
 * pool's large_alloc_limit, 64 KB, past which the pool uses the heap) */
#ifndef AOO_MAX_PENDING_STREAM_MESSAGES_SIZE
# define AOO_MAX_PENDING_STREAM_MESSAGES_SIZE 16384
#endif

/** \brief default size of the RT memory pool */
#ifndef AOO_MEM_POOL_SIZE
 #define AOO_MEM_POOL_SIZE (1 << 20) /* 1 MB */
#endif

/** \brief clip audio output between -1 and 1 */
#ifndef AOO_CLIP_OUTPUT
# define AOO_CLIP_OUTPUT 0
#endif

/** \brief use built-in Opus codec */
#ifndef AOO_USE_OPUS
#define AOO_USE_OPUS 1
#endif

/*--------------- compile time debug options ----------------*/

/** \brief debug memory usage */
#ifndef AOO_DEBUG_MEMORY
# define AOO_DEBUG_MEMORY 0
#endif

/** \brief debug the state of the DLL filter */
#ifndef AOO_DEBUG_DLL
# define AOO_DEBUG_DLL 0
#endif

/** \brief debug incoming/outgoing data */
#ifndef AOO_DEBUG_DATA
# define AOO_DEBUG_DATA 0
#endif

/** \brief debug data resending */
#ifndef AOO_DEBUG_RESEND
# define AOO_DEBUG_RESEND 0
#endif

/** \brief debug the state of the dynamic resampler */
#ifndef AOO_DEBUG_RESAMPLER
# define AOO_DEBUG_RESAMPLER 0
#endif

/** \brief debug the state of the jitter buffer */
#ifndef AOO_DEBUG_JITTER_BUFFER
# define AOO_DEBUG_JITTER_BUFFER 0
#endif

/** \brief debug stream message scheduling */
#ifndef AOO_DEBUG_STREAM_MESSAGE
# define AOO_DEBUG_STREAM_MESSAGE 0
#endif
