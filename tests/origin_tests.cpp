// The origin gate, which is the only thing standing between a page somebody
// has open in another tab and the switches in their house.
#include "originpolicy.h"

#include <cstdio>
#include <string>

using namespace phicore::transport::ws::originpolicy;

namespace {

int g_failures = 0;

void check(bool ok, const char *expression, int line)
{
    if (ok)
        return;
    ++g_failures;
    std::fprintf(stderr, "FAIL origin_tests.cpp:%d: %s\n", line, expression);
}

#define CHECK(expr) check((expr), #expr, __LINE__)

void testThePageTheBoxServedIsLetIn()
{
    // However the browser was pointed at the box, the page it got back is the
    // box's own. This is the case that used to need every name in a config
    // file, and the one that left the UI unusable from a laptop.
    CHECK(isSameHost("http://phi-magic-cfd324.local", "phi-magic-cfd324.local"));
    CHECK(isSameHost("http://192.168.1.182", "192.168.1.182"));
    CHECK(isSameHost("https://phi-magic-cfd324.local", "phi-magic-cfd324.local"));
    // A port, when it is not the scheme's default, is part of the name and a
    // browser puts it in both headers.
    CHECK(isSameHost("http://box:8080", "box:8080"));
    // Case is not part of a host name.
    CHECK(isSameHost("HTTP://Phi-Magic-CFD324.Local", "phi-magic-cfd324.local"));
    CHECK(isSameHost("http://box", " box "));
    // IPv6, brackets and all.
    CHECK(isSameHost("http://[fd00::1]", "[fd00::1]"));
    CHECK(isSameHost("http://[fd00::1]:8080", "[fd00::1]:8080"));
}

void testEverybodyElseIsRefused()
{
    // The attack this exists for: a page open in another tab, opening a socket
    // to the box, with whatever the browser has stored for the box attached.
    CHECK(!isSameHost("http://evil.example.com", "phi-magic-cfd324.local"));
    CHECK(!isSameHost("https://evil.example.com", "192.168.1.182"));
    // A name that merely starts or ends the same is a different name.
    CHECK(!isSameHost("http://phi-magic-cfd324.local.evil.com", "phi-magic-cfd324.local"));
    CHECK(!isSameHost("http://notphi-magic-cfd324.local", "phi-magic-cfd324.local"));
    // Same host, different port: a different origin, and a different server.
    CHECK(!isSameHost("http://box:8080", "box"));
    CHECK(!isSameHost("http://box", "box:8080"));
    // What a sandboxed frame or a file:// page sends. It must match nothing,
    // including a Host header that somehow says the same word.
    CHECK(!isSameHost("null", "null"));
    CHECK(!isSameHost("null", "phi-magic-cfd324.local"));
    // Schemes that are not the web's.
    CHECK(!isSameHost("ws://box", "box"));
    CHECK(!isSameHost("file://", "box"));
    // Nothing on either side is not a match either.
    CHECK(!isSameHost("", "phi-magic-cfd324.local"));
    CHECK(!isSameHost("http://box", ""));
    CHECK(!isSameHost("", ""));
}

void testTheMachineTalkingToItself()
{
    // A dev server or the packaged UI on the box reaches core as localhost
    // while the Host header may say anything, so this stays its own rule.
    CHECK(isLoopback("http://localhost"));
    CHECK(isLoopback("http://localhost:3000"));
    CHECK(isLoopback("http://127.0.0.1"));
    CHECK(isLoopback("http://127.5.5.5:8080"));
    CHECK(isLoopback("http://[::1]:5040"));
    CHECK(!isLoopback("http://phi-magic-cfd324.local"));
    // Not loopback: names that merely begin with the digits, and things that
    // are not four octets. `127.example.com` is registrable.
    CHECK(!isLoopback("http://127.example.com"));
    CHECK(!isLoopback("http://127.0.0"));
    CHECK(!isLoopback("http://127.0.0.1.2"));
    CHECK(!isLoopback("http://1270.0.0.1"));
    CHECK(!isLoopback("http://127.0.0.256"));
    CHECK(!isLoopback("http://128.0.0.1"));
    CHECK(isLoopback("http://127.0.0.1:5040"));
    CHECK(!isLoopback("null"));
    CHECK(!isLoopback(""));
}

void testWhatAnOriginNames()
{
    CHECK(authority("http://box:8080/anything") == "box:8080");
    CHECK(authority("https://BOX") == "box");
    CHECK(authority("http://box/") == "box");
    CHECK(authority("null").empty());
    CHECK(authority("").empty());
    CHECK(hostOf("box:8080") == "box");
    CHECK(hostOf("[fd00::1]:8080") == "fd00::1");
    CHECK(hostOf("box") == "box");
    CHECK(hostOf("").empty());
}

} // namespace

void testRebindingIsRefused()
{
    // DNS rebinding: a public name re-pointed at the box. Origin and Host agree,
    // so only the name itself gives it away.
    CHECK(isSameHost("http://evil.example.com", "evil.example.com"));
    CHECK(!isRebindSafeHost("evil.example.com", {}));
    CHECK(!isRebindSafeHost("evil.example.com:5040", {}));
    CHECK(!isRebindSafeHost("local.evil.com", {}));
    CHECK(!isRebindSafeHost("127.example.com", {}));
    CHECK(!isRebindSafeHost("", {}));
    // Unless the household listed it - a box behind its own domain.
    CHECK(isRebindSafeHost("home.example.com", {"home.example.com"}));
    CHECK(isRebindSafeHost("Home.Example.com:443", {" home.example.com "}));
    CHECK(!isRebindSafeHost("other.example.com", {"home.example.com"}));
}

void testLocalNamesAreLetIn()
{
    CHECK(isRebindSafeHost("192.168.18.2", {}));
    CHECK(isRebindSafeHost("192.168.18.2:5040", {}));
    CHECK(isRebindSafeHost("[fd00::1]:8080", {}));
    CHECK(isRebindSafeHost("[::1]", {}));
    CHECK(isRebindSafeHost("localhost:5040", {}));
    CHECK(isRebindSafeHost("phi-magic-cfd324.local", {}));
    CHECK(isRebindSafeHost("PI5.LOCAL.", {}));
    CHECK(isRebindSafeHost("taifun", {}));
    CHECK(isRebindSafeHost("box.home.arpa", {}));
    CHECK(isRebindSafeHost("box.lan", {}));
    CHECK(!isRebindSafeHost("1.2.3", {})); // a dotted name, not an address: public
    CHECK(!isAddressLiteral("1.2.3"));
    CHECK(!isAddressLiteral("256.1.1.1"));
    CHECK(isAddressLiteral("10.0.0.1"));
}

int main()
{
    testThePageTheBoxServedIsLetIn();
    testEverybodyElseIsRefused();
    testTheMachineTalkingToItself();
    testWhatAnOriginNames();
    testRebindingIsRefused();
    testLocalNamesAreLetIn();

    if (g_failures > 0) {
        std::fprintf(stderr, "origin_tests: %d failure(s)\n", g_failures);
        return 1;
    }
    std::fprintf(stderr, "origin_tests: all passed\n");
    return 0;
}
