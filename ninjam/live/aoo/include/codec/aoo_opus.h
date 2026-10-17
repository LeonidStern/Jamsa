/* Copyright (c) 2021 Christof Ressi
 * For information on usage and redistribution, and for a DISCLAIMER OF ALL
 * WARRANTIES, see the file, "LICENSE.txt," in this distribution.  */

/** \file
 * \brief Opus codec settings
 */

#pragma once

#include "aoo_config.h"
#include "aoo_defines.h"
#include "aoo_types.h"

#include "opus_multistream.h"

#include <string.h>

AOO_PACK_BEGIN

/*---------------------------------------------------*/

/** \brief name of Opus codec */
#define kAooCodecOpus "opus"

/** \brief Opus codec format */
typedef struct AooFormatOpus
{
    /** format header */
    AooFormat header;
    /** Opus application type.
     * Possible values:
     * `OPUS_APPLICATION_AUDIO`
     * `OPUS_APPLICATION_RESTRICTED_LOWDELAY`
     * `OPUS_APPLICATION_VOIP`
     */
    opus_int32 applicationType;
    /** Jamsa: the encoder's bitrate in bits per second (`OPUS_AUTO` = Opus'
     * default). Only the encoder reads it; it is not sent to the sink. */
    opus_int32 bitrate;
} AooFormatOpus;

/*--------------------------------------------------*/

/** \brief initialize AooFormatOpus struct */
AOO_INLINE void AooFormatOpus_init(
        AooFormatOpus *fmt,
        AooInt32 numChannels, AooInt32 sampleRate,
        AooInt32 blockSize, opus_int32 applicationType)
{
    AOO_STRUCT_INIT(&fmt->header, AooFormatOpus, bitrate);
    fmt->header.numChannels = numChannels;
    fmt->header.sampleRate = sampleRate;
    fmt->header.blockSize = blockSize;
    fmt->applicationType = applicationType;
    fmt->bitrate = OPUS_AUTO;
    strcpy(fmt->header.codecName, kAooCodecOpus);
}

/*--------------------------------------------------------------------*/

AOO_PACK_END
