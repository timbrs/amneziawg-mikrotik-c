#include <stdint.h>
#include <stdlib.h>
#include "test.h"
#include "transform.h"
#include "blake2s.h"
#include "fastrand.h"

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

/* v3 config: every padding >= 12 (the ChaCha20 nonce lives in it). */
static awg_config_t make_hp_config(uint8_t key_seed) {
    awg_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.jc = 3; cfg.jmin = 30; cfg.jmax = 500;
    cfg.s1 = 20; cfg.s2 = 18; cfg.s3 = 15; cfg.s4 = 12;
    cfg.h1 = (hrange_t){100000, 200000};
    cfg.h2 = (hrange_t){300000, 400000};
    cfg.h3 = (hrange_t){500000, 600000};
    cfg.h4 = (hrange_t){700000, 800000};
    for (int i = 0; i < CHACHA20_KEY_SIZE; i++)
        cfg.hp_key[i] = (uint8_t)(key_seed + i * 3);
    cfg.has_hp = 1;
    config_compute(&cfg);
    return cfg;
}

/* Every message type must survive outbound -> inbound with the header encrypted
 * in between. */
static void test_hp_roundtrip_all_types(void) {
    awg_config_t cfg = make_hp_config(0x41);
    const uint32_t types[4] = {
        WG_HANDSHAKE_INIT, WG_HANDSHAKE_RESPONSE, WG_COOKIE_REPLY, WG_TRANSPORT_DATA
    };
    const int sizes[4] = { WG_INIT_SIZE, WG_RESP_SIZE, WG_COOKIE_SIZE, 300 };
    const int pads[4] = { cfg.s1, cfg.s2, cfg.s3, cfg.s4 };

    for (int t = 0; t < 4; t++) {
        uint8_t orig[512], buf[256 + 512];
        int dataoff = 256, n = sizes[t];

        write32_le(orig, types[t]);
        fill_seq(orig + 4, n - 4);
        memset(buf, 0, sizeof(buf));
        memcpy(buf + dataoff, orig, n);

        int out_len, sendJunk;
        uint8_t *out = transform_outbound(buf, dataoff, n, &cfg,
                                          0x1122334455667788ULL + t,
                                          &out_len, &sendJunk);
        ASSERT_EQ(out_len, pads[t] + n);

        int in_len;
        uint8_t *r = transform_inbound(out, out_len, &cfg, &in_len);
        ASSERT(r != NULL);
        ASSERT_EQ(in_len, n);
        ASSERT_EQ(read32_le(r), types[t]);
        ASSERT_MEM_EQ(r + 4, orig + 4, n - 4);
    }
}

/* On the wire the type field must be ciphertext: neither the WireGuard type nor
 * the H value may be readable. For transport only the 16-byte header is
 * protected — the payload must stay untouched. */
static void test_hp_header_is_encrypted(void) {
    awg_config_t cfg = make_hp_config(0x7e);
    uint8_t orig[300], buf[256 + 300];
    int dataoff = 256, n = 300;

    write32_le(orig, WG_TRANSPORT_DATA);
    fill_seq(orig + 4, n - 4);
    memset(buf, 0, sizeof(buf));
    memcpy(buf + dataoff, orig, n);

    int out_len, sendJunk;
    uint8_t *out = transform_outbound(buf, dataoff, n, &cfg, 0xdeadbeefULL,
                                      &out_len, &sendJunk);

    uint32_t wire_type = read32_le(out + cfg.s4);
    ASSERT(wire_type != WG_TRANSPORT_DATA);
    ASSERT(!hrange_contains(&cfg.h4, wire_type));
    /* Header encrypted, payload as-is */
    ASSERT(memcmp(out + cfg.s4, orig, WG_TRANSPORT_HDR) != 0);
    ASSERT_MEM_EQ(out + cfg.s4 + WG_TRANSPORT_HDR, orig + WG_TRANSPORT_HDR,
                  n - WG_TRANSPORT_HDR);
}

/* The padding doubles as the nonce, so it must be regenerated per packet —
 * otherwise identical headers would encrypt to identical bytes. */
