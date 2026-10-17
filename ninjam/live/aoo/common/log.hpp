#pragma once

#include "aoo.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <sstream>
#include <string>
#include <string_view>
#include <type_traits>

#define DO_LOG(level, msg) do { aoo::Log(level) << msg; } while (false)

#define LOG_ALL(msg) DO_LOG(kAooLogLevelSilent, msg)

#if AOO_LOG_LEVEL >= kAooLogLevelError
# define LOG_ERROR(msg) DO_LOG(kAooLogLevelError, msg)
#else
# define LOG_ERROR(msg)
#endif

#if AOO_LOG_LEVEL >= kAooLogLevelWarning
# define LOG_WARNING(msg) DO_LOG(kAooLogLevelWarning, msg)
#else
# define LOG_WARNING(msg)
#endif

#if AOO_LOG_LEVEL >= kAooLogLevelInfo
# define LOG_INFO(msg) DO_LOG(kAooLogLevelInfo, msg)
#else
# define LOG_INFO(msg)
#endif

#if AOO_LOG_LEVEL >= kAooLogLevelDebug
# define LOG_DEBUG(msg) DO_LOG(kAooLogLevelDebug, msg)
#else
# define LOG_DEBUG(msg)
#endif

#if AOO_LOG_LEVEL >= kAooLogLevelVerbose
# define LOG_VERBOSE(msg) DO_LOG(kAooLogLevelVerbose, msg)
#else
# define LOG_VERBOSE(msg)
#endif

namespace aoo {

// Jamsa: formats into a fixed buffer on the stack without an std::ostream.
// Constructing one (an std::ios_base) allocates a locale on the heap with
// MSVC's standard library, and LOG_ERROR is reachable from process() on
// the audio thread. Strings, characters, numbers and pointers are written
// with snprintf(); any other type goes through its own operator<< into an
// std::ostringstream (heap), which only the messages above Error use.
class Log final {
public:
    static const int32_t buffer_size = 256;

    Log(AooLogLevel level = kAooLogLevelSilent)
        : level_(level) {}
    ~Log() {
        buffer_[pos_] = 0;
        aoo_logMessage(level_, buffer_);
    }
    Log(const Log&) = delete;
    Log& operator=(const Log&) = delete;

    Log& operator<<(const char *s) {
        if (!s) s = "(null)";
        write(s, strlen(s));
        return *this;
    }
    Log& operator<<(const std::string& s) {
        write(s.data(), s.size());
        return *this;
    }
    Log& operator<<(std::string_view s) {
        write(s.data(), s.size());
        return *this;
    }
    // (as std::ostream: characters, not numbers)
    Log& operator<<(char c) { write(&c, 1); return *this; }
    Log& operator<<(signed char c) { return *this << (char)c; }
    Log& operator<<(unsigned char c) { return *this << (char)c; }
    Log& operator<<(bool b) { return *this << (b ? '1' : '0'); }
    Log& operator<<(const void *p) {
        char buf[32];
        format(buf, snprintf(buf, sizeof(buf), "%p", p));
        return *this;
    }

    template <typename T>
    std::enable_if_t<std::is_integral_v<T> && std::is_signed_v<T>, Log&>
    operator<<(T v) {
        char buf[32];
        format(buf, snprintf(buf, sizeof(buf), "%lld", (long long)v));
        return *this;
    }
    template <typename T>
    std::enable_if_t<std::is_integral_v<T> && std::is_unsigned_v<T>, Log&>
    operator<<(T v) {
        char buf[32];
        format(buf, snprintf(buf, sizeof(buf), "%llu", (unsigned long long)v));
        return *this;
    }
    template <typename T>
    std::enable_if_t<std::is_floating_point_v<T>, Log&>
    operator<<(T v) {
        char buf[32];
        format(buf, snprintf(buf, sizeof(buf), "%g", (double)v));
        return *this;
    }
    // enums, endpoints, ...
    template <typename T>
    std::enable_if_t<!std::is_arithmetic_v<T> && !std::is_pointer_v<T>
                     && !std::is_array_v<T> && !std::is_convertible_v<const T&, std::string_view>, Log&>
    operator<<(const T& v) {
        std::ostringstream os;
        os << v;
        return *this << os.str();
    }
private:
    void write(const char *s, size_t n) {
        const size_t limit = buffer_size - 1;
        if ((size_t)pos_ < limit) {
            if (n > limit - pos_) {
                n = limit - pos_;
            }
            memcpy(buffer_ + pos_, s, n);
            pos_ += (int32_t)n;
        }
    }
    void format(const char *buf, int n) {
        if (n > 0) write(buf, std::min<size_t>(n, 31));
    }

    AooLogLevel level_;
    int32_t pos_ = 0;
    char buffer_[buffer_size];
};

}
