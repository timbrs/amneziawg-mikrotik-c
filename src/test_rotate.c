/* When the port rotation falls due, and when it may act.
 *
 * Both halves are a handful of comparisons, and each of them stands between a
 * user and a visible stall: move during real traffic and the user rides an
 * unproven port; move on a batch nothing answers and the port can never be
 * confirmed; move on a cold-start init and the user's first packets wait
 * behind a handshake to a port nobody vouched for; parse "2h" as two seconds
 * and the tunnel hops all night. So every edge is pinned here,
 * including the unsigned wrap. */
#include <stdint.h>
#include "test.h"
#include "proxy.h"

static void test_parse_time_spans(void) {
    uint32_t lo, hi;
    ASSERT_EQ(rot_parse_span("30m-2h", 1, &lo, &hi), 0);
    ASSERT_EQ(lo, 1800u);
    ASSERT_EQ(hi, 7200u);
    ASSERT_EQ(rot_parse_span("1800-7200", 1, &lo, &hi), 0);
    ASSERT_EQ(lo, 1800u);
    ASSERT_EQ(hi, 7200u);
    ASSERT_EQ(rot_parse_span("45m", 1, &lo, &hi), 0);
    ASSERT_EQ(lo, 2700u);
    ASSERT_EQ(hi, 2700u);
    ASSERT_EQ(rot_parse_span("90s", 1, &lo, &hi), 0);
    ASSERT_EQ(lo, 90u);
}

static void test_parse_volume_spans(void) {
    uint32_t lo, hi;
    ASSERT_EQ(rot_parse_span("1000", 0, &lo, &hi), 0);
    ASSERT_EQ(lo, 1000u);
    ASSERT_EQ(hi, 1000u);
    ASSERT_EQ(rot_parse_span("800-1200", 0, &lo, &hi), 0);
    ASSERT_EQ(lo, 800u);
    ASSERT_EQ(hi, 1200u);
    ASSERT_EQ(rot_parse_span("1G", 0, &lo, &hi), 0);
    ASSERT_EQ(lo, 1024u);
    ASSERT_EQ(rot_parse_span("500MB-2GB", 0, &lo, &hi), 0);
    ASSERT_EQ(lo, 500u);
    ASSERT_EQ(hi, 2048u);
}

static void test_parse_rejects(void) {
    static const char *bad[] = { "", "abc", "0", "10-5", "5-", "-5", "30x",
                                 "1h-30m", "1-2-3", "99999999999" };
    for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        uint32_t lo = 7, hi = 7;
        ASSERT_EQ(rot_parse_span(bad[i], 1, &lo, &hi), -1);
        /* A rejected value must read as "off", never as half a span. */
        ASSERT_EQ(lo, 0u);
        ASSERT_EQ(hi, 0u);
    }
}

static void test_cfg_ticks(void) {
    rot_cfg_t c = rot_cfg_make(1800, 7200, 0, 0);
    ASSERT_EQ(c.t_lo, 360u);
    ASSERT_EQ(c.t_hi, 1440u);
    ASSERT_EQ(c.idle, (uint32_t)(ROT_IDLE_S / 5));
    ASSERT_EQ(c.retry, (uint32_t)ROT_RETRY_TICKS);
    ASSERT_EQ(c.force, (uint32_t)(ROT_FORCE_S / 5));

    /* A timer shorter than the idle wait and the back-off shrinks both. */
    c = rot_cfg_make(5, 5, 0, 0);
    ASSERT_EQ(c.t_lo, 1u);
    ASSERT_EQ(c.idle, 1u);
    ASSERT_EQ(c.retry, 1u);
    ASSERT_EQ(c.force, 1u);

    /* Volume alone keeps the stock idle wait. */
    c = rot_cfg_make(0, 0, 1000, 1000);
    ASSERT_EQ(c.t_lo, 0u);
    ASSERT_EQ(c.idle, (uint32_t)(ROT_IDLE_S / 5));
    ASSERT_EQ(c.force, (uint32_t)(ROT_FORCE_S / 5));
}

static void test_draw_stays_in_span(void) {
    fastrand_t r;
    fastrand_init(&r, 12345);
    int saw_lo = 0, saw_hi = 0;
    for (int i = 0; i < 2000; i++) {
        uint32_t v = rot_draw(10, 13, &r);
        ASSERT(v >= 10 && v <= 13);
        saw_lo |= v == 10;
        saw_hi |= v == 13;
    }
    ASSERT(saw_lo && saw_hi);
    ASSERT_EQ(rot_draw(42, 42, &r), 42u);
    /* The widest volume span must not overflow the modulus. */
    uint32_t w = rot_draw(1, 0xFFFFFFFFu, &r);
    ASSERT(w >= 1);
}

static void test_due_by_timer(void) {
    fastrand_t r;
    fastrand_init(&r, 1);
    rot_cfg_t c = rot_cfg_make(1800, 7200, 0, 0);
    rot_sched_t s;
    rot_sched_reset(&s, &c, 100, &r);
    ASSERT(s.deadline >= 100 + c.t_lo && s.deadline <= 100 + c.t_hi);
    ASSERT_EQ(rot_is_due(&c, &s, s.deadline - 1), 0);
    ASSERT_EQ(rot_is_due(&c, &s, s.deadline), 1);
    /* Traffic has no say without a volume span. */
    s.bytes = 1ull << 40;
    ASSERT_EQ(rot_is_due(&c, &s, s.deadline - 1), 0);
}

