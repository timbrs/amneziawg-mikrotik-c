/* Ceiling on the WireGuard interface MTU, per address family.
 *
 * The outer datagram the proxy puts on the wire is
 *   IP hdr + UDP(8) + S4 + WG hdr(16) + round_up(mtu,16) + Poly1305 tag(16)
 * and the IPv6 header is 40 bytes where IPv4 uses 20. A config that fits 1500
 * bytes over IPv4 therefore overshoots by exactly 20 over IPv6 — which is why
 * wg-quick itself drops from 1420 to 1400 when the endpoint is v6. */
#include <stdint.h>
#include "test.h"
#include "proxy.h"

/* Full-size outer datagram for a given MTU, mirroring the formula above. */
static int outer_len(int mtu, int s4, int ipv6) {
    int rounded = ((mtu + 15) / 16) * 16;
    return (ipv6 ? 40 : 20) + 8 + s4 + 16 + rounded + 16;
}

static void test_no_padding(void) {
    ASSERT_EQ(awg_max_wg_mtu(1500, 0, 0), 1440);
    /* 1420 is not a multiple of 16, so WireGuard would pad it back up to 1424
     * and overshoot — the attainable v6 ceiling is one step down. */
    ASSERT_EQ(awg_max_wg_mtu(1500, 0, 1), 1408);
}

static void test_ipv6_costs_20_bytes(void) {
    /* The whole point: same config, 20 bytes less room. */
    for (int s4 = 0; s4 <= 256; s4 += 4)
        ASSERT_EQ(awg_max_wg_mtu(1500, s4, 1), awg_max_wg_mtu(1500, s4 + 20, 0));
}

static void test_typical_v3_padding(void) {
    /* S4 = 12..16 is the usual AWG 3.0 range (12 is the ChaCha20 nonce floor) */
    ASSERT_EQ(awg_max_wg_mtu(1500, 12, 1), 1408);
    ASSERT_EQ(awg_max_wg_mtu(1500, 16, 1), 1392);
    ASSERT_EQ(awg_max_wg_mtu(1500, 12, 0), 1424);
    ASSERT_EQ(awg_max_wg_mtu(1500, 16, 0), 1424);
}

static void test_pppoe_path(void) {
    /* PPPoE takes 8 bytes: the stock 1420 with S4=16 no longer fits 1492, and
     * the ceiling the warning names must step down to the next 16. */
    ASSERT(outer_len(1420, 16, 0) > 1492);
    ASSERT_EQ(awg_max_wg_mtu(1492, 16, 0), 1408);
    ASSERT_EQ(awg_max_wg_mtu(1492, 0, 0), 1424);
    ASSERT_EQ(awg_max_wg_mtu(1492, 16, 1), 1392);
}

static void test_result_is_16_aligned(void) {
    /* WireGuard pads the plaintext to a 16-byte boundary, so a ceiling that is
     * not itself aligned would be rounded back up and overshoot. */
    for (int path = 1280; path <= 1500; path++)
        for (int s4 = 0; s4 <= 256; s4 += 3)
            ASSERT_EQ(awg_max_wg_mtu(path, s4, 1) % 16, 0);
}

static void test_ceiling_actually_fits(void) {
    /* Every path MTU the kernel can learn, from the IPv6 floor up: the named
     * MTU fits, and it is the largest multiple of 16 that does. */
    for (int path = 1280; path <= 1500; path++) {
        for (int ipv6 = 0; ipv6 <= 1; ipv6++) {
            for (int s4 = 0; s4 <= 256; s4++) {
                int mtu = awg_max_wg_mtu(path, s4, ipv6);
                if (mtu == 0) continue;
                ASSERT(outer_len(mtu, s4, ipv6) <= path);
                ASSERT(outer_len(mtu + 16, s4, ipv6) > path);
            }
        }
    }
}

static void test_absurd_padding_does_not_underflow(void) {
    ASSERT_EQ(awg_max_wg_mtu(1500, 5000, 1), 0);
    ASSERT_EQ(awg_max_wg_mtu(1500, 1440, 0), 0);
    ASSERT_EQ(awg_max_wg_mtu(0, 0, 0), 0);
}

int main(void) {
    fprintf(stderr, "=== mtu tests ===\n");
    RUN_TEST(no_padding);
    RUN_TEST(ipv6_costs_20_bytes);
    RUN_TEST(typical_v3_padding);
    RUN_TEST(pppoe_path);
    RUN_TEST(result_is_16_aligned);
    RUN_TEST(ceiling_actually_fits);
    RUN_TEST(absurd_padding_does_not_underflow);
    TEST_MAIN_END();
}