static void test_hp_nonce_is_fresh_per_packet(void) {
    awg_config_t cfg = make_hp_config(0x0a);
    uint8_t buf_a[256 + 64], buf_b[256 + 64];
    uint8_t wire_a[64 + 32], wire_b[64 + 32];
    int dataoff = 256, n = 64, len_a, len_b, junk;

    memset(buf_a, 0, sizeof(buf_a));
    memset(buf_b, 0, sizeof(buf_b));
    write32_le(buf_a + dataoff, WG_TRANSPORT_DATA);
    write32_le(buf_b + dataoff, WG_TRANSPORT_DATA);
    fill_seq(buf_a + dataoff + 4, n - 4);
    memcpy(buf_b + dataoff, buf_a + dataoff, n);

    uint8_t *a = transform_outbound(buf_a, dataoff, n, &cfg, 111, &len_a, &junk);
    memcpy(wire_a, a, len_a);
    uint8_t *b = transform_outbound(buf_b, dataoff, n, &cfg, 222, &len_b, &junk);
    memcpy(wire_b, b, len_b);

    ASSERT_EQ(len_a, len_b);
    /* different nonce ... */
    ASSERT(memcmp(wire_a, wire_b, CHACHA20_NONCE_SIZE) != 0);
    /* ... hence different ciphertext for the same header */
    ASSERT(memcmp(wire_a + cfg.s4, wire_b + cfg.s4, WG_TRANSPORT_HDR) != 0);
}

/* The padding is the nonce, so it must not be derivable from anything an
 * observer can see or replay: identical inputs (same rand_val) must still give
 * different padding. Guards against going back to fastrand here. */
static void test_hp_padding_is_not_derived_from_rand_val(void) {
    awg_config_t cfg = make_hp_config(0x5c);
    uint8_t buf_a[256 + 64], buf_b[256 + 64], pad_a[64];
    int dataoff = 256, n = 64, len_a, len_b, junk;

    memset(buf_a, 0, sizeof(buf_a));
    memset(buf_b, 0, sizeof(buf_b));
    write32_le(buf_a + dataoff, WG_TRANSPORT_DATA);
    write32_le(buf_b + dataoff, WG_TRANSPORT_DATA);

    uint8_t *a = transform_outbound(buf_a, dataoff, n, &cfg, 12345, &len_a, &junk);
    memcpy(pad_a, a, cfg.s4);
    uint8_t *b = transform_outbound(buf_b, dataoff, n, &cfg, 12345, &len_b, &junk);

    ASSERT(memcmp(pad_a, b, cfg.s4) != 0);
}

/* Golden vectors: packets built by amneziawg-go itself (device/send.go layout,
 * golang.org/x/crypto/chacha20), so the tests are not just C talking to C. Key
 * = make_hp_config(0x41), paddings 20/18/15/12, H values inside the ranges of
 * that config, body byte i = i. */
static void hp_check_golden(const char *hex, int wire_len, int pad, int body_len,
                            uint32_t want_type) {
    awg_config_t cfg = make_hp_config(0x41);
    uint8_t wire[512];
    int in_len;

    ASSERT_EQ(hex_decode(hex, wire, (int)sizeof(wire)), wire_len);

    uint8_t *r = transform_inbound(wire, wire_len, &cfg, &in_len);
    ASSERT(r != NULL);
    ASSERT_EQ(in_len, body_len);
    ASSERT_EQ(read32_le(r), want_type);
    for (int i = 4; i < body_len; i++)
        ASSERT_EQ(r[i], (uint8_t)i);
    ASSERT_EQ(pad, wire_len - body_len);
}

static void test_hp_golden_init_from_go(void) {
    hp_check_golden(
        "303b46515c67727d88939ea9b4bfcad5e0ebf601ee03841e430d7d98dc75a22d67ccedcad863ef72"
        "6317870d083db5052b71d828886a53995968363411cc6f970e6454fec92b0620f2a42082e8007645"
        "7a7956624513df6b97bc2b6db5e7640409cdc0e38c091dc97d732795df94bb4886a0ff3237bf614a"
        "7c80b06d1843e0930ed807cbc23eacbe8f8ae9575faf5e781d0d1ddfe7866c6451a60d5186dc2b61"
        "70ae468dc4b7fda1",
        20 + WG_INIT_SIZE, 20, WG_INIT_SIZE, WG_HANDSHAKE_INIT);
}

static void test_hp_golden_response_from_go(void) {
    hp_check_golden(
        "505b66717c87929da8b3bec9d4dfeaf5000b5af7c6a8b86abd8228b984ba39f76b65a3167815755a"
        "086e03b198b4d7b9bda09f79dc38f736d7e9a6f58abac4a07c0ab4a5cc758a99084a5754d4fd2ebc"
        "b9aff9d44724fe9cbd1b93a717492254bc8c96fe4ed748624096e03b957e",
        18 + WG_RESP_SIZE, 18, WG_RESP_SIZE, WG_HANDSHAKE_RESPONSE);
}

