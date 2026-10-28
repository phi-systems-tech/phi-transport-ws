// Which browser may open a WebSocket to this core, and which may not.
//
// The same-origin policy does not cover WebSocket handshakes: a page on any
// site can open one to any host, and the browser attaches whatever that host
// has stored for it. So the server has to decide for itself, and the Origin
// header is what it decides on.
//
// Header-only and free of everything else in the transport, because this is
// the one rule here worth reading on its own and worth testing on its own.
#pragma once

#include <phi/runtime/str.h>

#include <cctype>
#include <string>
#include <string_view>
#include <vector>

namespace phicore::transport::ws::originpolicy {

inline std::string lowered(std::string_view text) { return phi::str::toLower(text); }
inline std::string trimmed(std::string_view text) { return phi::str::trimmed(text); }

/**
 * The `host[:port]` an origin names, lowercased; empty for anything that is not
 * an http(s) origin - which includes the literal `null` a sandboxed frame or a
 * `file://` page sends, and which must never match anything.
 *
 * The port is kept, because it is part of what makes an origin that origin. A
 * browser leaves out the default port for the scheme it used, and leaves it out
 * of the Host header too, so the two still line up.
 */
inline std::string authority(const std::string &origin)
{
    const std::string low = lowered(trimmed(origin));
    std::string rest;
    if (low.rfind("http://", 0) == 0)
        rest = low.substr(7);
    else if (low.rfind("https://", 0) == 0)
        rest = low.substr(8);
    else
        return {};
    return rest.substr(0, rest.find('/'));
}

/** The host part alone, brackets stripped for IPv6, port dropped. */
inline std::string hostOf(const std::string &authorityText)
{
    if (authorityText.empty())
        return {};
    if (authorityText.front() == '[') {
        const std::size_t closing = authorityText.find(']');
        return closing == std::string::npos ? std::string() : authorityText.substr(1, closing - 1);
    }
    return authorityText.substr(0, authorityText.find(':'));
}

/**
 * A page this very box served, asking this very box.
 *
 * `host` is the Host header of the handshake: the name the browser was told to
 * ask for. If the origin names the same one, the page came from here, whichever
 * of the box's names was used to reach it - its address,
 * `phi-magic-cfd324.local`, or anything else that resolves. A page from
 * somewhere else carries that somewhere else in its origin and can never
 * match, which is the whole point.
 *
 * The scheme is deliberately not compared. Whether the page arrived over TLS is
 * a question for the browser, which already refuses to open a plain socket from
 * a secure page, and the core behind a terminating proxy cannot see it anyway.
 */
inline bool isSameHost(const std::string &origin, const std::string &host)
{
    const std::string fromOrigin = authority(origin);
    const std::string asked = lowered(trimmed(host));
    return !fromOrigin.empty() && !asked.empty() && fromOrigin == asked;
}

/**
 * Four decimal octets, the first of them 127.
 *
 * Spelled out rather than matched as a `127.` prefix, which is what this used
 * to be: `127.example.com` starts with those four characters and is a name
 * anybody can register, not an address on this machine.
 */
inline bool isIpv4Loopback(const std::string &host)
{
    std::size_t pos = 0;
    int first = -1;
    for (int octet = 0; octet < 4; ++octet) {
        int value = 0;
        int digits = 0;
        while (pos < host.size() && std::isdigit(static_cast<unsigned char>(host[pos]))) {
            value = value * 10 + (host[pos] - '0');
            ++pos;
            if (++digits > 3 || value > 255)
                return false;
        }
        if (digits == 0)
            return false;
        if (octet == 0)
            first = value;
        if (octet < 3) {
            if (pos >= host.size() || host[pos] != '.')
                return false;
            ++pos;
        }
    }
    return pos == host.size() && first == 127;
}

/**
 * The machine talking to itself: a packaged UI or a dev server on the box.
 * Kept as its own rule because such a page reaches core as `localhost` while
 * the Host header may say something else entirely.
 */
inline bool isLoopback(const std::string &origin)
{
    const std::string host = hostOf(authority(origin));
    if (host.empty())
        return false;
    if (host == "localhost" || host == "::1")
        return true;
    return isIpv4Loopback(host); // 127.0.0.0/8
}

/** True when `host` is an IPv4 or IPv6 address literal (IPv6 without brackets). */
inline bool isAddressLiteral(const std::string &host)
{
    if (host.empty())
        return false;
    if (host.find(':') != std::string::npos) {
        // IPv6: hex digits, colons, and an embedded IPv4 tail or a zone.
        for (const char c : host) {
            if (!(std::isxdigit(static_cast<unsigned char>(c)) || c == ':' || c == '.' || c == '%'))
                return false;
        }
        return true;
    }
    std::size_t pos = 0;
    for (int octet = 0; octet < 4; ++octet) {
        int value = 0;
        int digits = 0;
        while (pos < host.size() && std::isdigit(static_cast<unsigned char>(host[pos]))) {
            value = value * 10 + (host[pos] - '0');
            ++pos;
            if (++digits > 3 || value > 255)
                return false;
        }
        if (digits == 0)
            return false;
        if (octet < 3) {
            if (pos >= host.size() || host[pos] != '.')
                return false;
            ++pos;
        }
    }
    return pos == host.size();
}

/**
 * A Host header that no stranger's DNS can point at this box.
 *
 * The same-host rule above has one blind spot, and it is DNS rebinding: a page
 * on `evil.example` whose name the attacker then re-points at the box's
 * address. The browser still believes it is talking to `evil.example`, so the
 * Origin and the Host header both say that name, they match, and the page is
 * let in with the house behind it. The Origin cannot tell the two apart; the
 * name can. What a stranger's DNS cannot answer for is an address literal,
 * `localhost`, a name with no dot (resolved by the LAN's own resolver through
 * its search domain), and the names reserved for local networks - `.local`
 * over mDNS above all, which is how the box is found. Everything else is a
 * public name and has to be listed: `allowed` holds the names the household
 * configured, `allowedHosts` and the hosts of its `allowedOrigins`.
 *
 * `hostHeader` is the header verbatim (port and IPv6 brackets included).
 */
inline bool isRebindSafeHost(const std::string &hostHeader, const std::vector<std::string> &allowed)
{
    const std::string host = hostOf(lowered(trimmed(hostHeader)));
    if (host.empty())
        return false;
    if (isAddressLiteral(host) || host == "localhost")
        return true;
    std::string name = host;
    if (name.back() == '.')
        name.pop_back(); // the absolute form names the same thing
    if (name.find('.') == std::string::npos)
        return true;
    static constexpr std::string_view kLocalSuffixes[] = {
        ".local", ".localhost", ".lan", ".home", ".home.arpa", ".internal", ".localdomain",
    };
    for (const std::string_view suffix : kLocalSuffixes) {
        if (name.size() > suffix.size() && name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0)
            return true;
    }
    for (const std::string &entry : allowed) {
        if (lowered(trimmed(entry)) == name)
            return true;
    }
    return false;
}

} // namespace phicore::transport::ws::originpolicy
