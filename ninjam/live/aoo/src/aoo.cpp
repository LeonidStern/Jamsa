/* Copyright (c) 2010-Now Christof Ressi, Winfried Ritsch and others.
 * For information on usage and redistribution, and for a DISCLAIMER OF ALL
 * WARRANTIES, see the file, "LICENSE.txt," in this distribution.  */

// Trimmed for Jamsa Live: AOO_NET=0, Opus only, no codec registry,
// no C sockaddr/NTP/strerror helpers.

#include "aoo.h"
#include "aoo_codec.h"
#include "codec/aoo_opus.h"

#include "binmsg.hpp"
#include "detail.hpp"
#include "rt_memory_pool.hpp"

#include "common/log.hpp"
#include "common/sync.hpp"
#include "common/time.hpp"
#include "common/utils.hpp"

#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <random>

//--------------------- helper functions -----------------//

namespace aoo {

int32_t get_random_id(){
    // std::minstd_rand is fast, uses very little memory
    // and is good enough for our purposes.
    static std::random_device rd;
    thread_local std::minstd_rand eng(rd());
    std::uniform_int_distribution<int32_t> dist;
    return dist(eng);
}

AooError check_version(const char *version) {
    // NB: no sscanf("%d"), whose behavior is undefined for numbers out of
    // range; the string comes from the network.
    char *end;
    auto major = strtol(version, &end, 10);
    if (end == version || *end != '.') {
        return kAooErrorBadFormat;
    }
    auto minor_start = end + 1;
    strtol(minor_start, &end, 10);
    if (end == minor_start) {
        return kAooErrorBadFormat;
    }
    if (major == kAooVersionMajor){
        return kAooOk;
    } else {
        return kAooErrorVersionNotSupported;
    }
}

} // namespace aoo

//----------------------- logging --------------------------//

namespace aoo {
namespace {

void AOO_CALL default_logfunc(AooLogLevel level, const char *message) {
    const char *label = nullptr;
    switch (level) {
    case kAooLogLevelError: label = "error"; break;
    case kAooLogLevelWarning: label = "warning"; break;
    case kAooLogLevelInfo: label = "info"; break;
    case kAooLogLevelDebug: label = "debug"; break;
    case kAooLogLevelVerbose: label = "verbose"; break;
    default: break;
    }
    const auto size = aoo::Log::buffer_size + 12;
    char buffer[size];
    int count;
    if (label) {
        count = snprintf(buffer, size, "[aoo][%s] %s\n", label, message);
    } else {
        count = snprintf(buffer, size, "[aoo] %s\n", message);
    }
    fwrite(buffer, count, 1, stderr);
    fflush(stderr);
}

std::atomic<AooLogFunc> g_logfunc{default_logfunc};

} // namespace
} // namespace aoo

AOO_API AooError AOO_CALL aoo_logMessage(AooLogLevel level, const AooChar *msg) {
    if (auto fn = aoo::g_logfunc.load(std::memory_order_acquire)) {
        fn(level, msg);
    }
    return kAooOk;
}

//----------------------- OSC --------------------------//

AOO_API AooError AOO_CALL aoo_parsePattern(
        const AooByte *msg, AooInt32 size,
        AooMsgType *type, AooId *id, AooInt32 *offset) {
    int32_t count = 0;
    if (aoo::binmsg_check(msg, size)) {
        *type = aoo::binmsg_type(msg, size);
        *id = aoo::binmsg_to(msg, size);
        auto n = aoo::binmsg_headersize(msg, size);
        if (n > 0) {
            if (offset) {
                *offset = n;
            }
            return kAooOk;
        } else {
            return kAooErrorBadArgument;
        }
    } else if (size >= kAooMsgDomainLen
        && !memcmp(msg, kAooMsgDomain, kAooMsgDomainLen)) {
        count += kAooMsgDomainLen;
        if (size >= (count + kAooMsgSourceLen)
            && !memcmp(msg + count, kAooMsgSource, kAooMsgSourceLen)) {
            *type = kAooMsgTypeSource;
            count += kAooMsgSourceLen;
        } else if (size >= (count + kAooMsgSinkLen)
            && !memcmp(msg + count, kAooMsgSink, kAooMsgSinkLen)) {
            *type = kAooMsgTypeSink;
            count += kAooMsgSinkLen;
        } else {
            return kAooErrorBadArgument; // net message types not supported
        }

        // /aoo/source or /aoo/sink
        // NB: the message comes from the network and is not NUL-terminated,
        // so parse "/<id>" within 'size' (sscanf could read past the end, and
        // overflowing "%d" is undefined).
        if (id) {
            if (count >= size || msg[count] != '/') {
                LOG_DEBUG("aoo_parse_pattern: bad ID");
                return kAooErrorBadArgument;
            }
            int32_t i = count + 1;
            int64_t value = 0;
            while (i < size && msg[i] >= '0' && msg[i] <= '9') {
                value = value * 10 + (msg[i] - '0');
                if (value > INT32_MAX) {
                    LOG_DEBUG("aoo_parse_pattern: bad ID");
                    return kAooErrorBadArgument;
                }
                i++;
            }
            if (i == count + 1) {
                LOG_DEBUG("aoo_parse_pattern: bad ID");
                return kAooErrorBadArgument;
            }
            *id = (AooId)value;
            count = i;
        } else {
            return kAooErrorBadArgument;
        }

        if (offset) {
            *offset = count;
        }
        return kAooOk;
    } else {
        return kAooErrorBadArgument; // not an AOO message
    }
}

