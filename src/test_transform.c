#include <stdint.h>
#include <stdlib.h>
#include "test.h"
#include "transform.h"
#include "blake2s.h"
#include "fastrand.h"
#include "cps.h"

/* Shared test config: Jc=3, Jmin=30, Jmax=500, S1=S2=20, H1-H4 point values */
static awg_config_t make_test_config(void) {
    awg_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.jc = 3;
    cfg.jmin = 30;
    cfg.jmax = 500;
    cfg.s1 = 20;
    cfg.s2 = 20;
    cfg.s3 = 0;
    cfg.s4 = 0;
    cfg.h1 = (hrange_t){1234567890, 1234567890};
    cfg.h2 = (hrange_t){1234567891, 1234567891};
    cfg.h3 = (hrange_t){1234567892, 1234567892};
    cfg.h4 = (hrange_t){1234567893, 1234567893};
    config_compute(&cfg);
    return cfg;
}

static void fill_seq(uint8_t *buf, int n) {
    for (int i = 0; i < n; i++) buf[i] = (uint8_t)i;
}

static void write32_le(uint8_t *p, uint32_t v) {
    memcpy(p, &v, 4);
}

static uint32_t read32_le(const uint8_t *p) {
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}

/* 1. Outbound handshake init */
static void test_outbound_handshake_init(void) {
    awg_config_t cfg = make_test_config();
    uint8_t buf[256 + WG_INIT_SIZE];
    int dataoff = 256;
    memset(buf, 0, sizeof(buf));
    uint8_t *data = buf + dataoff;
    write32_le(data, WG_HANDSHAKE_INIT);
    fill_seq(data + 4, WG_INIT_SIZE - 4);

    int out_len, sendJunk;
    uint8_t *out = transform_outbound(buf, dataoff, WG_INIT_SIZE, &cfg, 12345, &out_len, &sendJunk);
    ASSERT_EQ(out_len, cfg.s1 + WG_INIT_SIZE);
    ASSERT_EQ(sendJunk, 1);
    /* Type at offset S1 should be H1 */
    uint32_t h = read32_le(out + cfg.s1);
    ASSERT(hrange_contains(&cfg.h1, h));
}

/* 2. Outbound handshake response */
static void test_outbound_handshake_response(void) {
    awg_config_t cfg = make_test_config();
    uint8_t buf[256 + WG_RESP_SIZE];
    int dataoff = 256;
    memset(buf, 0, sizeof(buf));
    uint8_t *data = buf + dataoff;
    write32_le(data, WG_HANDSHAKE_RESPONSE);
    fill_seq(data + 4, WG_RESP_SIZE - 4);

    int out_len, sendJunk;
    uint8_t *out = transform_outbound(buf, dataoff, WG_RESP_SIZE, &cfg, 12345, &out_len, &sendJunk);
    ASSERT_EQ(out_len, cfg.s2 + WG_RESP_SIZE);
    ASSERT_EQ(sendJunk, 0);
    uint32_t h = read32_le(out + cfg.s2);
    ASSERT(hrange_contains(&cfg.h2, h));
}

/* 3. Outbound cookie reply */
static void test_outbound_cookie_reply(void) {
    awg_config_t cfg = make_test_config();
    uint8_t buf[256 + WG_COOKIE_SIZE];
    int dataoff = 256;
    memset(buf, 0, sizeof(buf));
    uint8_t *data = buf + dataoff;
    write32_le(data, WG_COOKIE_REPLY);
    fill_seq(data + 4, WG_COOKIE_SIZE - 4);

    int out_len, sendJunk;
    uint8_t *out = transform_outbound(buf, dataoff, WG_COOKIE_SIZE, &cfg, 12345, &out_len, &sendJunk);
    ASSERT_EQ(out_len, WG_COOKIE_SIZE); /* S3=0, no padding */
    ASSERT_EQ(sendJunk, 0);
    uint32_t h = read32_le(out);
    ASSERT(hrange_contains(&cfg.h3, h));
}

/* 4. Outbound transport data */
static void test_outbound_transport_data(void) {
    awg_config_t cfg = make_test_config();
    uint8_t buf[256 + 100];
    int dataoff = 256;
    memset(buf, 0, sizeof(buf));
    uint8_t *data = buf + dataoff;
    write32_le(data, WG_TRANSPORT_DATA);
    fill_seq(data + 4, 96);

    int out_len, sendJunk;
    uint8_t *out = transform_outbound(buf, dataoff, 100, &cfg, 12345, &out_len, &sendJunk);
    ASSERT_EQ(out_len, 100); /* S4=0, no padding */
    ASSERT_EQ(sendJunk, 0);
    uint32_t h = read32_le(out);
    ASSERT(hrange_contains(&cfg.h4, h));
}

/* 5. Inbound handshake init */
static void test_inbound_handshake_init(void) {
    awg_config_t cfg = make_test_config();
    int total = cfg.s1 + WG_INIT_SIZE;
    uint8_t buf[256];
    memset(buf, 0xAA, sizeof(buf));
    /* S1 bytes padding + H1 type + payload */
    write32_le(buf + cfg.s1, cfg.h1.min);
    fill_seq(buf + cfg.s1 + 4, WG_INIT_SIZE - 4);

    int out_len;
    uint8_t *out = transform_inbound(buf, total, &cfg, &out_len);
    ASSERT(out != NULL);
    ASSERT_EQ(out_len, WG_INIT_SIZE);
    ASSERT_EQ(read32_le(out), WG_HANDSHAKE_INIT);
}

/* 6. Inbound handshake response */
static void test_inbound_handshake_response(void) {
    awg_config_t cfg = make_test_config();
    int total = cfg.s2 + WG_RESP_SIZE;
    uint8_t buf[256];
    memset(buf, 0xAA, sizeof(buf));
    write32_le(buf + cfg.s2, cfg.h2.min);
    fill_seq(buf + cfg.s2 + 4, WG_RESP_SIZE - 4);

    int out_len;
    uint8_t *out = transform_inbound(buf, total, &cfg, &out_len);
    ASSERT(out != NULL);
    ASSERT_EQ(out_len, WG_RESP_SIZE);
    ASSERT_EQ(read32_le(out), WG_HANDSHAKE_RESPONSE);
}

/* 7. Inbound cookie reply */
static void test_inbound_cookie_reply(void) {
    awg_config_t cfg = make_test_config();
    uint8_t buf[WG_COOKIE_SIZE];
    memset(buf, 0xAA, sizeof(buf));
    write32_le(buf, cfg.h3.min);

    int out_len;
    uint8_t *out = transform_inbound(buf, WG_COOKIE_SIZE, &cfg, &out_len);
    ASSERT(out != NULL);
    ASSERT_EQ(out_len, WG_COOKIE_SIZE);
    ASSERT_EQ(read32_le(out), WG_COOKIE_REPLY);
}

/* 8. Inbound transport data */
static void test_inbound_transport_data(void) {
    awg_config_t cfg = make_test_config();
    uint8_t buf[100];
    memset(buf, 0xAA, sizeof(buf));
    write32_le(buf, cfg.h4.min);

    int out_len;
    uint8_t *out = transform_inbound(buf, 100, &cfg, &out_len);
    ASSERT(out != NULL);
    ASSERT_EQ(out_len, 100);
    ASSERT_EQ(read32_le(out), WG_TRANSPORT_DATA);
}

/* 9. Roundtrip handshake init */
static void test_roundtrip_init(void) {
    awg_config_t cfg = make_test_config();
    uint8_t orig[WG_INIT_SIZE];
    write32_le(orig, WG_HANDSHAKE_INIT);
    fill_seq(orig + 4, WG_INIT_SIZE - 4);

    uint8_t buf[256 + WG_INIT_SIZE];
    int dataoff = 256;
    memset(buf, 0, sizeof(buf));
    memcpy(buf + dataoff, orig, WG_INIT_SIZE);

    int out_len, sendJunk;
    uint8_t *out = transform_outbound(buf, dataoff, WG_INIT_SIZE, &cfg, 99, &out_len, &sendJunk);

    int in_len;
    uint8_t *result = transform_inbound(out, out_len, &cfg, &in_len);
    ASSERT(result != NULL);
    ASSERT_EQ(in_len, WG_INIT_SIZE);
    ASSERT_EQ(read32_le(result), WG_HANDSHAKE_INIT);
    ASSERT_MEM_EQ(result + 4, orig + 4, WG_INIT_SIZE - 4);
}

/* 10. Roundtrip handshake response */
static void test_roundtrip_response(void) {
    awg_config_t cfg = make_test_config();
    uint8_t orig[WG_RESP_SIZE];
    write32_le(orig, WG_HANDSHAKE_RESPONSE);
    fill_seq(orig + 4, WG_RESP_SIZE - 4);

    uint8_t buf[256 + WG_RESP_SIZE];
    int dataoff = 256;
    memset(buf, 0, sizeof(buf));
    memcpy(buf + dataoff, orig, WG_RESP_SIZE);

    int out_len, sendJunk;
    uint8_t *out = transform_outbound(buf, dataoff, WG_RESP_SIZE, &cfg, 99, &out_len, &sendJunk);

    int in_len;
    uint8_t *result = transform_inbound(out, out_len, &cfg, &in_len);
    ASSERT(result != NULL);
    ASSERT_EQ(in_len, WG_RESP_SIZE);
    ASSERT_EQ(read32_le(result), WG_HANDSHAKE_RESPONSE);
}

/* 11. Roundtrip cookie */
static void test_roundtrip_cookie(void) {
    awg_config_t cfg = make_test_config();
    uint8_t orig[WG_COOKIE_SIZE];
    write32_le(orig, WG_COOKIE_REPLY);
    fill_seq(orig + 4, WG_COOKIE_SIZE - 4);

    uint8_t buf[256 + WG_COOKIE_SIZE];
    int dataoff = 256;
    memset(buf, 0, sizeof(buf));
    memcpy(buf + dataoff, orig, WG_COOKIE_SIZE);

    int out_len, sendJunk;
    uint8_t *out = transform_outbound(buf, dataoff, WG_COOKIE_SIZE, &cfg, 99, &out_len, &sendJunk);

    int in_len;
    uint8_t *result = transform_inbound(out, out_len, &cfg, &in_len);
    ASSERT(result != NULL);
    ASSERT_EQ(read32_le(result), WG_COOKIE_REPLY);
}

/* 12. Roundtrip transport */
static void test_roundtrip_transport(void) {
    awg_config_t cfg = make_test_config();
    uint8_t orig[200];
    write32_le(orig, WG_TRANSPORT_DATA);
    fill_seq(orig + 4, 196);

    uint8_t buf[256 + 200];
    int dataoff = 256;
    memset(buf, 0, sizeof(buf));
    memcpy(buf + dataoff, orig, 200);

    int out_len, sendJunk;
    uint8_t *out = transform_outbound(buf, dataoff, 200, &cfg, 99, &out_len, &sendJunk);

    int in_len;
    uint8_t *result = transform_inbound(out, out_len, &cfg, &in_len);
    ASSERT(result != NULL);
    ASSERT_EQ(in_len, 200);
    ASSERT_EQ(read32_le(result), WG_TRANSPORT_DATA);
    ASSERT_MEM_EQ(result + 4, orig + 4, 196);
}

/* 13. Generate junk packets */
static void test_generate_junk(void) {
    awg_config_t cfg = make_test_config();
    uint8_t jbuf[3 * 500];
    int sizes[3];
    fastrand_t rng;
    fastrand_init(&rng, 42);
    fastrand_fill(&rng, jbuf, sizeof(jbuf));

    int n = generate_junk(&cfg, jbuf, sizes);
    ASSERT_EQ(n, 3);
    for (int i = 0; i < 3; i++) {
        ASSERT(sizes[i] >= 30);
        ASSERT(sizes[i] <= 500);
    }
}

/* 14. Junk Jc=0 */
static void test_generate_junk_zero_jc(void) {
    awg_config_t cfg = make_test_config();
    cfg.jc = 0;
    uint8_t jbuf[4];
    int sizes[1];
    int n = generate_junk(&cfg, jbuf, sizes);
    ASSERT_EQ(n, 0);
}

/* 15. Inbound drops unknown type */
static void test_inbound_drops_unknown(void) {
    awg_config_t cfg = make_test_config();
    uint8_t buf[100];
    memset(buf, 0, sizeof(buf));
    write32_le(buf, 99999);

    int out_len;
    ASSERT(transform_inbound(buf, 100, &cfg, &out_len) == NULL);
}

/* 16. Inbound drops too short */
static void test_inbound_drops_too_short(void) {
    awg_config_t cfg = make_test_config();
    uint8_t buf[3] = {1, 0, 0};
    int out_len;
    ASSERT(transform_inbound(buf, 3, &cfg, &out_len) == NULL);
}

