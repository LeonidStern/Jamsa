#pragma once

#include "aoo_config.h"
#include "aoo_defines.h"
#include "aoo_types.h"

#include <cassert>
#include <cstring>
#include <ostream>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#if AOO_USE_IPV6
#include <ws2ipdef.h>
#endif
typedef int socklen_t;
struct sockaddr;
#else
#include <sys/socket.h>
#include <netinet/in.h>
#endif // _WIN32

namespace aoo {

//-------------- ip_address --------------//

using port_type = uint16_t;

class ip_address {
public:
    enum ip_type {
        Unspec,
        IPv4,
        IPv6
    };

    static const socklen_t max_length = 32;

    ip_address();

    // NB: for convenience 'sa' may be NULL!
    ip_address(const struct sockaddr *sa, socklen_t len) {
        if (sa) {
            assert(len > 0 && len <= max_length);
            memcpy(data_, sa, len);
#if 0
            // keep clang-analyzer happy...
            addr_.sa_family = sa->sa_family;
#endif
            length_ = len;
        } else {
            assert(len == 0);
            clear();
        }
    }

    ip_address(const AooSockAddr &addr)
        : ip_address((const struct sockaddr *)addr.data, addr.size) {}

    ip_address(const AooByte *bytes, AooSize size, port_type port, ip_type type);

    ip_address(const ip_address& other){
        if (other.length_ > 0) {
            memcpy(data_, &other.data_, other.length_);
            length_ = other.length_;
        } else {
            clear();
        }
    }

    ip_address& operator=(const ip_address& other){
        if (other.length_ > 0) {
            memcpy(data_, other.data_, other.length_);
            length_ = other.length_;
        } else {
            clear();
        }
        return *this;
    }

    void clear();

    void reserve();

    void resize(socklen_t size) {
        length_ = size;
    }

    bool operator==(const ip_address& other) const;

    bool operator !=(const ip_address& other) const {
        return !(*this == other);
    }

    size_t hash() const;

    const char* name() const;

    const char* name_unmapped() const;

    port_type port() const;

    const AooByte* address_bytes() const;

    size_t address_size() const;

    bool valid() const;

    ip_type type() const;

    bool is_ipv4_mapped() const;

    ip_address ipv4_mapped() const;

    ip_address unmapped() const;

    void unmap();

    const struct sockaddr *address() const {
        return (const struct sockaddr *)data_;
    }

    struct sockaddr *address_ptr() {
        return (struct sockaddr *)data_;
    }

    socklen_t length() const {
        return length_;
    }

    socklen_t *length_ptr() {
        return &length_;
    }

    friend std::ostream& operator<<(std::ostream& os, const ip_address& addr);

private:
    static const char *get_name(const struct sockaddr *addr);
    // large enough to hold both sockaddr_in
    // and sockaddr_in6 (max. 32 bytes)
    union {
        sockaddr_in addr_in_;
#if AOO_USE_IPV6
        sockaddr_in6 addr_in6_;
#endif
        // NB: on some systems (macOS, BSD, ESP32) 'sockaddr' starts with a
        // 'sa_len` field and 'sa_family' is only uint8_t! Therefore we must
        // always access the family through 'sockaddr'.
        sockaddr addr_;
        uint64_t align_;
        AooByte data_[max_length];
    };
    socklen_t length_;

    void check();
};

} // aoo