//---------------------- time / version -------------------------//

AOO_API AooNtpTime AOO_CALL aoo_getCurrentNtpTime(void) {
    return aoo::time_tag::now();
}

#define STR_HELPER(x) #x
#define STR(x) STR_HELPER(x)

AOO_API const AooChar * AOO_CALL aoo_getVersionString(void) {
    return STR(kAooVersionMajor) "." STR(kAooVersionMinor)
    #if kAooVersionPatch > 0
        "." STR(kAooVersionPatch)
    #endif
    #if kAooVersionTest > 0
       "-test" STR(kAooVersionTest)
    #endif
        ;
}

//---------------------- AooData ----------------------//

namespace {

const std::array g_data_type_names {
    "raw", // same as "binary"!
    "text",
    "osc",
    "midi",
    "fudi",
    "json",
    "xml",
    "float32",
    "float64",
    "int16",
    "int32",
    "int64"
};

static_assert(g_data_type_names.size() == kAooDataInt64 + 1,
              "missing data type");

} // namespace

AOO_API const AooChar * AOO_CALL aoo_dataTypeToString(AooDataType type) {
    if (type < 0) {
        return "unspecified";
    } else if (type < (AooDataType)g_data_type_names.size()) {
        return g_data_type_names[type];
    } else if (type >= kAooDataUser) {
        return "user";
    } else {
        return "";
    }
}

//---------------------- RT memory --------------------------//

namespace aoo {

namespace {
aoo::rt_memory_pool<true, aoo::allocator<char>> g_rt_memory_pool;
}

void * rt_allocate(size_t size) {
    auto ptr = g_rt_memory_pool.allocate(size);
    if (!ptr && (size > 0)) {
        throw std::bad_alloc{};
    }
    return ptr;
}

void rt_deallocate(void *ptr, size_t size) {
    g_rt_memory_pool.deallocate(ptr, size);
}

sync::mutex g_rt_memory_pool_lock;
size_t g_rt_memory_pool_refcount = 0;

void rt_memory_pool_ref() {
    sync::scoped_lock<sync::mutex> l(g_rt_memory_pool_lock);
    g_rt_memory_pool_refcount++;
}

void rt_memory_pool_unref() {
    sync::scoped_lock<sync::mutex> l(g_rt_memory_pool_lock);
    if (--g_rt_memory_pool_refcount == 0) {
        LOG_DEBUG("total RT memory usage: " << g_rt_memory_pool.memory_usage()
                  << " / " << g_rt_memory_pool.capacity() << " bytes");
        g_rt_memory_pool.reset();
    }
}

} // namespace aoo

//------------------------ codec ---------------------------//

const AooCodecInterface * aoo_opusInterface(); // codec/opus.cpp

namespace aoo {

// hard-wired: Opus is the only codec
const AooCodecInterface * find_codec(const char * name){
    if (name && !strcmp(name, kAooCodecOpus)) {
        return aoo_opusInterface();
    }
    return nullptr;
}

} // namespace aoo

//--------------------------- (de)initialize -----------------------------------//

#define CHECK_SETTING(ptr, field) \
    (ptr && AOO_CHECK_FIELD(ptr, AooSettings, field))

namespace {
std::once_flag g_init_flag;
}

AOO_API AooError AOO_CALL aoo_initialize(const AooSettings *settings) {
    // thread-safe and idempotent; only the first call's settings take effect.
    std::call_once(g_init_flag, [settings]() {
        if (CHECK_SETTING(settings, logFunc) && settings->logFunc) {
            aoo::g_logfunc.store(settings->logFunc, std::memory_order_release);
        }
        if (CHECK_SETTING(settings, allocFunc) && settings->allocFunc) {
            LOG_WARNING("aoo_initialize: custom allocator not supported");
        }
        if (CHECK_SETTING(settings, memPoolSize) && settings->memPoolSize > 0) {
            aoo::g_rt_memory_pool.resize(settings->memPoolSize);
        } else {
            aoo::g_rt_memory_pool.resize(AOO_MEM_POOL_SIZE);
        }
    });
    return kAooOk;
}

AOO_API void AOO_CALL aoo_terminate(void) {
    // nothing to unload: Opus is statically linked and hard-wired
}