/* 17. S1=0 */
static void test_no_padding_s1_zero(void) {
    awg_config_t cfg = make_test_config();
    cfg.s1 = 0;
    config_compute(&cfg);

    uint8_t buf[256 + WG_INIT_SIZE];
    int dataoff = 256;
    memset(buf, 0, sizeof(buf));
    uint8_t *data = buf + dataoff;
    write32_le(data, WG_HANDSHAKE_INIT);
    fill_seq(data + 4, WG_INIT_SIZE - 4);

    int out_len, sendJunk;
    uint8_t *out = transform_outbound(buf, dataoff, WG_INIT_SIZE, &cfg, 12345, &out_len, &sendJunk);
    ASSERT_EQ(out_len, WG_INIT_SIZE);
    ASSERT_EQ(sendJunk, 1);
    ASSERT(hrange_contains(&cfg.h1, read32_le(out)));

    /* Roundtrip */
    int in_len;
    uint8_t *result = transform_inbound(out, out_len, &cfg, &in_len);
    ASSERT(result != NULL);
    ASSERT_EQ(read32_le(result), WG_HANDSHAKE_INIT);
}

/* 18. S2=0 */
static void test_no_padding_s2_zero(void) {
    awg_config_t cfg = make_test_config();
    cfg.s2 = 0;
    config_compute(&cfg);

    uint8_t buf[256 + WG_RESP_SIZE];
    int dataoff = 256;
    memset(buf, 0, sizeof(buf));
    uint8_t *data = buf + dataoff;
    write32_le(data, WG_HANDSHAKE_RESPONSE);
    fill_seq(data + 4, WG_RESP_SIZE - 4);

    int out_len, sendJunk;
    uint8_t *out = transform_outbound(buf, dataoff, WG_RESP_SIZE, &cfg, 12345, &out_len, &sendJunk);
    ASSERT_EQ(out_len, WG_RESP_SIZE);

    /* Roundtrip */
    int in_len;
    uint8_t *result = transform_inbound(out, out_len, &cfg, &in_len);
    ASSERT(result != NULL);
    ASSERT_EQ(read32_le(result), WG_HANDSHAKE_RESPONSE);
}

/* 19. Outbound too short */
static void test_outbound_too_short(void) {
    awg_config_t cfg = make_test_config();
    uint8_t buf[256 + 2];
    int dataoff = 256;
    buf[dataoff] = 0xAA;
    buf[dataoff + 1] = 0xBB;

    int out_len, sendJunk;
    uint8_t *out = transform_outbound(buf, dataoff, 2, &cfg, 0, &out_len, &sendJunk);
    ASSERT_EQ(sendJunk, 0);
    ASSERT_EQ(out_len, 2);
    ASSERT_EQ(out[0], 0xAA);
    ASSERT_EQ(out[1], 0xBB);
}

/* 20. HRange pick/contains */
static void test_hrange_pick_contains(void) {
    /* Point range */
    hrange_t r1 = {42, 42};
    for (int i = 0; i < 100; i++) {
        ASSERT_EQ(hrange_pick(&r1, (uint64_t)i), 42u);
    }

    /* Wide range */
    hrange_t r2 = {100, 200};
    for (int i = 0; i < 100; i++) {
        uint32_t v = hrange_pick(&r2, (uint64_t)(i * 7919));
        ASSERT(v >= 100 && v <= 200);
    }

    /* contains */
    hrange_t r3 = {10, 20};
    ASSERT(hrange_contains(&r3, 10));
    ASSERT(hrange_contains(&r3, 15));
    ASSERT(hrange_contains(&r3, 20));
    ASSERT(!hrange_contains(&r3, 9));
    ASSERT(!hrange_contains(&r3, 21));

    hrange_t r4 = {1000200001u, 4294967295u};
    for (int i = 0; i < 1000; i++) {
        uint32_t v = hrange_pick(&r4, (uint64_t)i * 0x9E3779B97F4A7C15ULL);
        ASSERT(hrange_contains(&r4, v));
    }

    hrange_t r5 = {0u, UINT32_MAX};
    ASSERT_EQ(hrange_pick(&r5, 0x1122334455667788ULL), 0x55667788u);
}

static void test_config_validate_accepts_safe_limits(void) {
    awg_config_t cfg = make_test_config();
    const char *err = "unexpected";

    cfg.s1 = AWG_PACKET_BUF_SIZE - WG_INIT_SIZE;
    cfg.s2 = AWG_PACKET_BUF_SIZE - WG_RESP_SIZE;
    cfg.s3 = AWG_PACKET_BUF_SIZE - WG_COOKIE_SIZE;
    cfg.s4 = AWG_PACKET_HEADROOM;

    ASSERT_EQ(config_validate(&cfg, &err), 0);
    ASSERT(err == NULL);
}

static void test_config_validate_rejects_unsafe_padding(void) {
    awg_config_t cfg = make_test_config();
    const char *err = NULL;

    cfg.s1 = AWG_PACKET_BUF_SIZE - WG_INIT_SIZE + 1;
    ASSERT_EQ(config_validate(&cfg, &err), -1);
    ASSERT(err != NULL);

    cfg = make_test_config();
    err = NULL;
    cfg.s4 = AWG_PACKET_HEADROOM + 1;
    ASSERT_EQ(config_validate(&cfg, &err), -1);
    ASSERT(err != NULL);

    cfg = make_test_config();
    err = NULL;
    cfg.s2 = -1;
    ASSERT_EQ(config_validate(&cfg, &err), -1);
    ASSERT(err != NULL);
}

static void test_config_validate_rejects_overlapping_hranges(void) {
    awg_config_t cfg = make_test_config();
    const char *err = NULL;

    cfg.h1 = (hrange_t){100, 200};
    cfg.h2 = (hrange_t){200, 300};

    ASSERT_EQ(config_validate(&cfg, &err), -1);
    ASSERT(err != NULL);
}

/* Dual-profile snapshot/apply: inbound classification follows the active
 * profile, and switching profiles lets a packet crafted for the other one
 * decode (site-to-site fallback mechanism). */
static void test_dual_profile_switch(void) {
    awg_config_t cfg = make_test_config(); /* primary: s1=20, h1=1234567890 */
    config_snapshot_profile(&cfg, &cfg.profiles[0]);

    awg_profile_t *fb = &cfg.profiles[1];
    memset(fb, 0, sizeof(*fb));
    fb->s1 = 30; fb->s2 = 25; fb->s3 = 0; fb->s4 = 0;
    fb->h1 = (hrange_t){1000000001, 1000000001};
    fb->h2 = (hrange_t){1000000002, 1000000002};
    fb->h3 = (hrange_t){1000000003, 1000000003};
    fb->h4 = (hrange_t){1000000004, 1000000004};
    config_compute_profile(fb);
    cfg.profile_count = 2;
    cfg.active_profile = 0;

    /* Init obfuscated with the fallback profile (30-byte pad + fallback H1) */
    uint8_t fbinit[64 + WG_INIT_SIZE];
    int fbn = fb->s1 + WG_INIT_SIZE;
    memset(fbinit, 0, sizeof(fbinit));
    write32_le(fbinit + fb->s1, fb->h1.min);
    fill_seq(fbinit + fb->s1 + 4, WG_INIT_SIZE - 4);

    /* Active = primary: fallback init is not recognized */
    int out_len;
    ASSERT(transform_inbound(fbinit, fbn, &cfg, &out_len) == NULL);

    /* Switch to fallback: same packet now decodes to a WG init */
    config_apply_profile(&cfg, 1);
    ASSERT_EQ(cfg.active_profile, 1);
    memset(fbinit, 0, sizeof(fbinit));
    write32_le(fbinit + fb->s1, fb->h1.min);
    fill_seq(fbinit + fb->s1 + 4, WG_INIT_SIZE - 4);
    uint8_t *out = transform_inbound(fbinit, fbn, &cfg, &out_len);
    ASSERT(out != NULL);
    ASSERT_EQ(out_len, WG_INIT_SIZE);
    ASSERT_EQ(read32_le(out), (uint32_t)WG_HANDSHAKE_INIT);

    /* Switch back to primary: a primary-format init decodes again */
    config_apply_profile(&cfg, 0);
    ASSERT_EQ(cfg.active_profile, 0);
    uint8_t prinit[64 + WG_INIT_SIZE];
    int prn = cfg.profiles[0].s1 + WG_INIT_SIZE;
    memset(prinit, 0, sizeof(prinit));
    write32_le(prinit + cfg.profiles[0].s1, cfg.profiles[0].h1.min);
    fill_seq(prinit + cfg.profiles[0].s1 + 4, WG_INIT_SIZE - 4);
    out = transform_inbound(prinit, prn, &cfg, &out_len);
    ASSERT(out != NULL);
    ASSERT_EQ(read32_le(out), (uint32_t)WG_HANDSHAKE_INIT);
}

/* 21. Outbound cookie with S3 */
static void test_outbound_cookie_with_s3(void) {
    awg_config_t cfg = make_test_config();
    cfg.s3 = 49;
    config_compute(&cfg);

    uint8_t buf[256 + WG_COOKIE_SIZE];
    int dataoff = 256;
    memset(buf, 0, sizeof(buf));
    uint8_t *data = buf + dataoff;
    write32_le(data, WG_COOKIE_REPLY);
    fill_seq(data + 4, WG_COOKIE_SIZE - 4);

    int out_len, sendJunk;
    uint8_t *out = transform_outbound(buf, dataoff, WG_COOKIE_SIZE, &cfg, 12345, &out_len, &sendJunk);
    ASSERT_EQ(out_len, 49 + WG_COOKIE_SIZE);
    ASSERT_EQ(sendJunk, 0);
    uint32_t h = read32_le(out + 49);
    ASSERT(hrange_contains(&cfg.h3, h));
}

/* 22. Outbound transport with S4 */
static void test_outbound_transport_with_s4(void) {
    awg_config_t cfg = make_test_config();
    cfg.s4 = 17;
    config_compute(&cfg);

    uint8_t buf[256 + 100];
    int dataoff = 256;
    memset(buf, 0, sizeof(buf));
    uint8_t *data = buf + dataoff;
    write32_le(data, WG_TRANSPORT_DATA);
    fill_seq(data + 4, 96);

    int out_len, sendJunk;
    uint8_t *out = transform_outbound(buf, dataoff, 100, &cfg, 12345, &out_len, &sendJunk);
    ASSERT_EQ(out_len, 17 + 100);
    ASSERT_EQ(sendJunk, 0);
    uint32_t h = read32_le(out + 17);
    ASSERT(hrange_contains(&cfg.h4, h));
}

/* 23. Inbound scanning with S3/S4 */
static void test_inbound_scanning_s3(void) {
    awg_config_t cfg = make_test_config();
    cfg.s3 = 49;
    config_compute(&cfg);

    int total = 49 + WG_COOKIE_SIZE;
    uint8_t buf[256];
    memset(buf, 0xBB, sizeof(buf));
    write32_le(buf + 49, cfg.h3.min);

    int out_len;
    uint8_t *out = transform_inbound(buf, total, &cfg, &out_len);
    ASSERT(out != NULL);
    ASSERT_EQ(out_len, WG_COOKIE_SIZE);
    ASSERT_EQ(read32_le(out), WG_COOKIE_REPLY);
}

static void test_inbound_scanning_s4(void) {
    awg_config_t cfg = make_test_config();
    cfg.s4 = 17;
    config_compute(&cfg);

    int total = 17 + 100;
    uint8_t buf[256];
    memset(buf, 0xBB, sizeof(buf));
    write32_le(buf + 17, cfg.h4.min);

    int out_len;
    uint8_t *out = transform_inbound(buf, total, &cfg, &out_len);
    ASSERT(out != NULL);
    ASSERT_EQ(out_len, 100);
    ASSERT_EQ(read32_le(out), WG_TRANSPORT_DATA);
}

/* 24. Inbound H range / reject */
static void test_inbound_hrange_accept(void) {
    awg_config_t cfg = make_test_config();
    cfg.h4 = (hrange_t){1000, 2000};
    config_compute(&cfg);

    uint8_t buf[100];
    memset(buf, 0, sizeof(buf));
    write32_le(buf, 1500);

    int out_len;
    uint8_t *out = transform_inbound(buf, 100, &cfg, &out_len);
    ASSERT(out != NULL);
    ASSERT_EQ(read32_le(out), WG_TRANSPORT_DATA);
}

static void test_inbound_hrange_reject(void) {
    awg_config_t cfg = make_test_config();
    cfg.h4 = (hrange_t){1000, 2000};
    config_compute(&cfg);

    uint8_t buf[100];
    memset(buf, 0, sizeof(buf));
    write32_le(buf, 999);

    int out_len;
    ASSERT(transform_inbound(buf, 100, &cfg, &out_len) == NULL);
}

