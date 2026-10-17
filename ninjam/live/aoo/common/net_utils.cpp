/* Copyright (c) 2010-Now Christof Ressi, Winfried Ritsch and others.
 * For information on usage and redistribution, and for a DISCLAIMER OF ALL
 * WARRANTIES, see the file, "LICENSE.txt," in this distribution.  */

#include "net_utils.hpp"

#ifdef _WIN32
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#endif

#include <cstdio>
#include <cstring>
#include <algorithm>

namespace aoo {

//------------------------ ip_address ------------------------//

ip_address::ip_address() {
    static_assert(sizeof(data_) == max_length,
                  "wrong max_length value");
#if AOO_USE_IPV6
    static_assert(sizeof(data_) >= sizeof(sockaddr_in6),
                  "ip_address can't hold IPv6 sockaddr");
#endif
    clear();
}

ip_address::ip_address(const AooByte *bytes, AooSize size,
                       port_type port, ip_type type) {
    switch (type) {
#if AOO_USE_IPV6
    case IPv6:
    {
        assert(size == 16);
#if 1
        memset(&addr_in6_, 0, sizeof(addr_in6_));
#endif
        // NB: we don't need to set 'sin6_len' (if present)
        addr_in6_.sin6_family = AF_INET6;
        addr_in6_.sin6_port = htons(port);
        memcpy(&addr_in6_.sin6_addr, bytes, size);

        length_ = sizeof(addr_in6_);

        break;
    }
#endif
    case IPv4:
    {
        assert(size == 4);
#if 1
        memset(&addr_in_, 0, sizeof(addr_in_));
#endif
        // NB: we don't need to set 'sin_len' (if present)
        addr_in_.sin_family = AF_INET;
        addr_in_.sin_port = htons(port);
        memcpy(&addr_in_.sin_addr, bytes, size);

        length_ = sizeof(addr_in_);

        break;
    }
    default:
        clear();
        break;
    }
#if 0
    check();
#endif
}

void ip_address::clear() {
#if 1
    memset(data_, 0, sizeof(data_));
#endif
    addr_.sa_family = AF_UNSPEC;
    length_ = 0;
}

void ip_address::reserve() {
    clear();
    length_ = max_length;
}

void ip_address::check() {
    auto f = addr_.sa_family;
#if AOO_USE_IPV6
    bool ok = (f == AF_INET6 || f == AF_INET || f == AF_UNSPEC);
#else
    bool ok = (f == AF_INET || f == AF_UNSPEC);
#endif
    if (!ok) {
        fprintf(stderr, "bad address family: %d\n", f);
        fflush(stderr);
    }
}


bool ip_address::operator==(const ip_address& other) const {
    if (addr_.sa_family == other.addr_.sa_family) {
        switch (addr_.sa_family){
        case AF_INET:
        {
            auto& a = addr_in_;
            auto& b = other.addr_in_;
            return (a.sin_addr.s_addr == b.sin_addr.s_addr)
                    && (a.sin_port == b.sin_port);
        }
    #if AOO_USE_IPV6
        case AF_INET6:
        {
            auto& a = addr_in6_;
            auto& b = other.addr_in6_;
            return !memcmp(a.sin6_addr.s6_addr, b.sin6_addr.s6_addr,
                           sizeof(struct in6_addr))
                    && (a.sin6_port == b.sin6_port);
        }
    #endif
        default:
            break;
        }
    }
    return false;
}

size_t ip_address::hash() const {
    switch (addr_.sa_family) {
#if AOO_USE_IPV6
    case AF_INET6:
    {
        uint32_t w[4];
        memcpy(w, addr_in6_.sin6_addr.s6_addr, 16);
        size_t state = addr_in6_.sin6_port;
        state = (state << 1) ^ w[0];
        state = (state << 1) ^ w[1];
        state = (state << 1) ^ w[2];
        state = (state << 1) ^ w[3];
        return state;
    }
#endif
    case AF_INET:
    {
        size_t state = addr_in_.sin_port;
        return (state << 1) ^ addr_in_.sin_addr.s_addr;
    }
    default:
        return 0;
    }
}

std::ostream& operator<<(std::ostream& os, const ip_address& addr) {
    switch (addr.address()->sa_family) {
#if AOO_USE_IPV6
    case AF_INET6:
        os << "[" << addr.name() << "]:" << addr.port();
        break;
#endif
    case AF_INET:
        os << addr.name() << ":" << addr.port();
        break;
    case AF_UNSPEC:
        os << "[empty]";
        break;
    default:
        os << "[bad address]";
        break;
    }
    return os;
}

const char * ip_address::get_name(const sockaddr *addr){
#if AOO_USE_IPV6
    thread_local char buf[INET6_ADDRSTRLEN];
#else
    thread_local char buf[INET_ADDRSTRLEN];
#endif
    const void *na;
    auto family = addr->sa_family;
    switch (family){
#if AOO_USE_IPV6
    case AF_INET6:
        na = &reinterpret_cast<const sockaddr_in6 *>(addr)->sin6_addr;
        break;
#endif
    case AF_INET:
        na = &reinterpret_cast<const sockaddr_in *>(addr)->sin_addr;
        break;
    default:
        return "";
    }

    if (inet_ntop(family, na, buf, sizeof(buf)) != nullptr) {
        return buf;
    } else {
        return "";
    }
}

const char* ip_address::name() const {
    if (length_ > 0) {
        return get_name(&addr_);
    } else {
        return "";
    }
}

// for backwards compatibility with IPv4 only servers
const char* ip_address::name_unmapped() const {
    auto str = name();
    // strip leading "::ffff:" for mapped IPv4 addresses
    if (!strncmp(str, "::ffff:", 7) ||
        !strncmp(str, "::FFFF:", 7)) {
        return str + 7;
    } else {
        return str;
    }
}

port_type ip_address::port() const {
    switch (addr_.sa_family){
    case AF_INET:
        return ntohs(addr_in_.sin_port);
#if AOO_USE_IPV6
    case AF_INET6:
        return ntohs(addr_in6_.sin6_port);
#endif
    default:
        return 0;
    }
}

const AooByte* ip_address::address_bytes() const {
    switch (addr_.sa_family){
    case AF_INET:
        return (const AooByte *)&addr_in_.sin_addr;
#if AOO_USE_IPV6
    case AF_INET6:
        return (const AooByte *)&addr_in6_.sin6_addr;
#endif
    default:
        return nullptr;
    }
}

size_t ip_address::address_size() const {
    switch(addr_.sa_family){
    case AF_INET:
        return 4;
#if AOO_USE_IPV6
    case AF_INET6:
        return 16;
#endif
    default:
        return 0;
    }
}

bool ip_address::valid() const {
    return address()->sa_family != AF_UNSPEC;
}

ip_address::ip_type ip_address::type() const {
    switch(addr_.sa_family){
    case AF_INET:
        return IPv4;
#if AOO_USE_IPV6
    case AF_INET6:
        return IPv6;
#endif
    default:
        return Unspec;
    }
}

bool ip_address::is_ipv4_mapped() const {
#if AOO_USE_IPV6
    if (addr_.sa_family == AF_INET6) {
        auto w = (uint16_t *)addr_in6_.sin6_addr.s6_addr;
        return (w[0] == 0) && (w[1] == 0) && (w[2] == 0) && (w[3] == 0) &&
               (w[4] == 0) && (w[5] == 0xffff);
    }
#endif
    return false;
}

ip_address ip_address::ipv4_mapped() const {
#if AOO_USE_IPV6
    if (addr_.sa_family == AF_INET) {
        uint16_t w[8] = { 0, 0, 0, 0, 0, 0xffff };
        memcpy(&w[6], &addr_in_.sin_addr.s_addr, 4);
        return ip_address((const AooByte *)&w, 16, port(), IPv6);
    }
#endif
    return *this;
}

ip_address ip_address::unmapped() const {
#if AOO_USE_IPV6
    if (is_ipv4_mapped()) {
        auto w = (uint16_t *)addr_in6_.sin6_addr.s6_addr;
        return ip_address((const AooByte *)&w[6], 4, port(), IPv4);
    }
#endif
    return *this;
}

void ip_address::unmap() {
#if AOO_USE_IPV6
    if (is_ipv4_mapped()){
        auto w = (uint16_t *)addr_in6_.sin6_addr.s6_addr;
        *this = ip_address((const AooByte *)&w[6], 4, port(), IPv4);
    }
#endif
}

} // aoo
