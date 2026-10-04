#include <stdint.h>
#include <time.h>
#include "test.h"
#include "transform.h"
#include "cps.h"

static csprng_t rng;

static void test_parse_static_bytes(void) {
    cps_template_t tmpl;
    ASSERT_EQ(cps_parse("<b 0x0844>", &tmpl), 0);
    ASSERT_EQ(tmpl.nseg, 1);
    ASSERT_EQ(tmpl.segs[0].kind, CPS_STATIC);
    ASSERT_EQ(tmpl.segs[0].data_len, 2);
    ASSERT_EQ(tmpl.static_data[0], 0x08);
    ASSERT_EQ(tmpl.static_data[1], 0x44);
}

static void test_parse_random(void) {
    cps_template_t tmpl;
    ASSERT_EQ(cps_parse("<r 16>", &tmpl), 0);
    ASSERT_EQ(tmpl.nseg, 1);
    ASSERT_EQ(tmpl.segs[0].kind, CPS_RANDOM);
    ASSERT_EQ(tmpl.segs[0].size, 16);
}

static void test_parse_timestamp(void) {
    cps_template_t tmpl;
    ASSERT_EQ(cps_parse("<t>", &tmpl), 0);
    ASSERT_EQ(tmpl.nseg, 1);
    ASSERT_EQ(tmpl.segs[0].kind, CPS_TIMESTAMP);
}

static void test_parse_counter(void) {
    cps_template_t tmpl;
    ASSERT_EQ(cps_parse("<c>", &tmpl), 0);
    ASSERT_EQ(tmpl.nseg, 1);
    ASSERT_EQ(tmpl.segs[0].kind, CPS_COUNTER);
}

static void test_parse_random_chars(void) {
    cps_template_t tmpl;
    ASSERT_EQ(cps_parse("<rc 12>", &tmpl), 0);
    ASSERT_EQ(tmpl.nseg, 1);
    ASSERT_EQ(tmpl.segs[0].kind, CPS_RANDOM_CHARS);
    ASSERT_EQ(tmpl.segs[0].size, 12);
}

static void test_parse_random_digits(void) {
    cps_template_t tmpl;
    ASSERT_EQ(cps_parse("<rd 8>", &tmpl), 0);
    ASSERT_EQ(tmpl.nseg, 1);
    ASSERT_EQ(tmpl.segs[0].kind, CPS_RANDOM_DIGITS);
    ASSERT_EQ(tmpl.segs[0].size, 8);
}

static void test_generate_random_chars(void) {
    cps_template_t tmpl;
    uint8_t buf[64];
    ASSERT_EQ(cps_parse("<rc 20>", &tmpl), 0);
    int n = cps_generate(&tmpl, 0, &rng, buf, 64);
    ASSERT_EQ(n, 20);
    for (int i = 0; i < 20; i++) {
        int ok = (buf[i] >= '0' && buf[i] <= '9') ||
                 (buf[i] >= 'A' && buf[i] <= 'Z') ||
                 (buf[i] >= 'a' && buf[i] <= 'z');
        ASSERT(ok);
    }
}

static void test_generate_random_digits(void) {
    cps_template_t tmpl;
    uint8_t buf[64];
    ASSERT_EQ(cps_parse("<rd 10>", &tmpl), 0);
    int n = cps_generate(&tmpl, 0, &rng, buf, 64);
    ASSERT_EQ(n, 10);
    for (int i = 0; i < 10; i++) {
        ASSERT(buf[i] >= '0' && buf[i] <= '9');
    }
}