/* 25. Roundtrip v2 (S3, S4, H ranges) */
static void test_roundtrip_v2(void) {
    awg_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.jc = 3; cfg.jmin = 30; cfg.jmax = 500;
    cfg.s1 = 20; cfg.s2 = 20; cfg.s3 = 49; cfg.s4 = 17;
    cfg.h1 = (hrange_t){100000, 200000};
    cfg.h2 = (hrange_t){300000, 400000};
    cfg.h3 = (hrange_t){500000, 600000};
    cfg.h4 = (hrange_t){700000, 800000};
    config_compute(&cfg);

    /* Handshake init roundtrip */
    {
        uint8_t orig[WG_INIT_SIZE];
        write32_le(orig, WG_HANDSHAKE_INIT);
        fill_seq(orig + 4, WG_INIT_SIZE - 4);

        uint8_t buf[256 + WG_INIT_SIZE];
        int dataoff = 256;
        memset(buf, 0, sizeof(buf));
        memcpy(buf + dataoff, orig, WG_INIT_SIZE);

        int out_len, sendJunk;
        uint8_t *out = transform_outbound(buf, dataoff, WG_INIT_SIZE, &cfg, 42, &out_len, &sendJunk);
        ASSERT_EQ(out_len, 20 + WG_INIT_SIZE);

        int in_len;
        uint8_t *r = transform_inbound(out, out_len, &cfg, &in_len);
        ASSERT(r != NULL);
        ASSERT_EQ(read32_le(r), WG_HANDSHAKE_INIT);
        ASSERT_MEM_EQ(r + 4, orig + 4, WG_INIT_SIZE - 4);
    }

    /* Cookie reply roundtrip */
    {
        uint8_t orig[WG_COOKIE_SIZE];
        write32_le(orig, WG_COOKIE_REPLY);
        fill_seq(orig + 4, WG_COOKIE_SIZE - 4);

        uint8_t buf[256 + WG_COOKIE_SIZE];
        int dataoff = 256;
        memset(buf, 0, sizeof(buf));
        memcpy(buf + dataoff, orig, WG_COOKIE_SIZE);

        int out_len, sendJunk;
        uint8_t *out = transform_outbound(buf, dataoff, WG_COOKIE_SIZE, &cfg, 77, &out_len, &sendJunk);
        ASSERT_EQ(out_len, 49 + WG_COOKIE_SIZE);

        int in_len;
        uint8_t *r = transform_inbound(out, out_len, &cfg, &in_len);
        ASSERT(r != NULL);
        ASSERT_EQ(read32_le(r), WG_COOKIE_REPLY);
    }

    /* Transport data roundtrip */
    {
        uint8_t orig[200];
        write32_le(orig, WG_TRANSPORT_DATA);
        fill_seq(orig + 4, 196);

        uint8_t buf[256 + 200];
        int dataoff = 256;
        memset(buf, 0, sizeof(buf));
        memcpy(buf + dataoff, orig, 200);

        int out_len, sendJunk;
        uint8_t *out = transform_outbound(buf, dataoff, 200, &cfg, 123, &out_len, &sendJunk);
        ASSERT_EQ(out_len, 17 + 200);

        int in_len;
        uint8_t *r = transform_inbound(out, out_len, &cfg, &in_len);
        ASSERT(r != NULL);
        ASSERT_EQ(in_len, 200);
        ASSERT_EQ(read32_le(r), WG_TRANSPORT_DATA);
        ASSERT_MEM_EQ(r + 4, orig + 4, 196);
    }
}

/* 26. V1 backward compatibility */
static void test_v1_backward(void) {
    awg_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.jc = 2; cfg.jmin = 10; cfg.jmax = 50;
    cfg.s1 = 46; cfg.s2 = 122; cfg.s3 = 0; cfg.s4 = 0;
    cfg.h1 = (hrange_t){1033089720, 1033089720};
    cfg.h2 = (hrange_t){1336452505, 1336452505};
    cfg.h3 = (hrange_t){1858775673, 1858775673};
    cfg.h4 = (hrange_t){332219739, 332219739};
    config_compute(&cfg);

    /* Handshake init */
    {
        uint8_t buf[256 + WG_INIT_SIZE];
        int dataoff = 256;
        memset(buf, 0, sizeof(buf));
        uint8_t *data = buf + dataoff;
        write32_le(data, WG_HANDSHAKE_INIT);
        fill_seq(data + 4, WG_INIT_SIZE - 4);

        int out_len, sendJunk;
        transform_outbound(buf, dataoff, WG_INIT_SIZE, &cfg, 0, &out_len, &sendJunk);
        ASSERT_EQ(out_len, 46 + WG_INIT_SIZE);
        ASSERT_EQ(sendJunk, 1);
    }

    /* Transport data: no S4 padding */
    {
        uint8_t buf[256 + 100];
        int dataoff = 256;
        memset(buf, 0, sizeof(buf));
        uint8_t *data = buf + dataoff;
        write32_le(data, WG_TRANSPORT_DATA);

        int out_len, sendJunk;
        transform_outbound(buf, dataoff, 100, &cfg, 0, &out_len, &sendJunk);
        ASSERT_EQ(out_len, 100);
    }

    /* Cookie: no S3 padding */
    {
        uint8_t buf[256 + WG_COOKIE_SIZE];
        int dataoff = 256;
        memset(buf, 0, sizeof(buf));
        uint8_t *data = buf + dataoff;
        write32_le(data, WG_COOKIE_REPLY);

        int out_len, sendJunk;
        transform_outbound(buf, dataoff, WG_COOKIE_SIZE, &cfg, 0, &out_len, &sendJunk);
        ASSERT_EQ(out_len, WG_COOKIE_SIZE);
    }
}

/* 27. V2 false positive regression */
static void test_v2_false_positive(void) {
    awg_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.jc = 2; cfg.jmin = 10; cfg.jmax = 50;
    cfg.s1 = 46; cfg.s2 = 62; cfg.s3 = 30; cfg.s4 = 17;
    cfg.h1 = (hrange_t){100000, 200000};
    cfg.h2 = (hrange_t){300000, 400000};
    cfg.h3 = (hrange_t){500000, 600000};
    cfg.h4 = (hrange_t){0, 1073741823}; /* wide: 25% of uint32 space */
    config_compute(&cfg);

    /* Handshake response: poison padding with H4 value */
    {
        int total = 62 + WG_RESP_SIZE;
        uint8_t buf[256];
        memset(buf, 0, sizeof(buf));
        /* Poison padding with a value in H4 range */
        write32_le(buf, 500000000); /* in H4 range */
        write32_le(buf + 62, cfg.h2.min);
        fill_seq(buf + 62 + 4, WG_RESP_SIZE - 4);

        int out_len;
        uint8_t *out = transform_inbound(buf, total, &cfg, &out_len);
        ASSERT(out != NULL);
        ASSERT_EQ(read32_le(out), WG_HANDSHAKE_RESPONSE);
    }

    /* Handshake init: poison padding with H4 value */
    {
        int total = 46 + WG_INIT_SIZE;
        uint8_t buf[256];
        memset(buf, 0, sizeof(buf));
        write32_le(buf, 500000000);
        write32_le(buf + 46, cfg.h1.min);
        fill_seq(buf + 46 + 4, WG_INIT_SIZE - 4);

        int out_len;
        uint8_t *out = transform_inbound(buf, total, &cfg, &out_len);
        ASSERT(out != NULL);
        ASSERT_EQ(read32_le(out), WG_HANDSHAKE_INIT);
    }

    /* Cookie reply: poison padding */
    {
        int total = 30 + WG_COOKIE_SIZE;
        uint8_t buf[256];
        memset(buf, 0, sizeof(buf));
        write32_le(buf, 500000000);
        write32_le(buf + 30, cfg.h3.min);

        int out_len;
        uint8_t *out = transform_inbound(buf, total, &cfg, &out_len);
        ASSERT(out != NULL);
        ASSERT_EQ(read32_le(out), WG_COOKIE_REPLY);
    }

    /* Transport data */
    {
        int total = 17 + 100;
        uint8_t buf[256];
        memset(buf, 0, sizeof(buf));
        write32_le(buf + 17, cfg.h4.min);

        int out_len;
        uint8_t *out = transform_inbound(buf, total, &cfg, &out_len);
        ASSERT(out != NULL);
        ASSERT_EQ(read32_le(out), WG_TRANSPORT_DATA);
    }

    /* Junk packet: too small for anything */
    {
        uint8_t buf[17];
        memset(buf, 0, sizeof(buf));
        write32_le(buf, 500000000);

        int out_len;
        ASSERT(transform_inbound(buf, 17, &cfg, &out_len) == NULL);
    }
}

/* --- MAC1 tests --- */

/* Helper: verify MAC1 is correct for handshake init (148 bytes) */
static int verify_mac1_init(const uint8_t *pkt, const uint8_t mac1key[32]) {
    uint8_t expected[16];
    blake2s_128mac(mac1key, pkt, 116, expected);
    return memcmp(pkt + 116, expected, 16) == 0;
}

/* Helper: verify MAC1 is correct for handshake response (92 bytes) */
static int verify_mac1_response(const uint8_t *pkt, const uint8_t mac1key[32]) {
    uint8_t expected[16];
    blake2s_128mac(mac1key, pkt, 60, expected);
    return memcmp(pkt + 60, expected, 16) == 0;
}

/* Config with real pubkeys for MAC1 testing.
 * Fills cfg in-place to keep mac1key_out/mac1key_in pointers valid. */
static void make_mac1_config(awg_config_t *cfg, int mode) {
    memset(cfg, 0, sizeof(*cfg));
    cfg->jc = 3; cfg->jmin = 30; cfg->jmax = 500;
    cfg->s1 = 20; cfg->s2 = 20;
    cfg->h1 = (hrange_t){1234567890, 1234567890};
    cfg->h2 = (hrange_t){1234567891, 1234567891};
    cfg->h3 = (hrange_t){1234567892, 1234567892};
    cfg->h4 = (hrange_t){1234567893, 1234567893};
    /* Distinct test keys */
    for (int i = 0; i < 32; i++) cfg->server_pub[i] = (uint8_t)(i + 1);
    for (int i = 0; i < 32; i++) cfg->client_pub[i] = (uint8_t)(i + 0x80);
    cfg->mode = mode;
    config_compute(cfg);
}

static void fill_test_pub(uint8_t pub[32], uint8_t seed) {
    for (int i = 0; i < 32; i++)
        pub[i] = (uint8_t)(seed + i);
}

static void build_test_response(uint8_t *pkt, const uint8_t mac1key[32], uint32_t receiver_index) {
    memset(pkt, 0, WG_RESP_SIZE);
    write32_le(pkt, WG_HANDSHAKE_RESPONSE);
    fill_seq(pkt + 4, WG_RESP_SIZE - 4);
    memcpy(pkt + 8, &receiver_index, 4);
    recompute_mac1_response(pkt, mac1key);
}

static void build_test_init(uint8_t *pkt, const uint8_t mac1key[32], uint32_t sender_index) {
    memset(pkt, 0, WG_INIT_SIZE);
    write32_le(pkt, WG_HANDSHAKE_INIT);
    fill_seq(pkt + 4, WG_INIT_SIZE - 4);
    memcpy(pkt + 4, &sender_index, 4);
    recompute_mac1(pkt, mac1key);
}

/* A handshake the server starts carries no receiver_index, so the only way to
 * aim it at the right client is its MAC1 — keyed on that client's static key. */
static void test_server_init_peer_resolution(void) {
    awg_config_t cfg;
    make_mac1_config(&cfg, AWG_MODE_SERVER);
    memset(cfg.client_pub, 0, sizeof(cfg.client_pub));
    cfg.server_peer_count = 2;
    fill_test_pub(cfg.server_peer_pubs[0], 0x20);
    fill_test_pub(cfg.server_peer_pubs[1], 0x40);
    config_compute(&cfg);

    uint8_t buf[WG_INIT_SIZE];

    build_test_init(buf, cfg.server_peer_mac1keys[0], 0xAAAAAAAAu);
    ASSERT_EQ(config_server_resolve_peer_for_init(&cfg, buf, WG_INIT_SIZE), 0);

    build_test_init(buf, cfg.server_peer_mac1keys[1], 0xBBBBBBBBu);
    ASSERT_EQ(config_server_resolve_peer_for_init(&cfg, buf, WG_INIT_SIZE), 1);

    /* A key nobody is configured with must not match anything. */
    uint8_t stranger_pub[32], stranger_key[32];
    fill_test_pub(stranger_pub, 0x77);
    compute_mac1_key(stranger_pub, stranger_key);
    build_test_init(buf, stranger_key, 0xCCCCCCCCu);
    ASSERT_EQ(config_server_resolve_peer_for_init(&cfg, buf, WG_INIT_SIZE), -1);

    /* Wrong size or wrong type is not an init. */
    build_test_init(buf, cfg.server_peer_mac1keys[0], 0xAAAAAAAAu);
    ASSERT_EQ(config_server_resolve_peer_for_init(&cfg, buf, WG_INIT_SIZE - 1), -1);
    write32_le(buf, WG_HANDSHAKE_RESPONSE);
    ASSERT_EQ(config_server_resolve_peer_for_init(&cfg, buf, WG_INIT_SIZE), -1);
}

