/* Copyright (c) 2021 Christof Ressi
 * For information on usage and redistribution, and for a DISCLAIMER OF ALL
 * WARRANTIES, see the file, "LICENSE.txt," in this distribution.  */

/** \file
 * \brief main library API
 */

#pragma once

#include "aoo_config.h"
#include "aoo_defines.h"
#include "aoo_types.h"

AOO_PACK_BEGIN

/*------------------------------------------------------*/

/** \brief settings for aoo_initialize()
 *
 * \attention (C only) always initialize with AOO_SETTINGS_INIT()!
 *
 * \details Example:
 * ~~~~
 * #ifdef __cplusplus
 *     AooSettings settings;
 * #else
 *     AooSettings settings = AOO_SETTINGS_INIT();
 * #endif
 *     settings.logFunc = myLogFunc; // set custom log function
 *     aoo_initialize(&settings);
 * ~~~~
 */
typedef struct AooSettings
{
#ifdef __cplusplus
    /** default constructor */
    AooSettings()
        : structSize(AOO_STRUCT_SIZE(AooSettings, memPoolSize)),
          allocFunc(NULL), logFunc(NULL), memPoolSize(0) {}
#endif

    /** struct size */
    AooSize structSize;
    /** custom allocator function, or `NULL` */
    AooAllocFunc allocFunc;
    /** custom log function, or `NULL` */
    AooLogFunc logFunc;
    /** size of RT memory pool */
    AooSize memPoolSize;
} AooSettings;

/** \brief (C only) default initializer for AooSettings struct */
#define AOO_SETTINGS_INIT() {                   \
    AOO_STRUCT_SIZE(AooSettings, memPoolSize),  \
    NULL, NULL, AOO_MEM_POOL_SIZE               \
}

/**
 * \brief initialize AOO library settings
 *
 * \note Call before any other AOO function!
 * \param settings (optional) settings struct
 */
AOO_API AooError AOO_CALL aoo_initialize(const AooSettings *settings);

/**
 * \brief terminate AOO library
 *
 * \note Call before program exit.
 */
AOO_API void AOO_CALL aoo_terminate(void);

/**
 * \brief get the AOO version string
 *
 * Format: `<major>[.<minor>][.<patch>][-test<test>]`
 * \return the version as a C string
 */
AOO_API const AooChar * AOO_CALL aoo_getVersionString(void);

/**
 * \brief get the current NTP time
 *
 * \return NTP time stamp
 */
AOO_API AooNtpTime AOO_CALL aoo_getCurrentNtpTime(void);

/**
 * \brief parse an AOO message
 *
 * Tries to obtain the AOO message type and ID from the address pattern,
 * like in `/aoo/src/<id>/data`.
 *
 * \param msg the OSC message data
 * \param size the OSC message size
 * \param [out] type the AOO message type
 * \param [out] id the source/sink ID
 * \param [out] offset pointer to the start of the remaining address pattern
 * \return error code
 */
AOO_API AooError AOO_CALL aoo_parsePattern(
        const AooByte *msg, AooInt32 size,
        AooMsgType *type, AooId *id, AooInt32 *offset);

/**
 * \brief convert AooData type to string representation
 *
 * \param type the data type
 * \return a C string on success; if the data type is unknown or invalid,
 *         an empty string is returned.
 */
AOO_API const AooChar * AOO_CALL aoo_dataTypeToString(AooDataType type);

/*------------------------------------------------------*/

/**
 * \brief log a message
 */
AOO_API AooError AOO_CALL aoo_logMessage(AooLogLevel level, const AooChar *msg);

/**
 * \brief (Jamsa) set the log function, at any time
 *
 * Unlike AooSettings::logFunc, which only the first aoo_initialize() call
 * takes, this replaces the function whenever it is called. AOO logs from
 * the audio thread too (rarely: at the default level, Error, only what
 * would be a bug), so the function must not block: hand the message to
 * another thread. `NULL` silences AOO.
 */
AOO_API void AOO_CALL aoo_setLogFunction(AooLogFunc fn);

AOO_PACK_END