static void test_parse_mixed_rc_rd(void) {
    cps_template_t tmpl;
    uint8_t buf[64];
    ASSERT_EQ(cps_parse("<b 0xDEAD> <rc 8> <t> <rd 4>", &tmpl), 0);
    ASSERT_EQ(tmpl.nseg, 4);
    int n = cps_generate(&tmpl, 0, &rng, buf, 64);
    ASSERT_EQ(n, 18); /* 2 + 8 + 4 + 4 */
    ASSERT_EQ(buf[0], 0xDE);
    ASSERT_EQ(buf[1], 0xAD);
    /* bytes [2:10] alphanumeric */
    for (int i = 2; i < 10; i++) {
        int ok = (buf[i] >= '0' && buf[i] <= '9') ||
                 (buf[i] >= 'A' && buf[i] <= 'Z') ||
                 (buf[i] >= 'a' && buf[i] <= 'z');
        ASSERT(ok);
    }
    /* bytes [10:14] timestamp (just check it's 4 bytes, can't verify exact value) */
    /* bytes [14:18] digits */
    for (int i = 14; i < 18; i++) {
        ASSERT(buf[i] >= '0' && buf[i] <= '9');
    }
}

static void test_parse_rc_rd_invalid(void) {
    cps_template_t tmpl;
    ASSERT_EQ(cps_parse("<rc>", &tmpl), -1);
    ASSERT_EQ(cps_parse("<rc abc>", &tmpl), -1);
    ASSERT_EQ(cps_parse("<rc 0>", &tmpl), -1);
    ASSERT_EQ(cps_parse("<rc -1>", &tmpl), -1);
    ASSERT_EQ(cps_parse("<rd>", &tmpl), -1);
    ASSERT_EQ(cps_parse("<rd abc>", &tmpl), -1);
    ASSERT_EQ(cps_parse("<rd 0>", &tmpl), -1);
    ASSERT_EQ(cps_parse("<rd -1>", &tmpl), -1);
}

static void test_parse_multi_segment(void) {
    cps_template_t tmpl;
    ASSERT_EQ(cps_parse("<b 0xAABB> <r 8> <t> <c>", &tmpl), 0);
    ASSERT_EQ(tmpl.nseg, 4);
    ASSERT_EQ(tmpl.segs[0].kind, CPS_STATIC);
    ASSERT_EQ(tmpl.segs[1].kind, CPS_RANDOM);
    ASSERT_EQ(tmpl.segs[2].kind, CPS_TIMESTAMP);
    ASSERT_EQ(tmpl.segs[3].kind, CPS_COUNTER);
}

static void test_parse_empty(void) {
    cps_template_t tmpl;
    ASSERT_EQ(cps_parse("", &tmpl), -1);
}

static void test_parse_invalid(void) {
    cps_template_t tmpl;
    ASSERT_EQ(cps_parse("no tags here", &tmpl), -1);
    ASSERT_EQ(cps_parse("<b>", &tmpl), -1);
    ASSERT_EQ(cps_parse("<b 0x>", &tmpl), -1);
    ASSERT_EQ(cps_parse("<b 0xGG>", &tmpl), -1);
    ASSERT_EQ(cps_parse("<b 0x1>", &tmpl), -1);
    ASSERT_EQ(cps_parse("<r>", &tmpl), -1);
    ASSERT_EQ(cps_parse("<r abc>", &tmpl), -1);
    ASSERT_EQ(cps_parse("<r -5>", &tmpl), -1);
    ASSERT_EQ(cps_parse("<r 0>", &tmpl), -1);
    ASSERT_EQ(cps_parse("<x>", &tmpl), -1);
    ASSERT_EQ(cps_parse("<b 0xFF", &tmpl), -1);
}

static void test_parse_large_static(void) {
    /* Real SIP INVITE template ~348 bytes hex = 174 bytes binary */
    /* Simulate with 348 hex chars (174 bytes) */
    char tmpl_str[1024];
    int pos = 0;
    memcpy(tmpl_str + pos, "<b 0x", 5); pos += 5;
    for (int i = 0; i < 348; i++) {
        tmpl_str[pos++] = "0123456789ABCDEF"[i % 16];
    }
    tmpl_str[pos++] = '>';
    tmpl_str[pos] = '\0';
    cps_template_t tmpl;
    ASSERT_EQ(cps_parse(tmpl_str, &tmpl), 0);
    ASSERT_EQ(tmpl.nseg, 1);
    ASSERT_EQ(tmpl.segs[0].data_len, 174);
}