static void test_server_response_peer_resolution_single_direct(void) {
    awg_config_t cfg;
    make_mac1_config(&cfg, AWG_MODE_SERVER);
    fill_test_pub(cfg.client_pub, 0xD0); /* legacy fallback / placeholder */
    cfg.server_peer_count = 1;
    fill_test_pub(cfg.server_peer_pubs[0], 0x20);
    config_compute(&cfg);

    uint8_t buf[256 + WG_RESP_SIZE];
    int dataoff = 256;
    build_test_response(buf + dataoff, cfg.server_peer_mac1keys[0], 0x11111111u);

    ASSERT_EQ(config_server_resolve_peer_for_response(&cfg, buf + dataoff, WG_RESP_SIZE), 0);

    int out_len, sendJunk;
    uint8_t *out = transform_outbound_with_mac1(buf, dataoff, WG_RESP_SIZE, &cfg,
                                                cfg.server_peer_mac1keys[0],
                                                42, &out_len, &sendJunk);
    ASSERT(verify_mac1_response(out + cfg.s2, cfg.server_peer_mac1keys[0]));
    ASSERT(!verify_mac1_response(out + cfg.s2, cfg.mac1key_client));
}

static void test_server_response_peer_resolution_two_direct_clients(void) {
    awg_config_t cfg;
    make_mac1_config(&cfg, AWG_MODE_SERVER);
    memset(cfg.client_pub, 0, sizeof(cfg.client_pub));
    cfg.server_peer_count = 2;
    fill_test_pub(cfg.server_peer_pubs[0], 0x20);
    fill_test_pub(cfg.server_peer_pubs[1], 0x60);
    config_compute(&cfg);

    uint8_t buf[256 + WG_RESP_SIZE];
    int dataoff = 256;
    build_test_response(buf + dataoff, cfg.server_peer_mac1keys[1], 0x22222222u);

    ASSERT_EQ(config_server_resolve_peer_for_response(&cfg, buf + dataoff, WG_RESP_SIZE), 1);

    int out_len, sendJunk;
    uint8_t *out = transform_outbound_with_mac1(buf, dataoff, WG_RESP_SIZE, &cfg,
                                                cfg.server_peer_mac1keys[1],
                                                42, &out_len, &sendJunk);
    ASSERT(verify_mac1_response(out + cfg.s2, cfg.server_peer_mac1keys[1]));
    ASSERT(!verify_mac1_response(out + cfg.s2, cfg.server_peer_mac1keys[0]));
}

static void test_server_response_peer_resolution_mixed_direct_and_proxy_fallback(void) {
    awg_config_t cfg;
    uint8_t unknown_pub[32];
    uint8_t unknown_mac1key[32];

    make_mac1_config(&cfg, AWG_MODE_SERVER);
    fill_test_pub(cfg.client_pub, 0xD0); /* placeholder / legacy proxy fallback */
    cfg.server_peer_count = 1;
    fill_test_pub(cfg.server_peer_pubs[0], 0x20); /* direct AWG client */
    fill_test_pub(unknown_pub, 0x90);             /* proxy-only WG peer not listed */
    config_compute(&cfg);
    compute_mac1_key(unknown_pub, unknown_mac1key);

    {
        uint8_t buf[256 + WG_RESP_SIZE];
        int dataoff = 256;
        build_test_response(buf + dataoff, cfg.server_peer_mac1keys[0], 0x33333333u);
        ASSERT_EQ(config_server_resolve_peer_for_response(&cfg, buf + dataoff, WG_RESP_SIZE), 0);
    }

    {
        uint8_t buf[256 + WG_RESP_SIZE];
        int dataoff = 256;
        build_test_response(buf + dataoff, unknown_mac1key, 0x44444444u);
        ASSERT_EQ(config_server_resolve_peer_for_response(&cfg, buf + dataoff, WG_RESP_SIZE), -1);

        int out_len, sendJunk;
        uint8_t *out = transform_outbound(buf, dataoff, WG_RESP_SIZE, &cfg, 42, &out_len, &sendJunk);
        ASSERT(verify_mac1_response(out + cfg.s2, cfg.mac1key_client));
        ASSERT(!verify_mac1_response(out + cfg.s2, unknown_mac1key));
    }
}

/* Bug #1 (critical): outbound response must recompute MAC1 */
static void test_mac1_outbound_response_normal(void) {
    awg_config_t cfg; make_mac1_config(&cfg, AWG_MODE_NORMAL);
    uint8_t buf[256 + WG_RESP_SIZE];
    int dataoff = 256;
    memset(buf, 0xAA, sizeof(buf));
    uint8_t *data = buf + dataoff;
    write32_le(data, WG_HANDSHAKE_RESPONSE);
    fill_seq(data + 4, WG_RESP_SIZE - 4);

    int out_len, sendJunk;
    uint8_t *out = transform_outbound(buf, dataoff, WG_RESP_SIZE, &cfg, 42, &out_len, &sendJunk);
    /* MAC1 must be valid for server key (recipient = AWG server in normal mode) */
    uint8_t *pkt = out + cfg.s2;
    ASSERT(verify_mac1_response(pkt, cfg.mac1key_server));
}

/* Bug #2: outbound init in server mode must use client key */
static void test_mac1_outbound_init_server(void) {
    awg_config_t cfg; make_mac1_config(&cfg, AWG_MODE_SERVER);
    uint8_t buf[256 + WG_INIT_SIZE];
    int dataoff = 256;
    memset(buf, 0xAA, sizeof(buf));
    uint8_t *data = buf + dataoff;
    write32_le(data, WG_HANDSHAKE_INIT);
    fill_seq(data + 4, WG_INIT_SIZE - 4);

    int out_len, sendJunk;
    uint8_t *out = transform_outbound(buf, dataoff, WG_INIT_SIZE, &cfg, 42, &out_len, &sendJunk);
    uint8_t *pkt = out + cfg.s1;
    /* In server mode, outbound goes to AWG client → client key */
    ASSERT(verify_mac1_init(pkt, cfg.mac1key_client));
    /* Must NOT match server key */
    ASSERT(!verify_mac1_init(pkt, cfg.mac1key_server));
}

/* Bug #3: inbound init in normal mode must recompute MAC1 */
static void test_mac1_inbound_init_normal(void) {
    awg_config_t cfg; make_mac1_config(&cfg, AWG_MODE_NORMAL);
    int total = cfg.s1 + WG_INIT_SIZE;
    uint8_t buf[256];
    memset(buf, 0xAA, sizeof(buf));
    write32_le(buf + cfg.s1, cfg.h1.min);
    fill_seq(buf + cfg.s1 + 4, WG_INIT_SIZE - 4);

    int out_len;
    uint8_t *out = transform_inbound(buf, total, &cfg, &out_len);
    ASSERT(out != NULL);
    ASSERT_EQ(out_len, WG_INIT_SIZE);
    /* In normal mode, inbound init goes to WG client → client key */
    ASSERT(verify_mac1_init(out, cfg.mac1key_client));
}

/* Bug #4: inbound response in server mode must use server key */
static void test_mac1_inbound_response_server(void) {
    awg_config_t cfg; make_mac1_config(&cfg, AWG_MODE_SERVER);
    int total = cfg.s2 + WG_RESP_SIZE;
    uint8_t buf[256];
    memset(buf, 0xAA, sizeof(buf));
    write32_le(buf + cfg.s2, cfg.h2.min);
    fill_seq(buf + cfg.s2 + 4, WG_RESP_SIZE - 4);

    int out_len;
    uint8_t *out = transform_inbound(buf, total, &cfg, &out_len);
    ASSERT(out != NULL);
    ASSERT_EQ(out_len, WG_RESP_SIZE);
    /* In server mode, inbound response goes to WG server → server key */
    ASSERT(verify_mac1_response(out, cfg.mac1key_server));
    /* Must NOT match client key */
    ASSERT(!verify_mac1_response(out, cfg.mac1key_client));
}

/* Outbound init normal: uses server key */
static void test_mac1_outbound_init_normal(void) {
    awg_config_t cfg; make_mac1_config(&cfg, AWG_MODE_NORMAL);
    uint8_t buf[256 + WG_INIT_SIZE];
    int dataoff = 256;
    memset(buf, 0xAA, sizeof(buf));
    uint8_t *data = buf + dataoff;
    write32_le(data, WG_HANDSHAKE_INIT);
    fill_seq(data + 4, WG_INIT_SIZE - 4);

    int out_len, sendJunk;
    uint8_t *out = transform_outbound(buf, dataoff, WG_INIT_SIZE, &cfg, 42, &out_len, &sendJunk);
    uint8_t *pkt = out + cfg.s1;
    ASSERT(verify_mac1_init(pkt, cfg.mac1key_server));
    ASSERT(!verify_mac1_init(pkt, cfg.mac1key_client));
}

/* Outbound response in server mode: uses client key */
static void test_mac1_outbound_response_server(void) {
    awg_config_t cfg; make_mac1_config(&cfg, AWG_MODE_SERVER);
    uint8_t buf[256 + WG_RESP_SIZE];
    int dataoff = 256;
    memset(buf, 0xAA, sizeof(buf));
    uint8_t *data = buf + dataoff;
    write32_le(data, WG_HANDSHAKE_RESPONSE);
    fill_seq(data + 4, WG_RESP_SIZE - 4);

    int out_len, sendJunk;
    uint8_t *out = transform_outbound(buf, dataoff, WG_RESP_SIZE, &cfg, 42, &out_len, &sendJunk);
    uint8_t *pkt = out + cfg.s2;
    ASSERT(verify_mac1_response(pkt, cfg.mac1key_client));
    ASSERT(!verify_mac1_response(pkt, cfg.mac1key_server));
}

/* Inbound init in server mode: uses server key */
static void test_mac1_inbound_init_server(void) {
    awg_config_t cfg; make_mac1_config(&cfg, AWG_MODE_SERVER);
    int total = cfg.s1 + WG_INIT_SIZE;
    uint8_t buf[256];
    memset(buf, 0xAA, sizeof(buf));
    write32_le(buf + cfg.s1, cfg.h1.min);
    fill_seq(buf + cfg.s1 + 4, WG_INIT_SIZE - 4);

    int out_len;
    uint8_t *out = transform_inbound(buf, total, &cfg, &out_len);
    ASSERT(out != NULL);
    ASSERT(verify_mac1_init(out, cfg.mac1key_server));
    ASSERT(!verify_mac1_init(out, cfg.mac1key_client));
}

/* Inbound response in normal mode: uses client key */
static void test_mac1_inbound_response_normal(void) {
    awg_config_t cfg; make_mac1_config(&cfg, AWG_MODE_NORMAL);
    int total = cfg.s2 + WG_RESP_SIZE;
    uint8_t buf[256];
    memset(buf, 0xAA, sizeof(buf));
    write32_le(buf + cfg.s2, cfg.h2.min);
    fill_seq(buf + cfg.s2 + 4, WG_RESP_SIZE - 4);

    int out_len;
    uint8_t *out = transform_inbound(buf, total, &cfg, &out_len);
    ASSERT(out != NULL);
    ASSERT(verify_mac1_response(out, cfg.mac1key_client));
    ASSERT(!verify_mac1_response(out, cfg.mac1key_server));
}

/* mac1key_out/in are NULL when pubkey is zero */
static void test_mac1key_null_when_no_pub(void) {
    awg_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.h1 = (hrange_t){100, 100};
    cfg.h2 = (hrange_t){200, 200};
    cfg.h3 = (hrange_t){300, 300};
    cfg.h4 = (hrange_t){400, 400};
    /* Both keys zero */
    cfg.mode = AWG_MODE_NORMAL;
    config_compute(&cfg);
    ASSERT(cfg.mac1key_out == NULL);
    ASSERT(cfg.mac1key_in == NULL);

    /* Only server_pub set */
    for (int i = 0; i < 32; i++) cfg.server_pub[i] = (uint8_t)(i + 1);
    cfg.mode = AWG_MODE_NORMAL;
    config_compute(&cfg);
    ASSERT(cfg.mac1key_out != NULL); /* server key for outbound */
    ASSERT(cfg.mac1key_in == NULL);  /* no client key */

    /* Server mode with only server_pub */
    cfg.mode = AWG_MODE_SERVER;
    config_compute(&cfg);
    ASSERT(cfg.mac1key_out == NULL);  /* no client key for outbound */
    ASSERT(cfg.mac1key_in != NULL);   /* server key for inbound */
}