static void test_due_by_volume(void) {
    fastrand_t r;
    fastrand_init(&r, 2);
    rot_cfg_t c = rot_cfg_make(0, 0, 1000, 1000);
    rot_sched_t s;
    rot_sched_reset(&s, &c, 0, &r);
    ASSERT_EQ(s.vol, 1000ull << 20);
    s.bytes = s.vol - 1;
    ASSERT_EQ(rot_is_due(&c, &s, 1u << 30), 0);    /* no timer: time alone never */
    s.bytes = s.vol;
    ASSERT_EQ(rot_is_due(&c, &s, 1), 1);
}

static void test_due_whichever_first(void) {
    fastrand_t r;
    fastrand_init(&r, 3);
    rot_cfg_t c = rot_cfg_make(1800, 7200, 1000, 1000);
    rot_sched_t s;
    rot_sched_reset(&s, &c, 0, &r);
    s.bytes = s.vol;
    ASSERT_EQ(rot_is_due(&c, &s, 1), 1);
    s.bytes = 0;
    ASSERT_EQ(rot_is_due(&c, &s, s.deadline), 1);
}

static void test_retry_holds_the_flag(void) {
    fastrand_t r;
    fastrand_init(&r, 4);
    rot_cfg_t c = rot_cfg_make(0, 0, 1000, 1000);
    rot_sched_t s;
    rot_sched_reset(&s, &c, 0, &r);
    s.bytes = s.vol;                  /* still over after a silent port... */
    s.retry = 50 + c.retry;
    ASSERT_EQ(rot_is_due(&c, &s, 50), 0);           /* ...but backing off */
    ASSERT_EQ(rot_is_due(&c, &s, 50 + c.retry), 1);
}

/* A baseline window: 30+ s without a busy tick, the client's keepalive one
 * tick ago. */
static const uint32_t NOW = 1000;

static void test_window_opens_on_user_packet(void) {
    rot_cfg_t c = rot_cfg_make(1800, 7200, 0, 0);
    ASSERT_EQ(rot_window(&c, NOW, NOW - 7, NOW - 1, ROT_PKT_USER), 1);
    /* Somebody's packet is its own proof of a live session. */
    ASSERT_EQ(rot_window(&c, NOW, NOW - 7, 0, ROT_PKT_USER), 1);
}

static void test_window_idle_is_strict(void) {
    rot_cfg_t c = rot_cfg_make(1800, 7200, 0, 0);
    ASSERT_EQ(rot_window(&c, NOW, NOW, NOW, ROT_PKT_USER), 0);
    /* Busy six ticks back may have been at that tick's end: 25 s, short. */
    ASSERT_EQ(rot_window(&c, NOW, NOW - 6, NOW, ROT_PKT_USER), 0);
    ASSERT_EQ(rot_window(&c, NOW, NOW - 7, NOW, ROT_PKT_USER), 1);
}

static void test_window_keepalives_cannot_vouch(void) {
    rot_cfg_t c = rot_cfg_make(1800, 7200, 0, 0);
    /* Nothing in the batch draws an answer: no way to confirm a new port. */
    ASSERT_EQ(rot_window(&c, NOW, NOW - 50, NOW, 0), 0);
}

static void test_window_init_needs_live_session(void) {
    rot_cfg_t c = rot_cfg_make(1800, 7200, 0, 0);
    /* Silent for longer than a persistent keepalive: this init has user
     * packets queued behind it. */
    ASSERT_EQ(rot_window(&c, NOW, 0, NOW - ROT_WARM_TICKS - 1, ROT_PKT_INIT), 0);
    ASSERT_EQ(rot_window(&c, NOW, 0, NOW - ROT_WARM_TICKS, ROT_PKT_INIT), 1);
    ASSERT_EQ(rot_window(&c, NOW, 0, 0, ROT_PKT_INIT | ROT_PKT_USER), 1);
}

static void test_window_fresh_start(void) {
    rot_cfg_t c = rot_cfg_make(1800, 7200, 0, 0);
    /* Every stamp is zero at start: tick 0 counts as busy. */
    ASSERT_EQ(rot_window(&c, 0, 0, 0, ROT_PKT_USER), 0);
    ASSERT_EQ(rot_window(&c, c.idle, 0, c.idle, ROT_PKT_USER), 0);
    ASSERT_EQ(rot_window(&c, c.idle + 1, 0, c.idle + 1, ROT_PKT_USER), 1);
}

static void test_window_tick_wrap(void) {
    rot_cfg_t c = rot_cfg_make(1800, 7200, 0, 0);
    uint32_t now = 3;
    ASSERT_EQ(rot_window(&c, now, now - 10, now - 1, ROT_PKT_USER), 1);
    ASSERT_EQ(rot_window(&c, now, now - 2, now - 1, ROT_PKT_USER), 0);
}

int main(void) {
    RUN_TEST(parse_time_spans);
    RUN_TEST(parse_volume_spans);
    RUN_TEST(parse_rejects);
    RUN_TEST(cfg_ticks);
    RUN_TEST(draw_stays_in_span);
    RUN_TEST(due_by_timer);
    RUN_TEST(due_by_volume);
    RUN_TEST(due_whichever_first);
    RUN_TEST(retry_holds_the_flag);
    RUN_TEST(window_opens_on_user_packet);
    RUN_TEST(window_idle_is_strict);
    RUN_TEST(window_keepalives_cannot_vouch);
    RUN_TEST(window_init_needs_live_session);
    RUN_TEST(window_fresh_start);
    RUN_TEST(window_tick_wrap);
    TEST_MAIN_END();
}