static void test_parse_max_static(void) {
    /* 1500 bytes = 3000 hex chars — exactly at the limit */
    char tmpl_str[3100];
    int pos = 0;
    memcpy(tmpl_str + pos, "<b 0x", 5); pos += 5;
    for (int i = 0; i < 3000; i++) {
        tmpl_str[pos++] = "AA"[i % 2];
    }
    tmpl_str[pos++] = '>';
    tmpl_str[pos] = '\0';
    cps_template_t tmpl;
    ASSERT_EQ(cps_parse(tmpl_str, &tmpl), 0);
    ASSERT_EQ(tmpl.segs[0].data_len, 1500);
}

static void test_parse_overflow_static(void) {
    /* 1502 bytes = 3004 hex chars — over the limit */
    char tmpl_str[3100];
    int pos = 0;
    memcpy(tmpl_str + pos, "<b 0x", 5); pos += 5;
    for (int i = 0; i < 3004; i++) {
        tmpl_str[pos++] = "BB"[i % 2];
    }
    tmpl_str[pos++] = '>';
    tmpl_str[pos] = '\0';
    cps_template_t tmpl;
    ASSERT_EQ(cps_parse(tmpl_str, &tmpl), -1);
}

static void test_generate_large_static(void) {
    /* Parse a 200-byte static block and verify generate outputs it correctly */
    char tmpl_str[512];
    int pos = 0;
    memcpy(tmpl_str + pos, "<b 0x", 5); pos += 5;
    for (int i = 0; i < 400; i++) {
        tmpl_str[pos++] = "0123456789ABCDEF"[i % 16];
    }
    tmpl_str[pos++] = '>';
    tmpl_str[pos] = '\0';
    cps_template_t tmpl;
    ASSERT_EQ(cps_parse(tmpl_str, &tmpl), 0);
    ASSERT_EQ(tmpl.segs[0].data_len, 200);
    uint8_t buf[256];
    int n = cps_generate(&tmpl, 0, &rng, buf, 256);
    ASSERT_EQ(n, 200);
    ASSERT_MEM_EQ(buf, tmpl.static_data, 200);
}

static void test_parse_int_overflow(void) {
    cps_template_t tmpl;
    ASSERT_EQ(cps_parse("<r 99999999999>", &tmpl), -1);
}

static void test_generate_cps(void) {
    cps_template_t tmpl;
    uint8_t buf[64];
    ASSERT_EQ(cps_parse("<b 0xDEAD> <r 4> <c>", &tmpl), 0);
    int n = cps_generate(&tmpl, 42, &rng, buf, 64);
    ASSERT_EQ(n, 10); /* 2 + 4 + 4 */
    ASSERT_EQ(buf[0], 0xDE);
    ASSERT_EQ(buf[1], 0xAD);
    /* bytes [2:6] random */
    /* bytes [6:10] counter = 42 LE */
    uint32_t counter;
    memcpy(&counter, buf + 6, 4);
    ASSERT_EQ(counter, 42u);
}

/* --- AWG 3.0 obf tags and upstream byte-for-byte fidelity --- */

/* <d> and <ds> emit nothing upstream (ObfuscatedLen(0) == 0), so they are
 * skipped; <dz N> emits N bytes holding the big-endian source length, which is
 * always 0 for an I-packet. */
static void test_parse_d_tags(void) {
    cps_template_t t;
    uint8_t buf[64];

    ASSERT_EQ(cps_parse("<b 0xAABB><d><ds><r 4>", &t), 0);
    ASSERT_EQ(t.nseg, 2);
    ASSERT_EQ(cps_max_size(&t), 6);

    ASSERT_EQ(cps_parse("<b 0xAABB><dz 5>", &t), 0);
    ASSERT_EQ(t.nseg, 2);
    ASSERT_EQ(cps_max_size(&t), 7);
    memset(buf, 0xFF, sizeof(buf));
    ASSERT_EQ(cps_generate(&t, 1, &rng, buf, sizeof(buf)), 7);
    ASSERT_EQ(buf[0], 0xAA);
    ASSERT_EQ(buf[1], 0xBB);
    for (int i = 2; i < 7; i++) ASSERT_EQ(buf[i], 0x00);

    /* <dz> without a size is malformed */
    ASSERT_EQ(cps_parse("<dz>", &t), -1);
}