/* MAC1 roundtrip: outbound→inbound, both directions, both modes */
static void test_mac1_roundtrip_normal(void) {
    awg_config_t cfg; make_mac1_config(&cfg, AWG_MODE_NORMAL);
    /* Init roundtrip */
    {
        uint8_t buf[256 + WG_INIT_SIZE];
        int dataoff = 256;
        memset(buf, 0, sizeof(buf));
        uint8_t *data = buf + dataoff;
        write32_le(data, WG_HANDSHAKE_INIT);
        fill_seq(data + 4, WG_INIT_SIZE - 4);
        /* Compute original MAC1 with WG type (simulating WG stack) */
        recompute_mac1(data, cfg.mac1key_client);

        int out_len, sendJunk;
        uint8_t *out = transform_outbound(buf, dataoff, WG_INIT_SIZE, &cfg, 42, &out_len, &sendJunk);
        /* After outbound: MAC1 valid for server key */
        ASSERT(verify_mac1_init(out + cfg.s1, cfg.mac1key_server));

        int in_len;
        uint8_t *result = transform_inbound(out, out_len, &cfg, &in_len);
        ASSERT(result != NULL);
        ASSERT_EQ(in_len, WG_INIT_SIZE);
        /* After inbound: MAC1 valid for client key (back to WG) */
        ASSERT(verify_mac1_init(result, cfg.mac1key_client));
    }
    /* Response roundtrip */
    {
        uint8_t buf[256 + WG_RESP_SIZE];
        int dataoff = 256;
        memset(buf, 0, sizeof(buf));
        uint8_t *data = buf + dataoff;
        write32_le(data, WG_HANDSHAKE_RESPONSE);
        fill_seq(data + 4, WG_RESP_SIZE - 4);
        recompute_mac1_response(data, cfg.mac1key_server);

        int out_len, sendJunk;
        uint8_t *out = transform_outbound(buf, dataoff, WG_RESP_SIZE, &cfg, 42, &out_len, &sendJunk);
        ASSERT(verify_mac1_response(out + cfg.s2, cfg.mac1key_server));

        int in_len;
        uint8_t *result = transform_inbound(out, out_len, &cfg, &in_len);
        ASSERT(result != NULL);
        ASSERT(verify_mac1_response(result, cfg.mac1key_client));
    }
}

static void test_mac1_roundtrip_server(void) {
    awg_config_t cfg; make_mac1_config(&cfg, AWG_MODE_SERVER);
    /* Init roundtrip in server mode */
    {
        uint8_t buf[256 + WG_INIT_SIZE];
        int dataoff = 256;
        memset(buf, 0, sizeof(buf));
        uint8_t *data = buf + dataoff;
        write32_le(data, WG_HANDSHAKE_INIT);
        fill_seq(data + 4, WG_INIT_SIZE - 4);
        recompute_mac1(data, cfg.mac1key_server);

        int out_len, sendJunk;
        uint8_t *out = transform_outbound(buf, dataoff, WG_INIT_SIZE, &cfg, 42, &out_len, &sendJunk);
        /* Server mode outbound → client key */
        ASSERT(verify_mac1_init(out + cfg.s1, cfg.mac1key_client));

        int in_len;
        uint8_t *result = transform_inbound(out, out_len, &cfg, &in_len);
        ASSERT(result != NULL);
        /* Server mode inbound → server key */
        ASSERT(verify_mac1_init(result, cfg.mac1key_server));
    }
    /* Response roundtrip in server mode */
    {
        uint8_t buf[256 + WG_RESP_SIZE];
        int dataoff = 256;
        memset(buf, 0, sizeof(buf));
        uint8_t *data = buf + dataoff;
        write32_le(data, WG_HANDSHAKE_RESPONSE);
        fill_seq(data + 4, WG_RESP_SIZE - 4);
        recompute_mac1_response(data, cfg.mac1key_client);

        int out_len, sendJunk;
        uint8_t *out = transform_outbound(buf, dataoff, WG_RESP_SIZE, &cfg, 42, &out_len, &sendJunk);
        ASSERT(verify_mac1_response(out + cfg.s2, cfg.mac1key_client));

        int in_len;
        uint8_t *result = transform_inbound(out, out_len, &cfg, &in_len);
        ASSERT(result != NULL);
        ASSERT(verify_mac1_response(result, cfg.mac1key_server));
    }
}

/* Reverse mode: same key mapping as server */
static void test_mac1_reverse_mode(void) {
    awg_config_t cfg; make_mac1_config(&cfg, AWG_MODE_REVERSE);
    /* mac1key_out should be client key, mac1key_in should be server key */
    ASSERT(cfg.mac1key_out == cfg.mac1key_client);
    ASSERT(cfg.mac1key_in == cfg.mac1key_server);

    /* Outbound init uses client key */
    uint8_t buf[256 + WG_INIT_SIZE];
    int dataoff = 256;
    memset(buf, 0xAA, sizeof(buf));
    uint8_t *data = buf + dataoff;
    write32_le(data, WG_HANDSHAKE_INIT);
    fill_seq(data + 4, WG_INIT_SIZE - 4);

    int out_len, sendJunk;
    uint8_t *out = transform_outbound(buf, dataoff, WG_INIT_SIZE, &cfg, 42, &out_len, &sendJunk);
    uint8_t *pkt = out + cfg.s1;
    ASSERT(verify_mac1_init(pkt, cfg.mac1key_client));
}

/* ---- AWG 3.0 header protection ---- */

/* v3 config: every S is >= 12 because the first 12 padding bytes are the
 * ChaCha20 nonce. */
static awg_config_t make_v3_config(void) {
    awg_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.jc = 3;
    cfg.jmin = 30;
    cfg.jmax = 500;
    cfg.s1 = 20;
    cfg.s2 = 24;
    cfg.s3 = 18;
    cfg.s4 = 14;
    cfg.h1 = (hrange_t){1234567890, 1234567890};
    cfg.h2 = (hrange_t){1234567891, 1234567891};
    cfg.h3 = (hrange_t){1234567892, 1234567892};
    cfg.h4 = (hrange_t){1234567893, 1234567893};
    for (int i = 0; i < 32; i++) cfg.hp_key[i] = (uint8_t)(i * 7 + 1);
    cfg.hp_on = 1;
    config_compute(&cfg);
    return cfg;
}

/* Run one message through outbound, then back through inbound on a scratch
 * copy. `wire` keeps the on-wire (still encrypted) bytes. Returns inbound len. */
static int hp_roundtrip(const awg_config_t *cfg, uint32_t wg_type, int msg_len,
                        int pad, uint8_t *wire, int *wire_len, uint8_t *back) {
    uint8_t buf[AWG_PACKET_HEADROOM + AWG_PACKET_BUF_SIZE];
    uint8_t scratch[AWG_PACKET_BUF_SIZE];
    int dataoff = AWG_PACKET_HEADROOM;
    uint8_t *data = buf + dataoff;

    memset(buf, 0, sizeof(buf));
    write32_le(data, wg_type);
    fill_seq(data + 4, msg_len - 4);

    int out_len, sendJunk;
    uint8_t *out = transform_outbound((uint8_t *)buf, dataoff, msg_len,
                                      cfg, 0xC0FFEE, &out_len, &sendJunk);
    if (out_len != pad + msg_len) return -1;
    memcpy(wire, out, (size_t)out_len);
    *wire_len = out_len;

    int in_len;
    memcpy(scratch, wire, (size_t)out_len);
    uint8_t *res = transform_inbound(scratch, out_len, cfg, &in_len);
    if (!res) return -1;
    memcpy(back, res, (size_t)in_len);
    return in_len;
}

/* The recovered message must equal what a plain WG stack produced. */
static int hp_check_back(const uint8_t *back, int in_len, uint32_t wg_type, int msg_len) {
    if (in_len != msg_len) return 0;
    if (read32_le(back) != wg_type) return 0;
    for (int i = 4; i < msg_len; i++)
        if (back[i] != (uint8_t)(i - 4)) return 0;
    return 1;
}

static void test_hp_roundtrip_init(void) {
    awg_config_t cfg = make_v3_config();
    uint8_t wire[AWG_PACKET_BUF_SIZE], back[AWG_PACKET_BUF_SIZE];
    int wire_len;
    int in_len = hp_roundtrip(&cfg, WG_HANDSHAKE_INIT, WG_INIT_SIZE, cfg.s1,
                              wire, &wire_len, back);
    ASSERT(hp_check_back(back, in_len, WG_HANDSHAKE_INIT, WG_INIT_SIZE));
}

/* --- Reference receiver -------------------------------------------------
 *
 * A transcription of what amneziawg-go does to an incoming datagram
 * (device/receive.go:144-177 plus DeterminePacketTypeAndPadding), so the tests
 * can assert that what we put on the wire is what the reference would accept —
 * no server, no sockets, no Docker. It is written from the reference's own
 * structure rather than by calling our transform_inbound: a round-trip through
 * our own code would still pass if both directions shared the same wrong idea
 * of the format.
 *
 * Candidate order and the exact/`>=` distinction are the reference's, and so is
 * the rule that the type field is XORed with typeHash (the keystream of four
 * zero bytes) while transport decrypts only the 16-byte header and handshakes
 * decrypt the whole message. */
typedef struct {
    uint32_t type;
    int padding;
    uint8_t msg[AWG_PACKET_BUF_SIZE];
    int msg_len;
} ref_rx_t;

static int ref_receive(const awg_config_t *cfg, const awg_profile_t *pr,
                       const uint8_t *pkt, int len, ref_rx_t *out) {
    uint32_t type_hash = 0;
    if (pr->hp_on) {
        uint8_t ks[CHACHA20_BLOCK_SIZE];
        chacha20_block(cfg->hp_key, pkt, 0, ks);
        type_hash = read32_le(ks);
    }

    const struct {
        int pad, fixed, exact;
        const hrange_t *h;
        uint32_t type;
    } cand[4] = {
        { pr->s1, WG_INIT_SIZE,          1, &pr->h1, WG_HANDSHAKE_INIT },
        { pr->s2, WG_RESP_SIZE,          1, &pr->h2, WG_HANDSHAKE_RESPONSE },
        { pr->s3, WG_COOKIE_SIZE,        1, &pr->h3, WG_COOKIE_REPLY },
        { pr->s4, AWG_HP_TRANSPORT_HDR,  0, &pr->h4, WG_TRANSPORT_DATA },
    };

    for (int i = 0; i < 4; i++) {
        int fits = cand[i].exact ? (len == cand[i].pad + cand[i].fixed)
                                 : (len >= cand[i].pad + cand[i].fixed);
        if (!fits) continue;
        uint32_t h = read32_le(pkt + cand[i].pad) ^ type_hash;
        if (!hrange_contains(cand[i].h, h)) continue;

        out->type = cand[i].type;
        out->padding = cand[i].pad;
        out->msg_len = len - cand[i].pad;
        memcpy(out->msg, pkt + cand[i].pad, (size_t)out->msg_len);
        if (pr->hp_on) {
            int n = (cand[i].type == WG_TRANSPORT_DATA)
                        ? AWG_HP_TRANSPORT_HDR : out->msg_len;
            chacha20_xor(cfg->hp_key, pkt, out->msg, n);
        }
        return 0;
    }
    return -1;
}

/* Send one message of each kind through transform_outbound and require the
 * reference receiver to classify it correctly and recover the payload. */
static void ref_accepts_all_types(awg_config_t *cfg) {
    static const struct { uint32_t type; int len; } kinds[4] = {
        { WG_HANDSHAKE_INIT,     WG_INIT_SIZE },
        { WG_HANDSHAKE_RESPONSE, WG_RESP_SIZE },
        { WG_COOKIE_REPLY,       WG_COOKIE_SIZE },
        { WG_TRANSPORT_DATA,     64 },
    };
    const awg_profile_t *pr = config_active_profile(cfg);
    fastrand_t rng;
    fastrand_init(&rng, 0x0BADC0DE12345678ull);

    for (int k = 0; k < 4; k++) {
        uint8_t buf[AWG_PACKET_BUF_SIZE];
        int dataoff = AWG_PACKET_HEADROOM;
        uint8_t *data = buf + dataoff;

        /* Distinct body per kind so a mixed-up recovery cannot pass. */
        for (int i = 0; i < kinds[k].len; i++) data[i] = (uint8_t)(i + k * 31);
        write32_le(data, kinds[k].type);
        uint8_t expect[AWG_PACKET_BUF_SIZE];
        memcpy(expect, data, (size_t)kinds[k].len);

        int out_len = 0, sendJunk = 0;
        uint8_t *wire = transform_outbound(buf, dataoff, kinds[k].len, cfg,
                                           fastrand_u64(&rng), &out_len, &sendJunk);
        ASSERT(wire != NULL);

        ref_rx_t rx;
        memset(&rx, 0, sizeof(rx));
        ASSERT_EQ(ref_receive(cfg, pr, wire, out_len, &rx), 0);
        ASSERT_EQ((int)rx.type, (int)kinds[k].type);
        ASSERT_EQ(rx.msg_len, kinds[k].len);
        /* Bytes past the type field must survive untouched; the type itself is
         * deliberately rewritten to the profile's H value. */
        ASSERT_MEM_EQ(rx.msg + 4, expect + 4, (size_t)(kinds[k].len - 4));
    }
}