static void test_hp_golden_cookie_from_go(void) {
    hp_check_golden(
        "707b86919ca7b2bdc8d3dee9f4ff0a588d21d30f55aa31b80b5c9269c3978035d8042e9d68bd6f16"
        "69598964725644fd2bf45199c9cf309864bdb7f31f095108c8a76f2fdc68701b252baf8f05cd58",
        15 + WG_COOKIE_SIZE, 15, WG_COOKIE_SIZE, WG_COOKIE_REPLY);
}

static void test_hp_golden_transport_from_go(void) {
    /* Only the 16-byte header is protected; the rest must pass through intact */
    hp_check_golden(
        "909ba6b1bcc7d2dde8f3fe09a4d46cd583d3225718dc2add39f97716101112131415161718191a1b"
        "1c1d1e1f202122232425262728292a2b2c2d2e2f303132333435363738393a3b3c3d3e3f40414243"
        "4445464748494a4b4c4d4e4f505152535455565758595a5b5c5d5e5f606162636465666768696a6b"
        "6c6d6e6f707172737475767778797a7b7c7d7e7f808182838485868788898a8b8c8d8e8f90919293"
        "9495969798999a9b9c9d9e9fa0a1a2a3a4a5a6a7a8a9aaabacadaeafb0b1b2b3b4b5b6b7b8b9babb"
        "bcbdbebfc0c1c2c3c4c5c6c7c8c9cacbcccdcecfd0d1d2d3d4d5d6d7d8d9dadbdcdddedfe0e1e2e3"
        "e4e5e6e7e8e9eaebecedeeeff0f1f2f3f4f5f6f7f8f9fafbfcfdfeff000102030405060708090a0b"
        "0c0d0e0f101112131415161718191a1b1c1d1e1f202122232425262728292a2b",
        12 + 300, 12, 300, WG_TRANSPORT_DATA);
}

/* AWG 2.0 compatibility: a config without AWG_HP_KEY must produce exactly the
 * pre-3.0 wire format — padding, H type, everything else byte-identical — and
 * decode back. This is what guarantees existing v2 deployments keep working
 * after the upgrade. */
static void test_v2_wire_format_unchanged(void) {
    awg_config_t cfg = make_hp_config(0x22);
    cfg.has_hp = 0;
    memset(cfg.hp_key, 0, sizeof(cfg.hp_key));
    config_compute(&cfg);

    const uint32_t types[4] = {
        WG_HANDSHAKE_INIT, WG_HANDSHAKE_RESPONSE, WG_COOKIE_REPLY, WG_TRANSPORT_DATA
    };
    const int sizes[4] = { WG_INIT_SIZE, WG_RESP_SIZE, WG_COOKIE_SIZE, 220 };
    const int pads[4] = { cfg.s1, cfg.s2, cfg.s3, cfg.s4 };
    const hrange_t *ranges[4] = { &cfg.h1, &cfg.h2, &cfg.h3, &cfg.h4 };

    for (int t = 0; t < 4; t++) {
        uint8_t orig[512], buf[256 + 512];
        int dataoff = 256, n = sizes[t], out_len, junk, in_len;

        write32_le(orig, types[t]);
        fill_seq(orig + 4, n - 4);
        memset(buf, 0, sizeof(buf));
        memcpy(buf + dataoff, orig, n);

        uint8_t *out = transform_outbound(buf, dataoff, n, &cfg, 0x5150ULL + t,
                                          &out_len, &junk);
        ASSERT_EQ(out_len, pads[t] + n);
        /* type replaced by an H value, in the clear */
        ASSERT(hrange_contains(ranges[t], read32_le(out + pads[t])));
        /* body untouched (MAC1 recompute is off: no peer keys in this config) */
        ASSERT_MEM_EQ(out + pads[t] + 4, orig + 4, n - 4);

        uint8_t *r = transform_inbound(out, out_len, &cfg, &in_len);
        ASSERT(r != NULL);
        ASSERT_EQ(in_len, n);
        ASSERT_EQ(read32_le(r), types[t]);
        ASSERT_MEM_EQ(r + 4, orig + 4, n - 4);
    }
}