/* Upstream <rc N> draws from 52 letters only (device/obf_randchars.go) */
static void test_randchars_letters_only(void) {
    cps_template_t t;
    uint8_t buf[256];

    ASSERT_EQ(cps_parse("<rc 200>", &t), 0);
    for (uint32_t c = 0; c < 8; c++) {
        int n = cps_generate(&t, c, &rng, buf, sizeof(buf));
        ASSERT_EQ(n, 200);
        for (int i = 0; i < n; i++) {
            int ok = (buf[i] >= 'a' && buf[i] <= 'z') || (buf[i] >= 'A' && buf[i] <= 'Z');
            ASSERT(ok);
        }
    }
}

/* Upstream writes the timestamp big-endian (device/obf_timestamp.go) */
static void test_timestamp_big_endian(void) {
    cps_template_t t;
    uint8_t buf[16];

    ASSERT_EQ(cps_parse("<t>", &t), 0);
    ASSERT_EQ(cps_generate(&t, 0, &rng, buf, sizeof(buf)), 4);
    uint32_t be = ((uint32_t)buf[0] << 24) | ((uint32_t)buf[1] << 16) |
                  ((uint32_t)buf[2] << 8) | buf[3];
    uint32_t now = (uint32_t)time(NULL);
    /* Same second, or one tick later */
    ASSERT(be == now || be + 1 == now || be == now + 1);
    /* A plausible 2020s unix time must have a small top byte when big-endian */
    ASSERT(buf[0] >= 0x60 && buf[0] <= 0x80);
}

/* <r>/<rc>/<rd> come from the caller's generator, not from the counter: the
 * counter restarts with the process, and bytes derived from it repeated on
 * every start and on every install with the same template. <t>/<c> stay. */
static void test_random_from_caller_rng(void) {
    static const uint8_t k1[32] = { 1 }, k2[32] = { 2 };
    cps_template_t t;
    uint8_t a[64], b[64];
    csprng_t r1, r2;

    ASSERT_EQ(cps_parse("<b 0xAA><r 16><rc 8><rd 8><t><c>", &t), 0);
    csprng_init(&r1, k1, 0);
    csprng_init(&r2, k2, 0);
    ASSERT_EQ(cps_generate(&t, 7, &r1, a, sizeof(a)), 41);
    ASSERT_EQ(cps_generate(&t, 7, &r2, b, sizeof(b)), 41);
    ASSERT_EQ(a[0], 0xAA);
    ASSERT(memcmp(a + 1, b + 1, 32) != 0);
    uint32_t ta = ((uint32_t)a[33] << 24) | ((uint32_t)a[34] << 16) | ((uint32_t)a[35] << 8) | a[36];
    uint32_t tb = ((uint32_t)b[33] << 24) | ((uint32_t)b[34] << 16) | ((uint32_t)b[35] << 8) | b[36];
    ASSERT(tb - ta <= 1);
    ASSERT_MEM_EQ(a + 37, b + 37, 4);

    /* Same counter again: the generator has moved on, so the bytes are new */
    ASSERT_EQ(cps_generate(&t, 7, &r1, b, sizeof(b)), 41);
    ASSERT(memcmp(a + 1, b + 1, 16) != 0);

    /* Same key and stream: same bytes; another stream: other bytes */
    csprng_init(&r1, k1, 0);
    csprng_init(&r2, k1, 0);
    cps_generate(&t, 7, &r1, a, sizeof(a));
    cps_generate(&t, 7, &r2, b, sizeof(b));
    ASSERT_MEM_EQ(a + 1, b + 1, 32);
    csprng_init(&r2, k1, 1);
    cps_generate(&t, 7, &r2, b, sizeof(b));
    ASSERT(memcmp(a + 1, b + 1, 32) != 0);
}