static awg_config_t make_gen_config(int s1, int s2, int s3, int s4, int ranged, int hp) {
    awg_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.jc = 2; cfg.jmin = 30; cfg.jmax = 200;
    cfg.s1 = s1; cfg.s2 = s2; cfg.s3 = s3; cfg.s4 = s4;
    if (ranged) {
        cfg.h1 = (hrange_t){1000000, 1000050};
        cfg.h2 = (hrange_t){2000000, 2000050};
        cfg.h3 = (hrange_t){3000000, 3000050};
        cfg.h4 = (hrange_t){4000000, 4000050};
    } else {
        cfg.h1 = (hrange_t){1000000, 1000000};
        cfg.h2 = (hrange_t){2000000, 2000000};
        cfg.h3 = (hrange_t){3000000, 3000000};
        cfg.h4 = (hrange_t){4000000, 4000000};
    }
    if (hp) {
        for (int i = 0; i < 32; i++) cfg.hp_key[i] = (uint8_t)(i * 5 + 3);
        cfg.hp_on = 1;
    }
    config_compute(&cfg);
    return cfg;
}

/* v1: fixed H, no S3/S4, no CPS. */
static void test_reference_accepts_v1(void) {
    awg_config_t cfg = make_gen_config(15, 20, 0, 0, 0, 0);
    ref_accepts_all_types(&cfg);
}

/* v1.5: v1 plus CPS templates. CPS packets are separate datagrams, so the
 * framing of real messages must be byte-identical to v1. */
static void test_reference_accepts_v1_5(void) {
    awg_config_t cfg = make_gen_config(15, 20, 0, 0, 0, 0);
    static cps_template_t t;
    ASSERT_EQ(cps_parse("<b 0xf1e2><r 8>", &t), 0);
    cfg.cps[0] = &t;
    config_compute(&cfg);
    ref_accepts_all_types(&cfg);
}

/* v2: H as ranges plus non-zero S3/S4. */
static void test_reference_accepts_v2(void) {
    awg_config_t cfg = make_gen_config(77, 41, 33, 14, 1, 0);
    ref_accepts_all_types(&cfg);
}

/* v3: same as v2 plus header protection. */
static void test_reference_accepts_v3(void) {
    awg_config_t cfg = make_gen_config(77, 41, 33, 14, 1, 1);
    ref_accepts_all_types(&cfg);
}

/* Cross-check that the reference receiver is discriminating, not permissive:
 * the same packets must be rejected when the key is wrong. */
static void test_reference_rejects_wrong_hp_key(void) {
    awg_config_t cfg = make_gen_config(77, 41, 33, 14, 1, 1);
    awg_config_t other = cfg;
    for (int i = 0; i < 32; i++) other.hp_key[i] = (uint8_t)(i * 11 + 9);
    config_compute(&other);

    uint8_t buf[AWG_PACKET_BUF_SIZE];
    int dataoff = AWG_PACKET_HEADROOM;
    uint8_t *data = buf + dataoff;
    memset(data, 0x5A, WG_INIT_SIZE);
    write32_le(data, WG_HANDSHAKE_INIT);

    int out_len = 0, sendJunk = 0;
    uint8_t *wire = transform_outbound(buf, dataoff, WG_INIT_SIZE, &cfg,
                                       0x1122334455667788ull, &out_len, &sendJunk);
    ASSERT(wire != NULL);

    ref_rx_t rx;
    memset(&rx, 0, sizeof(rx));
    ASSERT_EQ(ref_receive(&other, config_active_profile(&other), wire, out_len, &rx), -1);
}

/* The S padding doubles as the ChaCha20 nonce under v3, and callers pass their
 * live PRNG state as rand_val. Seeding the padding generator with that value
 * directly made it a byte-exact clone of the caller's stream: the handshake
 * nonce reappeared as the next transport packet's nonce (same key, same nonce,
 * same counter), and the junk packet ahead of an init shared its first 8 bytes
 * with the init. Both must stay decorrelated. */
static void test_hp_padding_is_not_a_clone_of_caller_prng(void) {
    awg_config_t cfg = make_v3_config();
    fastrand_t rng;
    fastrand_init(&rng, 0x1234567890ABCDEFull);

    uint8_t buf[AWG_PACKET_BUF_SIZE];
    int dataoff = AWG_PACKET_HEADROOM;
    uint8_t *data = buf + dataoff;
    memset(data, 0xAA, WG_INIT_SIZE);
    write32_le(data, WG_HANDSHAKE_INIT);

    uint64_t rand_val = fastrand_u64(&rng);
    int out_len = 0, sendJunk = 0;
    uint8_t *out = transform_outbound(buf, dataoff, WG_INIT_SIZE, &cfg,
                                      rand_val, &out_len, &sendJunk);
    ASSERT(out != NULL);

    /* What the caller's generator produces next — the padding of the packet
     * that follows, and the junk bytes that precede this one. */
    uint8_t next[CHACHA20_NONCE_SIZE];
    fastrand_fill(&rng, next, sizeof(next));

    ASSERT(memcmp(out, next, CHACHA20_NONCE_SIZE) != 0);
}

/* The padding is the one field an observer sees raw, so it must not be the
 * output of a generator whose state it reveals. xorshift64 was exactly that:
 * the second 8 bytes are one step of the generator applied to the first 8, so
 * a censor confirmed awg-proxy from a single packet. */
static void test_padding_is_not_a_prng_chain(void) {
    awg_config_t cfg = make_v3_config();
    fastrand_t rng;
    fastrand_init(&rng, 0x5EED5EED5EED5EEDull);

    for (int i = 0; i < 16; i++) {
        uint8_t buf[AWG_PACKET_BUF_SIZE];
        int dataoff = AWG_PACKET_HEADROOM;
        uint8_t *data = buf + dataoff;
        memset(data, 0xAA, WG_INIT_SIZE);
        write32_le(data, WG_HANDSHAKE_INIT);

        int out_len = 0, sendJunk = 0;
        uint8_t *out = transform_outbound(buf, dataoff, WG_INIT_SIZE, &cfg,
                                          fastrand_u64(&rng), &out_len, &sendJunk);
        ASSERT(out != NULL);
        ASSERT(cfg.s1 >= 16);

        uint64_t first;
        memcpy(&first, out, 8);
        uint64_t step = first;
        step ^= step << 13;
        step ^= step >> 7;
        step ^= step << 17;
        ASSERT(memcmp(out + 8, &step, 8) != 0);
    }
}

/* When the caller's headroom is smaller than the padding, the transform falls
 * back to a buffer shared by the whole thread. Two packets in a row therefore
 * land on the same address and the first one's bytes are gone — which is why a
 * caller that queues packets has to send such a packet immediately. The
 * predicate is what proxy.c keys that decision on, so pin both halves. */
static void test_shared_buf_is_reported_and_reused(void) {
    awg_config_t cfg = make_v3_config();
    ASSERT(cfg.s1 > 8);

    uint8_t buf_a[AWG_PACKET_BUF_SIZE + AWG_PACKET_HEADROOM];
    uint8_t buf_b[AWG_PACKET_BUF_SIZE + AWG_PACKET_HEADROOM];
    int dataoff = 4;                       /* below S1: forces the fallback */
    ASSERT(dataoff < cfg.s1);

    memset(buf_a + dataoff, 0xA1, WG_INIT_SIZE);
    write32_le(buf_a + dataoff, WG_HANDSHAKE_INIT);
    memset(buf_b + dataoff, 0xB2, WG_INIT_SIZE);
    write32_le(buf_b + dataoff, WG_HANDSHAKE_INIT);

    int len_a = 0, len_b = 0, junk = 0;
    uint8_t *out_a = transform_outbound(buf_a, dataoff, WG_INIT_SIZE, &cfg,
                                        0x1111ull, &len_a, &junk);
    ASSERT(out_a != NULL);
    ASSERT(transform_is_shared_buf(out_a));
    ASSERT_EQ(len_a, cfg.s1 + WG_INIT_SIZE);

    uint8_t first[AWG_PACKET_BUF_SIZE];
    memcpy(first, out_a, (size_t)len_a);

    uint8_t *out_b = transform_outbound(buf_b, dataoff, WG_INIT_SIZE, &cfg,
                                        0x2222ull, &len_b, &junk);
    ASSERT(out_b != NULL);
    ASSERT(transform_is_shared_buf(out_b));
    ASSERT(out_a == out_b);
    ASSERT(memcmp(first, out_a, (size_t)len_a) != 0);

    /* With enough headroom the packet stays in its own buffer, so the batching
     * path is not disturbed for the common case. */
    uint8_t buf_c[AWG_PACKET_BUF_SIZE + AWG_PACKET_HEADROOM];
    int roomy = AWG_PACKET_HEADROOM;
    memset(buf_c + roomy, 0xC3, WG_INIT_SIZE);
    write32_le(buf_c + roomy, WG_HANDSHAKE_INIT);
    int len_c = 0;
    uint8_t *out_c = transform_outbound(buf_c, roomy, WG_INIT_SIZE, &cfg,
                                        0x3333ull, &len_c, &junk);
    ASSERT(out_c != NULL);
    ASSERT(!transform_is_shared_buf(out_c));
}

/* Two packets drawn from the same generator must never share a nonce. */
static void test_hp_nonce_differs_between_packets(void) {
    awg_config_t cfg = make_v3_config();
    fastrand_t rng;
    fastrand_init(&rng, 0xFEEDFACECAFEBEEFull);

    uint8_t first[CHACHA20_NONCE_SIZE];
    for (int i = 0; i < 64; i++) {
        uint8_t buf[AWG_PACKET_BUF_SIZE];
        int dataoff = AWG_PACKET_HEADROOM;
        uint8_t *data = buf + dataoff;
        memset(data, 0xAA, WG_INIT_SIZE);
        write32_le(data, WG_HANDSHAKE_INIT);

        int out_len = 0, sendJunk = 0;
        uint8_t *out = transform_outbound(buf, dataoff, WG_INIT_SIZE, &cfg,
                                          fastrand_u64(&rng), &out_len, &sendJunk);
        ASSERT(out != NULL);
        if (i == 0) memcpy(first, out, CHACHA20_NONCE_SIZE);
        else ASSERT(memcmp(out, first, CHACHA20_NONCE_SIZE) != 0);
    }
}

static void test_hp_roundtrip_response(void) {
    awg_config_t cfg = make_v3_config();
    uint8_t wire[AWG_PACKET_BUF_SIZE], back[AWG_PACKET_BUF_SIZE];
    int wire_len;
    int in_len = hp_roundtrip(&cfg, WG_HANDSHAKE_RESPONSE, WG_RESP_SIZE, cfg.s2,
                              wire, &wire_len, back);
    ASSERT(hp_check_back(back, in_len, WG_HANDSHAKE_RESPONSE, WG_RESP_SIZE));
}

static void test_hp_roundtrip_cookie(void) {
    awg_config_t cfg = make_v3_config();
    uint8_t wire[AWG_PACKET_BUF_SIZE], back[AWG_PACKET_BUF_SIZE];
    int wire_len;
    int in_len = hp_roundtrip(&cfg, WG_COOKIE_REPLY, WG_COOKIE_SIZE, cfg.s3,
                              wire, &wire_len, back);
    ASSERT(hp_check_back(back, in_len, WG_COOKIE_REPLY, WG_COOKIE_SIZE));
}

static void test_hp_roundtrip_transport(void) {
    awg_config_t cfg = make_v3_config();
    uint8_t wire[AWG_PACKET_BUF_SIZE], back[AWG_PACKET_BUF_SIZE];
    int wire_len;
    /* 200 bytes: unambiguous against the handshake sizes of this config */
    int in_len = hp_roundtrip(&cfg, WG_TRANSPORT_DATA, 200, cfg.s4,
                              wire, &wire_len, back);
    ASSERT(hp_check_back(back, in_len, WG_TRANSPORT_DATA, 200));
}

/* Only Type|Receiver|Counter is encrypted on a transport packet — the AEAD
 * body must go out untouched. */
static void test_hp_transport_encrypts_header_only(void) {
    awg_config_t cfg = make_v3_config();
    uint8_t buf[AWG_PACKET_HEADROOM + 200];
    int dataoff = AWG_PACKET_HEADROOM;
    uint8_t *data = buf + dataoff;

    memset(buf, 0, sizeof(buf));
    write32_le(data, WG_TRANSPORT_DATA);
    fill_seq(data + 4, 200 - 4);

    int out_len, sendJunk;
    uint8_t *out = transform_outbound(buf, dataoff, 200, &cfg, 0xC0FFEE, &out_len, &sendJunk);
    ASSERT_EQ(out_len, cfg.s4 + 200);

    const uint8_t *msg = out + cfg.s4;
    /* Bytes 16.. are the AEAD body: unchanged sequence bytes */
    for (int i = AWG_HP_TRANSPORT_HDR; i < 200; i++)
        ASSERT_EQ(msg[i], (uint8_t)(i - 4));
    /* The plaintext H4 must not be visible in the clear */
    ASSERT(read32_le(msg) != cfg.h4.min);
}

