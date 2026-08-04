#include <stdint.h>
#include "test.h"
#include "chacha20.h"

/* Reference keystreams produced by golang.org/x/crypto/chacha20 — the very
 * library amneziawg-go uses for header protection (see device/noise-protocol.go,
 * HeaderProtectionCipher). Counter starts at 0, as in AWG. */

static void fill_seq(uint8_t *p, int n, uint8_t start, uint8_t step) {
    for (int i = 0; i < n; i++) p[i] = (uint8_t)(start + (uint8_t)(i * step));
}

/* RFC 8439 §2.3.2 key/nonce, two keystream blocks (counters 0 and 1). */
static void test_chacha20_seq_key_two_blocks(void) {
    uint8_t key[32], expected[128], out[128] = {0};
    const uint8_t nonce[12] = {0, 0, 0, 9, 0, 0, 0, 0x4a, 0, 0, 0, 0};
    chacha20_t c;

    fill_seq(key, 32, 0, 1);
    hex_decode("8adc91fd9ff4f0f51b0fad50ff15d637e40efda206cc52c783a74200503c1582"
               "cd9833367d0a54d57d3c9e998f490ee69ca34c1ff9e939a75584c52d690a35d4"
               "10f1e7e4d13b5915500fdd1fa32071c4c7d1f4c733c068030422aa9ac3d46c4e"
               "d2826446079faa0914c2d705d98b02a2b5129cd1de164eb9cbd083e8a2503c4e",
               expected, 128);

    chacha20_init(&c, key, nonce);
    chacha20_xor(&c, out, 128);
    ASSERT_MEM_EQ(out, expected, 128);
}

/* All-zero key and nonce — the classic RFC 8439 test vector. */
static void test_chacha20_zero_key(void) {
    uint8_t key[32] = {0}, nonce[12] = {0}, expected[64], out[64] = {0};
    chacha20_t c;

    hex_decode("76b8e0ada0f13d90405d6ae55386bd28bdd219b8a08ded1aa836efcc8b770dc7"
               "da41597c5157488d7724e03fb8d84a376a43b8f41518a11cc387b669b2ee6586",
               expected, 64);

    chacha20_init(&c, key, nonce);
    chacha20_xor(&c, out, 64);
    ASSERT_MEM_EQ(out, expected, 64);
}

/* Three blocks — covers the largest header-protected packet (initiation, 148 B). */
static void test_chacha20_three_blocks(void) {
    uint8_t key[32], nonce[12], expected[192], out[192] = {0};
    chacha20_t c;

    for (int i = 0; i < 32; i++) key[i] = (uint8_t)(0xa5 ^ (i * 7));
    fill_seq(nonce, 12, 0x10, 1);
    hex_decode("55719dda54094d30dd5a54f9ecc3686a1ff76be560a7bfcf9ef0ec16005187f8"
               "693130a6c2227367fd0b97224b0fcd7bfb363cb88f3ff46b2e1a5b88060ae4ad"
               "37fe6f68327b3290754dc08abc74c76de68e9f875c70f3cdda223f0cc8aef0fb"
               "5f7794e2ac2a994375bb2c97c4c000fdbf532a7a9fd45920bd9d64eb484c6cd2"
               "56afbff852fa928ad1602e8262a54267f16ecd22a1dc2b153e0ece158826f30c"
               "bd4c63085fb3d72250d6d20d0bc25f72c3146cdee39d4396c32bf2c7e4d1e01c",
               expected, 192);

    chacha20_init(&c, key, nonce);
    chacha20_xor(&c, out, 192);
    ASSERT_MEM_EQ(out, expected, 192);
}

/* The type hash is the first four keystream bytes as a little-endian uint32 —
 * this is what AWG XORs against the message type field. */
static void test_chacha20_type_hash(void) {
    uint8_t key[32] = {0}, nonce[12] = {0}, ks[4] = {0};
    chacha20_t c;
    uint32_t expected;

    chacha20_init(&c, key, nonce);
    chacha20_xor(&c, ks, 4);
    expected = (uint32_t)ks[0] | ((uint32_t)ks[1] << 8) |
               ((uint32_t)ks[2] << 16) | ((uint32_t)ks[3] << 24);

    chacha20_init(&c, key, nonce);
    ASSERT_EQ(chacha20_type_hash(&c), expected);
}

/* XOR is an involution: applying the same keystream twice restores the data. */
static void test_chacha20_roundtrip(void) {
    uint8_t key[32], nonce[12], data[148], orig[148];
    chacha20_t c;

    fill_seq(key, 32, 3, 5);
    fill_seq(nonce, 12, 0x77, 3);
    fill_seq(data, 148, 0, 1);
    memcpy(orig, data, sizeof(orig));

    chacha20_init(&c, key, nonce);
    chacha20_xor(&c, data, sizeof(data));
    ASSERT(memcmp(data, orig, sizeof(orig)) != 0);

    chacha20_init(&c, key, nonce);
    chacha20_xor(&c, data, sizeof(data));
    ASSERT_MEM_EQ(data, orig, sizeof(orig));
}

/* Short XOR (16 B transport header) must match the head of a long keystream —
 * the cached block 0 is reused, so this guards the caching logic. */
static void test_chacha20_partial_matches_full(void) {
    uint8_t key[32], nonce[12], full[64] = {0}, part[16] = {0};
    chacha20_t c;

    fill_seq(key, 32, 0x11, 2);
    fill_seq(nonce, 12, 0x22, 1);

    chacha20_init(&c, key, nonce);
    chacha20_xor(&c, full, sizeof(full));

    chacha20_init(&c, key, nonce);
    chacha20_xor(&c, part, sizeof(part));

    ASSERT_MEM_EQ(part, full, sizeof(part));
}

int main(void) {
    fprintf(stderr, "chacha20 tests:\n");
    RUN_TEST(chacha20_seq_key_two_blocks);
    RUN_TEST(chacha20_zero_key);
    RUN_TEST(chacha20_three_blocks);
    RUN_TEST(chacha20_type_hash);
    RUN_TEST(chacha20_roundtrip);
    RUN_TEST(chacha20_partial_matches_full);
    TEST_MAIN_END();
}