/* xorshift hands out its whole state: with it, the 8 bytes after a random DCID
 * were one xorshift step of the DCID itself, and anyone could check that. */
static void test_random_not_self_predicting(void) {
    cps_template_t t;
    uint8_t a[64];

    ASSERT_EQ(cps_parse("<r 8><r 8>", &t), 0);
    for (int i = 0; i < 4; i++) {
        ASSERT_EQ(cps_generate(&t, 0, &rng, a, sizeof(a)), 16);
        fastrand_t x;
        memcpy(&x.s, a, 8);
        uint64_t next = fastrand_u64(&x);
        ASSERT(memcmp(&next, a + 8, 8) != 0);
    }
}

/* Every letter turns up: rejection sampling must not cut off the tail of the
 * alphabet the way a careless bound would. */
static void test_randchars_cover_alphabet(void) {
    cps_template_t t;
    uint8_t buf[1000];
    int seen[256] = { 0 };

    ASSERT_EQ(cps_parse("<rc 1000>", &t), 0);
    for (int r = 0; r < 4; r++) {
        ASSERT_EQ(cps_generate(&t, 0, &rng, buf, sizeof(buf)), 1000);
        for (int i = 0; i < 1000; i++) seen[buf[i]] = 1;
    }
    for (const char *c = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ"; *c; c++)
        ASSERT(seen[(uint8_t)*c]);
}

/* The configurator's WARP preset: what a Chrome QUIC v1 client Initial shows
 * without the keys, 1250 bytes in all, with a fresh DCID in every packet. */
static void test_warp_quic_template(void) {
    static const uint8_t lead[] = { 0xc3, 0x00, 0x00, 0x00, 0x01, 0x08 };
    cps_template_t t;
    uint8_t a[1500], b[1500];

    ASSERT_EQ(cps_parse("<b 0xc30000000108><r 8><b 0x000044d0><r 1232>", &t), 0);
    ASSERT_EQ(cps_max_size(&t), 1250);
    ASSERT_EQ(cps_generate(&t, 0, &rng, a, sizeof(a)), 1250);
    ASSERT_EQ(cps_generate(&t, 0, &rng, b, sizeof(b)), 1250);
    ASSERT_MEM_EQ(a, lead, 6);
    ASSERT_EQ(a[14], 0x00);                               /* SCID length */
    ASSERT_EQ(a[15], 0x00);                               /* token length */
    ASSERT_EQ(((a[16] & 0x3f) << 8) | a[17], 1250 - 18);  /* Length varint */
    ASSERT(memcmp(a + 6, b + 6, 8) != 0);                 /* DCID */
}

int main(void) {
    static const uint8_t key[32] = { 0xC0, 0xFF, 0xEE };
    fprintf(stderr, "=== cps tests ===\n");
    csprng_init(&rng, key, 0);
    RUN_TEST(parse_static_bytes);
    RUN_TEST(parse_random);
    RUN_TEST(parse_timestamp);
    RUN_TEST(parse_counter);
    RUN_TEST(parse_random_chars);
    RUN_TEST(parse_random_digits);
    RUN_TEST(generate_random_chars);
    RUN_TEST(generate_random_digits);
    RUN_TEST(parse_mixed_rc_rd);
    RUN_TEST(parse_rc_rd_invalid);
    RUN_TEST(parse_multi_segment);
    RUN_TEST(parse_empty);
    RUN_TEST(parse_invalid);
    RUN_TEST(generate_cps);
    RUN_TEST(parse_large_static);
    RUN_TEST(parse_max_static);
    RUN_TEST(parse_overflow_static);
    RUN_TEST(generate_large_static);
    RUN_TEST(parse_int_overflow);
    RUN_TEST(parse_d_tags);
    RUN_TEST(randchars_letters_only);
    RUN_TEST(timestamp_big_endian);
    RUN_TEST(random_from_caller_rng);
    RUN_TEST(random_not_self_predicting);
    RUN_TEST(randchars_cover_alphabet);
    RUN_TEST(warp_quic_template);
    TEST_MAIN_END();
}