/* On the wire the type is masked; only after XOR with the keystream does it
 * land in the configured H range. */
static void test_hp_inbound_type_detection(void) {
    awg_config_t cfg = make_v3_config();
    struct { uint32_t type; int len; int pad; hrange_t *h; } cases[] = {
        { WG_HANDSHAKE_INIT,     WG_INIT_SIZE,   cfg.s1, &cfg.h1 },
        { WG_HANDSHAKE_RESPONSE, WG_RESP_SIZE,   cfg.s2, &cfg.h2 },
        { WG_COOKIE_REPLY,       WG_COOKIE_SIZE, cfg.s3, &cfg.h3 },
        { WG_TRANSPORT_DATA,     200,            cfg.s4, &cfg.h4 },
    };

    for (int c = 0; c < 4; c++) {
        uint8_t wire[AWG_PACKET_BUF_SIZE], back[AWG_PACKET_BUF_SIZE];
        int wire_len;
        int in_len = hp_roundtrip(&cfg, cases[c].type, cases[c].len, cases[c].pad,
                                  wire, &wire_len, back);
        ASSERT(hp_check_back(back, in_len, cases[c].type, cases[c].len));

        /* Re-derive the mask the receiver uses and confirm it is what turns
         * the on-wire bytes into the H value. */
        uint8_t ks[CHACHA20_BLOCK_SIZE];
        chacha20_block(cfg.hp_key, wire, 0, ks);
        uint32_t onwire = read32_le(wire + cases[c].pad);
        ASSERT(!hrange_contains(cases[c].h, onwire));
        ASSERT(hrange_contains(cases[c].h, onwire ^ read32_le(ks)));
    }
}

/* An hp profile with any S below 12 must be rejected: the nonce would run off
 * the padding into the message. */
static void test_hp_validate_rejects_s_below_12(void) {
    const char *err;
    int *fields[4];

    for (int f = 0; f < 4; f++) {
        awg_profile_t pr;
        memset(&pr, 0, sizeof(pr));
        pr.s1 = pr.s2 = pr.s3 = pr.s4 = 16;
        pr.h1 = (hrange_t){10, 10};
        pr.h2 = (hrange_t){11, 11};
        pr.h3 = (hrange_t){12, 12};
        pr.h4 = (hrange_t){13, 13};
        pr.hp_on = 1;
        fields[0] = &pr.s1; fields[1] = &pr.s2; fields[2] = &pr.s3; fields[3] = &pr.s4;

        err = NULL;
        ASSERT_EQ(config_validate_profile(&pr, &err), 0);

        *fields[f] = 11;
        err = NULL;
        ASSERT_EQ(config_validate_profile(&pr, &err), -1);
        ASSERT(err != NULL);

        /* Same padding is fine without header protection */
        pr.hp_on = 0;
        err = NULL;
        ASSERT_EQ(config_validate_profile(&pr, &err), 0);
    }
}

/* No key => byte-for-byte identical to the v2 path, in both directions. This
 * is the regression guard for everyone who stays on 2.0. */
static void test_hp_off_matches_v2(void) {
    awg_config_t v2 = make_v3_config();
    memset(v2.hp_key, 0, 32);
    v2.hp_on = 0;
    config_compute(&v2);

    /* Same config, key present in the struct but the profile has hp off */
    awg_config_t keyed = make_v3_config();
    keyed.hp_on = 0;
    config_compute(&keyed);
    ASSERT_EQ(keyed.hp_key_set, 1);
    ASSERT_EQ(keyed.profiles[0].hp_on, 0);

    struct { uint32_t type; int len; } cases[] = {
        { WG_HANDSHAKE_INIT,     WG_INIT_SIZE   },
        { WG_HANDSHAKE_RESPONSE, WG_RESP_SIZE   },
        { WG_COOKIE_REPLY,       WG_COOKIE_SIZE },
        { WG_TRANSPORT_DATA,     200            },
    };

    for (int c = 0; c < 4; c++) {
        uint8_t a[AWG_PACKET_HEADROOM + AWG_PACKET_BUF_SIZE];
        uint8_t b[AWG_PACKET_HEADROOM + AWG_PACKET_BUF_SIZE];
        int dataoff = AWG_PACKET_HEADROOM;
        memset(a, 0, sizeof(a));
        memset(b, 0, sizeof(b));
        write32_le(a + dataoff, cases[c].type);
        fill_seq(a + dataoff + 4, cases[c].len - 4);
        memcpy(b, a, sizeof(a));

        int la, lb, ja, jb;
        uint8_t *oa = transform_outbound(a, dataoff, cases[c].len, &v2, 777, &la, &ja);
        uint8_t *ob = transform_outbound(b, dataoff, cases[c].len, &keyed, 777, &lb, &jb);
        ASSERT_EQ(la, lb);
        ASSERT_EQ(ja, jb);
        /* The padding itself is random bytes, so only the message after it can
         * be compared — that is where a stray ChaCha20 pass would show up. */
        int pad = la - cases[c].len;
        ASSERT_MEM_EQ(oa + pad, ob + pad, (size_t)cases[c].len);

        /* And inbound agrees too */
        int ia, ib;
        uint8_t *ra = transform_inbound(oa, la, &v2, &ia);
        uint8_t *rb = transform_inbound(ob, lb, &keyed, &ib);
        ASSERT(ra != NULL && rb != NULL);
        ASSERT_EQ(ia, ib);
        ASSERT_MEM_EQ(ra, rb, (size_t)ia);
    }
}

static int cmp_u64(const void *a, const void *b) {
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return (x > y) - (x < y);
}

/* Every packet must carry a fresh nonce: a repeat leaks the XOR of two
 * plaintext headers, and Receiver is constant within a session. */
static void test_hp_nonce_uniqueness(void) {
    awg_config_t cfg = make_v3_config();
    enum { N = 10000 };
    static uint64_t seen[N];
    fastrand_t rng;
    fastrand_init(&rng, 0x5EED);

    for (int i = 0; i < N; i++) {
        uint8_t buf[AWG_PACKET_HEADROOM + 200];
        int dataoff = AWG_PACKET_HEADROOM;
        memset(buf, 0, sizeof(buf));
        write32_le(buf + dataoff, WG_TRANSPORT_DATA);

        int out_len, sendJunk;
        uint8_t *out = transform_outbound(buf, dataoff, 200, &cfg,
                                          fastrand_u64(&rng), &out_len, &sendJunk);
        memcpy(&seen[i], out, 8);
    }

    qsort(seen, N, sizeof(seen[0]), cmp_u64);
    for (int i = 1; i < N; i++)
        ASSERT(seen[i] != seen[i - 1]);
}

/* ---- Fallback chain of four profiles ---- */

/* A packet crafted for any stage decodes under that stage and no other. */
static void test_multi_profile_switch(void) {
    awg_config_t cfg = make_v3_config(); /* stage 0 = v3 */
    ASSERT_EQ(cfg.profile_count, 1);

    /* stage 1: v2 — ranges, S3/S4, no header protection */
    awg_profile_t *p1 = &cfg.profiles[1];
    memset(p1, 0, sizeof(*p1));
    p1->s1 = 40; p1->s2 = 35; p1->s3 = 22; p1->s4 = 8;
    p1->h1 = (hrange_t){2000000000, 2000000100};
    p1->h2 = (hrange_t){2000000200, 2000000300};
    p1->h3 = (hrange_t){2000000400, 2000000500};
    p1->h4 = (hrange_t){2000000600, 2000000700};
    config_compute_profile(p1);

    /* stage 2: v1.5 — fixed H, no S3/S4 (CPS is send-only, so not modelled) */
    awg_profile_t *p2 = &cfg.profiles[2];
    memset(p2, 0, sizeof(*p2));
    p2->s1 = 55; p2->s2 = 45;
    p2->h1 = (hrange_t){1500000001, 1500000001};
    p2->h2 = (hrange_t){1500000002, 1500000002};
    p2->h3 = (hrange_t){1500000003, 1500000003};
    p2->h4 = (hrange_t){1500000004, 1500000004};
    config_compute_profile(p2);

    /* stage 3: v1 — identity H4 */
    awg_profile_t *p3 = &cfg.profiles[3];
    memset(p3, 0, sizeof(*p3));
    p3->s1 = 70; p3->s2 = 65;
    p3->h1 = (hrange_t){1100000001, 1100000001};
    p3->h2 = (hrange_t){1100000002, 1100000002};
    p3->h3 = (hrange_t){1100000003, 1100000003};
    p3->h4 = (hrange_t){WG_TRANSPORT_DATA, WG_TRANSPORT_DATA};
    config_compute_profile(p3);

    cfg.profile_count = 4;
    config_compute_max_s4(&cfg);
    ASSERT_EQ(cfg.max_s4, 14); /* v3 stage has the largest S4 */

    for (int stage = 0; stage < 4; stage++) {
        /* Build an init the way stage `stage` would send it */
        awg_config_t send = cfg;
        config_apply_profile(&send, stage);

        uint8_t buf[AWG_PACKET_HEADROOM + AWG_PACKET_BUF_SIZE];
        int dataoff = AWG_PACKET_HEADROOM;
        memset(buf, 0, sizeof(buf));
        write32_le(buf + dataoff, WG_HANDSHAKE_INIT);
        fill_seq(buf + dataoff + 4, WG_INIT_SIZE - 4);

        int out_len, sendJunk;
        uint8_t *out = transform_outbound(buf, dataoff, WG_INIT_SIZE, &send,
                                          0x1234 + stage, &out_len, &sendJunk);
        ASSERT_EQ(out_len, cfg.profiles[stage].s1 + WG_INIT_SIZE);

        /* Walk the chain the way c2s_thread_reverse does */
        int matched = -1;
        for (int k = 0; k < cfg.profile_count; k++) {
            uint8_t wire[AWG_PACKET_BUF_SIZE];
            awg_hp_ks_t ks = { .valid = 0 };
            int in_len;
            memcpy(wire, out, (size_t)out_len);
            uint8_t *res = transform_inbound_profile(wire, out_len, &cfg,
                                                     &cfg.profiles[k], &ks, &in_len);
            if (!res) continue;
            ASSERT_EQ(matched, -1); /* exactly one stage may claim it */
            matched = k;
            ASSERT_EQ(in_len, WG_INIT_SIZE);
            ASSERT_EQ(read32_le(res), (uint32_t)WG_HANDSHAKE_INIT);
        }
        ASSERT_EQ(matched, stage);
    }
}

/* ---- AWG 3.1: random trailers and disabled cookies ---- */

static awg_config_t make_v31_config(void) {
    awg_config_t cfg = make_v3_config();
    cfg.rt = 1;
    config_compute(&cfg);
    return cfg;
}

/* Outbound: every handshake carries a tail, and the packet never grows past
 * the trailer window. */
static void test_rt_outbound_appends_trailer(void) {
    awg_config_t cfg = make_v31_config();
    uint8_t buf[AWG_PACKET_HEADROOM + AWG_PACKET_BUF_SIZE];
    int dataoff = AWG_PACKET_HEADROOM;
    int longest = 0;

    for (int i = 0; i < 64; i++) {
        memset(buf, 0, sizeof(buf));
        uint8_t *data = buf + dataoff;
        write32_le(data, WG_HANDSHAKE_INIT);
        fill_seq(data + 4, WG_INIT_SIZE - 4);

        int out_len, sendJunk;
        uint8_t *out = transform_outbound(buf, dataoff, WG_INIT_SIZE, &cfg,
                                          0x1000 + (uint64_t)i, &out_len, &sendJunk);
        ASSERT(out != NULL);
        ASSERT(out_len >= cfg.init_total);
        ASSERT(out_len < AWG_DEFAULT_UDP_WINDOW);
        if (out_len > longest) longest = out_len;
    }
    ASSERT(longest > cfg.init_total); /* the tail is not always empty */
}