/* A v2 peer (no header protection) and a v3 config must not silently talk past
 * each other: packets from a plain-v2 sender are rejected, not misparsed. */
static void test_v2_packet_rejected_by_v3_config(void) {
    awg_config_t v2 = make_hp_config(0x66);
    v2.has_hp = 0;
    memset(v2.hp_key, 0, sizeof(v2.hp_key));
    config_compute(&v2);
    awg_config_t v3 = make_hp_config(0x66);

    uint8_t buf[256 + WG_INIT_SIZE];
    int dataoff = 256, out_len, junk, in_len;

    memset(buf, 0, sizeof(buf));
    write32_le(buf + dataoff, WG_HANDSHAKE_INIT);
    fill_seq(buf + dataoff + 4, WG_INIT_SIZE - 4);

    uint8_t *out = transform_outbound(buf, dataoff, WG_INIT_SIZE, &v2, 9, &out_len, &junk);
    ASSERT(transform_inbound(out, out_len, &v3, &in_len) == NULL);
}

/* A packet sealed with one key must not decode under another. */
static void test_hp_wrong_key_is_rejected(void) {
    awg_config_t sender = make_hp_config(0x11);
    awg_config_t receiver = make_hp_config(0x99);
    uint8_t buf[256 + WG_INIT_SIZE];
    int dataoff = 256, out_len, junk, in_len;

    memset(buf, 0, sizeof(buf));
    write32_le(buf + dataoff, WG_HANDSHAKE_INIT);
    fill_seq(buf + dataoff + 4, WG_INIT_SIZE - 4);

    uint8_t *out = transform_outbound(buf, dataoff, WG_INIT_SIZE, &sender, 7,
                                      &out_len, &junk);
    ASSERT(transform_inbound(out, out_len, &receiver, &in_len) == NULL);
}

/* Regression guard for v2 deployments: without a key nothing is encrypted and
 * the type on the wire is the plain H value, as before. */
static void test_hp_disabled_leaves_header_plain(void) {
    awg_config_t cfg = make_hp_config(0x33);
    cfg.has_hp = 0;
    memset(cfg.hp_key, 0, sizeof(cfg.hp_key));
    config_compute(&cfg);

    uint8_t buf[256 + 128];
    int dataoff = 256, n = 128, out_len, junk;

    memset(buf, 0, sizeof(buf));
    write32_le(buf + dataoff, WG_TRANSPORT_DATA);
    fill_seq(buf + dataoff + 4, n - 4);

    uint8_t *out = transform_outbound(buf, dataoff, n, &cfg, 5, &out_len, &junk);
    ASSERT(hrange_contains(&cfg.h4, read32_le(out + cfg.s4)));
}

/* The server refuses S < 12 with header protection on; so must the proxy. */
static void test_hp_requires_padding_of_12(void) {
    awg_config_t cfg = make_hp_config(0x55);
    const char *err = NULL;

    ASSERT_EQ(config_validate(&cfg, &err), 0);

    cfg.s4 = AWG_HP_MIN_PADDING - 1;
    ASSERT(config_validate(&cfg, &err) < 0);
    ASSERT(err != NULL);

    cfg.s4 = AWG_HP_MIN_PADDING;
    cfg.s3 = 4;
    ASSERT(config_validate(&cfg, &err) < 0);

    /* Same paddings are fine once header protection is off */
    cfg.has_hp = 0;
    ASSERT_EQ(config_validate(&cfg, &err), 0);
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
    RUN_TEST(hp_roundtrip_all_types);
    RUN_TEST(hp_header_is_encrypted);
    RUN_TEST(hp_nonce_is_fresh_per_packet);
    RUN_TEST(hp_padding_is_not_derived_from_rand_val);
    RUN_TEST(hp_wrong_key_is_rejected);
    RUN_TEST(hp_disabled_leaves_header_plain);
    RUN_TEST(hp_requires_padding_of_12);
    /* Golden vectors produced by amneziawg-go */
    RUN_TEST(hp_golden_init_from_go);
    RUN_TEST(hp_golden_response_from_go);
    RUN_TEST(hp_golden_cookie_from_go);
    RUN_TEST(hp_golden_transport_from_go);
    /* AWG 2.0 configs keep working */
    RUN_TEST(v2_wire_format_unchanged);
    RUN_TEST(v2_packet_rejected_by_v3_config);
    TEST_MAIN_END();
}