/* Inbound: a handshake with a tail decodes, and the tail is cut off. */
static void test_rt_inbound_accepts_trailer(void) {
    awg_config_t cfg = make_v31_config();
    uint8_t wire[AWG_PACKET_BUF_SIZE], scratch[AWG_PACKET_BUF_SIZE];
    uint8_t buf[AWG_PACKET_HEADROOM + AWG_PACKET_BUF_SIZE];
    int dataoff = AWG_PACKET_HEADROOM;

    static const struct { uint32_t type; int len; } msgs[] = {
        { WG_HANDSHAKE_INIT,     WG_INIT_SIZE   },
        { WG_HANDSHAKE_RESPONSE, WG_RESP_SIZE   },
        { WG_COOKIE_REPLY,       WG_COOKIE_SIZE },
    };

    for (unsigned m = 0; m < sizeof(msgs) / sizeof(msgs[0]); m++) {
        memset(buf, 0, sizeof(buf));
        uint8_t *data = buf + dataoff;
        write32_le(data, msgs[m].type);
        fill_seq(data + 4, msgs[m].len - 4);

        int out_len, sendJunk;
        uint8_t *out = transform_outbound(buf, dataoff, msgs[m].len, &cfg,
                                          0xBEEF + m, &out_len, &sendJunk);
        ASSERT(out != NULL);
        memcpy(wire, out, (size_t)out_len);

        int in_len;
        memcpy(scratch, wire, (size_t)out_len);
        uint8_t *res = transform_inbound(scratch, out_len, &cfg, &in_len);
        ASSERT(res != NULL);
        ASSERT_EQ(in_len, msgs[m].len);
        ASSERT_EQ(read32_le(res), msgs[m].type);
        for (int i = 4; i < msgs[m].len; i++)
            ASSERT_EQ(res[i], (uint8_t)(i - 4));
    }
}

/* A peer that did not enable the feature measures sizes exactly, so the same
 * padded init is not a handshake to it. */
static void test_rt_inbound_rejects_trailer_when_off(void) {
    awg_config_t cfg = make_v31_config();
    uint8_t buf[AWG_PACKET_HEADROOM + AWG_PACKET_BUF_SIZE];
    uint8_t scratch[AWG_PACKET_BUF_SIZE];
    int dataoff = AWG_PACKET_HEADROOM;
    int out_len = 0, sendJunk, padded = 0;
    uint8_t *out = NULL;

    for (uint64_t seed = 1; seed < 64 && !padded; seed++) {
        memset(buf, 0, sizeof(buf));
        uint8_t *data = buf + dataoff;
        write32_le(data, WG_HANDSHAKE_INIT);
        fill_seq(data + 4, WG_INIT_SIZE - 4);
        out = transform_outbound(buf, dataoff, WG_INIT_SIZE, &cfg, seed,
                                 &out_len, &sendJunk);
        padded = out_len > cfg.init_total;
    }
    ASSERT(padded);

    awg_config_t off = make_v3_config(); /* same profile, trailers disabled */
    int in_len;
    memcpy(scratch, out, (size_t)out_len);
    ASSERT(transform_inbound(scratch, out_len, &off, &in_len) == NULL);
}

/* The window follows the largest datagram seen, and trailers grow with it. */
static void test_rt_window_grows_with_traffic(void) {
    awg_config_t cfg = make_v31_config();
    uint8_t buf[AWG_PACKET_HEADROOM + AWG_PACKET_BUF_SIZE];
    int dataoff = AWG_PACKET_HEADROOM;
    int longest = 0;

    awg_window_note(&cfg, 1200);

    for (int i = 0; i < 64; i++) {
        memset(buf, 0, sizeof(buf));
        uint8_t *data = buf + dataoff;
        write32_le(data, WG_HANDSHAKE_INIT);
        fill_seq(data + 4, WG_INIT_SIZE - 4);

        int out_len, sendJunk;
        uint8_t *out = transform_outbound(buf, dataoff, WG_INIT_SIZE, &cfg,
                                          0x2000 + (uint64_t)i, &out_len, &sendJunk);
        ASSERT(out != NULL);
        ASSERT(out_len <= 1200);
        if (out_len > longest) longest = out_len;
    }
    ASSERT(longest > AWG_DEFAULT_UDP_WINDOW);
}

/* Even at the largest padding the config allows, padding + message + trailer
 * stays inside the packet buffer. */
static void test_rt_trailer_never_overflows_buffer(void) {
    awg_config_t cfg = make_v31_config();
    cfg.s1 = AWG_PACKET_BUF_SIZE - WG_INIT_SIZE; /* init_total == 1500 */
    config_compute(&cfg);
    awg_window_note(&cfg, 9000); /* absurd window: must still be capped */

    uint8_t buf[AWG_PACKET_HEADROOM + AWG_PACKET_BUF_SIZE];
    int dataoff = AWG_PACKET_HEADROOM;
    for (int i = 0; i < 16; i++) {
        memset(buf, 0, sizeof(buf));
        uint8_t *data = buf + dataoff;
        write32_le(data, WG_HANDSHAKE_INIT);
        fill_seq(data + 4, WG_INIT_SIZE - 4);

        int out_len, sendJunk;
        uint8_t *out = transform_outbound(buf, dataoff, WG_INIT_SIZE, &cfg,
                                          0x3000 + (uint64_t)i, &out_len, &sendJunk);
        ASSERT(out != NULL);
        ASSERT_EQ(out_len, AWG_PACKET_BUF_SIZE);
    }
}

/* DisableCookies: the cookie reply is dropped, everything else still flows. */
static void test_disable_cookies_drops_cookie_only(void) {
    awg_config_t cfg = make_v3_config();
    cfg.disable_cookies = 1;
    config_compute(&cfg);

    uint8_t buf[AWG_PACKET_HEADROOM + AWG_PACKET_BUF_SIZE];
    int dataoff = AWG_PACKET_HEADROOM;
    int out_len, sendJunk;

    memset(buf, 0, sizeof(buf));
    write32_le(buf + dataoff, WG_COOKIE_REPLY);
    fill_seq(buf + dataoff + 4, WG_COOKIE_SIZE - 4);
    ASSERT(transform_outbound(buf, dataoff, WG_COOKIE_SIZE, &cfg, 7,
                              &out_len, &sendJunk) == NULL);
    ASSERT_EQ(out_len, 0);

    memset(buf, 0, sizeof(buf));
    write32_le(buf + dataoff, WG_HANDSHAKE_INIT);
    fill_seq(buf + dataoff + 4, WG_INIT_SIZE - 4);
    ASSERT(transform_outbound(buf, dataoff, WG_INIT_SIZE, &cfg, 7,
                              &out_len, &sendJunk) != NULL);
    ASSERT_EQ(out_len, cfg.init_total);

    memset(buf, 0, sizeof(buf));
    write32_le(buf + dataoff, WG_TRANSPORT_DATA);
    fill_seq(buf + dataoff + 4, 196);
    ASSERT(transform_outbound(buf, dataoff, 200, &cfg, 7,
                              &out_len, &sendJunk) != NULL);
    ASSERT_EQ(out_len, cfg.s4 + 200);
}

/* A cookie still goes out untouched when the feature is off. */
static void test_cookies_pass_when_enabled(void) {
    awg_config_t cfg = make_v3_config();
    uint8_t buf[AWG_PACKET_HEADROOM + AWG_PACKET_BUF_SIZE];
    int dataoff = AWG_PACKET_HEADROOM;
    int out_len, sendJunk;

    memset(buf, 0, sizeof(buf));
    write32_le(buf + dataoff, WG_COOKIE_REPLY);
    fill_seq(buf + dataoff + 4, WG_COOKIE_SIZE - 4);
    uint8_t *out = transform_outbound(buf, dataoff, WG_COOKIE_SIZE, &cfg, 7,
                                      &out_len, &sendJunk);
    ASSERT(out != NULL);
    ASSERT_EQ(out_len, cfg.cookie_total);
}

/* Trailers are per profile: a fallback stage that speaks an older version must
 * not inherit them. */
static void test_rt_is_per_profile(void) {
    awg_config_t cfg = make_v31_config();
    cfg.profiles[1] = cfg.profiles[0];
    cfg.profiles[1].rt = 0;
    config_compute_profile(&cfg.profiles[1]);
    cfg.profile_count = 2;

    uint8_t buf[AWG_PACKET_HEADROOM + AWG_PACKET_BUF_SIZE];
    int dataoff = AWG_PACKET_HEADROOM;
    int out_len, sendJunk;

    for (int i = 0; i < 32; i++) {
        memset(buf, 0, sizeof(buf));
        write32_le(buf + dataoff, WG_HANDSHAKE_INIT);
        fill_seq(buf + dataoff + 4, WG_INIT_SIZE - 4);
        uint8_t *out = transform_outbound_profile(buf, dataoff, WG_INIT_SIZE, &cfg,
                                                  &cfg.profiles[1], NULL,
                                                  0x4000 + (uint64_t)i,
                                                  &out_len, &sendJunk);
        ASSERT(out != NULL);
        ASSERT_EQ(out_len, cfg.profiles[1].init_total);
    }
}

int main(void) {
    fprintf(stderr, "=== transform tests ===\n");
    RUN_TEST(outbound_handshake_init);
    RUN_TEST(outbound_handshake_response);
    RUN_TEST(outbound_cookie_reply);
    RUN_TEST(outbound_transport_data);
    RUN_TEST(inbound_handshake_init);
    RUN_TEST(inbound_handshake_response);
    RUN_TEST(inbound_cookie_reply);
    RUN_TEST(inbound_transport_data);
    RUN_TEST(roundtrip_init);
    RUN_TEST(roundtrip_response);
    RUN_TEST(roundtrip_cookie);
    RUN_TEST(roundtrip_transport);
    RUN_TEST(generate_junk);
    RUN_TEST(generate_junk_zero_jc);
    RUN_TEST(inbound_drops_unknown);
    RUN_TEST(inbound_drops_too_short);
    RUN_TEST(no_padding_s1_zero);
    RUN_TEST(no_padding_s2_zero);
    RUN_TEST(outbound_too_short);
    RUN_TEST(hrange_pick_contains);
    RUN_TEST(config_validate_accepts_safe_limits);
    RUN_TEST(config_validate_rejects_unsafe_padding);
    RUN_TEST(config_validate_rejects_overlapping_hranges);
    RUN_TEST(dual_profile_switch);
    RUN_TEST(outbound_cookie_with_s3);
    RUN_TEST(outbound_transport_with_s4);
    RUN_TEST(inbound_scanning_s3);
    RUN_TEST(inbound_scanning_s4);
    RUN_TEST(inbound_hrange_accept);
    RUN_TEST(inbound_hrange_reject);
    RUN_TEST(roundtrip_v2);
    RUN_TEST(v1_backward);
    RUN_TEST(v2_false_positive);
    RUN_TEST(server_init_peer_resolution);
    RUN_TEST(server_response_peer_resolution_single_direct);
    RUN_TEST(server_response_peer_resolution_two_direct_clients);
    RUN_TEST(server_response_peer_resolution_mixed_direct_and_proxy_fallback);
    /* MAC1 tests */
    RUN_TEST(mac1_outbound_init_normal);
    RUN_TEST(mac1_outbound_response_normal);
    RUN_TEST(mac1_outbound_init_server);
    RUN_TEST(mac1_outbound_response_server);
    RUN_TEST(mac1_inbound_init_normal);
    RUN_TEST(mac1_inbound_response_normal);
    RUN_TEST(mac1_inbound_init_server);
    RUN_TEST(mac1_inbound_response_server);
    RUN_TEST(mac1key_null_when_no_pub);
    RUN_TEST(mac1_roundtrip_normal);
    RUN_TEST(mac1_roundtrip_server);
    RUN_TEST(mac1_reverse_mode);
    /* AWG 3.0 header protection */
    RUN_TEST(hp_roundtrip_init);
    RUN_TEST(hp_padding_is_not_a_clone_of_caller_prng);
    RUN_TEST(hp_nonce_differs_between_packets);
    RUN_TEST(padding_is_not_a_prng_chain);
    RUN_TEST(shared_buf_is_reported_and_reused);
    RUN_TEST(reference_accepts_v1);
    RUN_TEST(reference_accepts_v1_5);
    RUN_TEST(reference_accepts_v2);
    RUN_TEST(reference_accepts_v3);
    RUN_TEST(reference_rejects_wrong_hp_key);
    RUN_TEST(hp_roundtrip_response);
    RUN_TEST(hp_roundtrip_cookie);
    RUN_TEST(hp_roundtrip_transport);
    RUN_TEST(hp_transport_encrypts_header_only);
    RUN_TEST(hp_inbound_type_detection);
    RUN_TEST(hp_validate_rejects_s_below_12);
    RUN_TEST(hp_off_matches_v2);
    RUN_TEST(hp_nonce_uniqueness);
    RUN_TEST(multi_profile_switch);
    /* AWG 3.1 random trailers / disabled cookies */
    RUN_TEST(rt_outbound_appends_trailer);
    RUN_TEST(rt_inbound_accepts_trailer);
    RUN_TEST(rt_inbound_rejects_trailer_when_off);
    RUN_TEST(rt_window_grows_with_traffic);
    RUN_TEST(rt_trailer_never_overflows_buffer);
    RUN_TEST(disable_cookies_drops_cookie_only);
    RUN_TEST(cookies_pass_when_enabled);
    RUN_TEST(rt_is_per_profile);
    TEST_MAIN_END();
}
