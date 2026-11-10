/* Perf knobs against the hardware the container actually got (#68).
 *
 * The set AWG_CPU_C2S/S2C + AWG_RT + AWG_RPS is built for four cores and more.
 * Carried by hand onto a two-core router it takes the CPU from WireGuard, so the
 * container has to drop it there by itself — and on a big router it must keep
 * every bit of it, because there it is what removes the last drops. */
#include <stdint.h>
#include <string.h>
#include <arpa/inet.h>
#include "test.h"
#include "proxy.h"

static awg_config_t cfg;

/* The configurator's four-core set. */
static void four_core_set(void) {
    memset(&cfg, 0, sizeof(cfg));
    cfg.cpu_c2s = 1;
    cfg.cpu_s2c = 2;
    cfg.rt_prio = 10;
    strcpy(cfg.rps_mask, "9");
}

static void test_untouched_on_four_cores(void) {
    four_core_set();
    ASSERT_EQ(perf_fit_hw(&cfg, 4, 0xF, 1), 0);
    ASSERT_EQ(cfg.cpu_c2s, 1);
    ASSERT_EQ(cfg.cpu_s2c, 2);
    ASSERT_EQ(cfg.rt_prio, 10);
    ASSERT(strcmp(cfg.rps_mask, "9") == 0);
}

static void test_dropped_whole_on_two_cores(void) {
    four_core_set();
    ASSERT_EQ(perf_fit_hw(&cfg, 2, 0x3, 1), PERF_FEW_CPUS);
    ASSERT_EQ(cfg.cpu_c2s, -1);
    ASSERT_EQ(cfg.cpu_s2c, -1);
    ASSERT_EQ(cfg.rt_prio, 0);
    ASSERT_EQ(cfg.rps_mask[0], 0);
}

static void test_rt_alone_dropped_on_two_cores(void) {
    /* Real-time without pins is the worst of it on two cores: nothing is left
     * for the workers that do WireGuard's crypto. */
    memset(&cfg, 0, sizeof(cfg));
    cfg.cpu_c2s = cfg.cpu_s2c = -1;
    cfg.rt_prio = 10;
    ASSERT_EQ(perf_fit_hw(&cfg, 2, 0x3, 1), PERF_FEW_CPUS);
    ASSERT_EQ(cfg.rt_prio, 0);
}

static void test_dropped_whole_on_32bit(void) {
    /* hAP ac²: four cores, but Cortex-A7 in 32-bit mode. The pinned real-time
     * thread starved WireGuard there (upload 100 -> 34 Mbit/s). */
    four_core_set();
    ASSERT_EQ(perf_fit_hw(&cfg, 4, 0xF, 0), PERF_32BIT);
    ASSERT_EQ(cfg.cpu_c2s, -1);
    ASSERT_EQ(cfg.cpu_s2c, -1);
    ASSERT_EQ(cfg.rt_prio, 0);
    ASSERT_EQ(cfg.rps_mask[0], 0);
    /* two 32-bit cores: the core count is the reason given */
    four_core_set();
    ASSERT_EQ(perf_fit_hw(&cfg, 2, 0x3, 0), PERF_FEW_CPUS);
    /* nothing set: nothing to say, whatever the CPU */
    memset(&cfg, 0, sizeof(cfg));
    cfg.cpu_c2s = cfg.cpu_s2c = -1;
    ASSERT_EQ(perf_fit_hw(&cfg, 4, 0xF, 0), 0);
}

static void test_strong_class(void) {
    ASSERT(hw_strong(4, 1));             /* hAP ax², ax³ */
    ASSERT(hw_strong(8, 1));
    ASSERT(!hw_strong(4, 0));            /* hAP ac²: four cores, 32-bit */
    ASSERT(!hw_strong(2, 1));            /* two 64-bit cores */
    ASSERT(!hw_strong(2, 0));            /* hAP ax lite, hEX S 2025 */
    ASSERT(!hw_strong(1, 1));
}

static void test_container_cpu_list_counts(void) {
    /* A four-core router, but cpu-list gave the container two of them. */
    four_core_set();
    ASSERT_EQ(perf_fit_hw(&cfg, 2, 0xC, 1), PERF_FEW_CPUS);
    ASSERT_EQ(cfg.rt_prio, 0);
}

static void test_nothing_set_nothing_said(void) {
    memset(&cfg, 0, sizeof(cfg));
    cfg.cpu_c2s = cfg.cpu_s2c = -1;
    ASSERT_EQ(perf_fit_hw(&cfg, 1, 0x1, 1), 0);
    ASSERT_EQ(perf_fit_hw(&cfg, 8, 0xFF, 1), 0);
}

static void test_pin_to_missing_core(void) {
    four_core_set();
    cfg.cpu_s2c = 5;
    strcpy(cfg.rps_mask, "1");
    ASSERT_EQ(perf_fit_hw(&cfg, 4, 0xF, 1), PERF_NO_S2C_CPU);
    ASSERT_EQ(cfg.cpu_c2s, 1);
    ASSERT_EQ(cfg.cpu_s2c, -1);
    ASSERT_EQ(cfg.rt_prio, 10);
    /* a core outside what the mask can even say */
    four_core_set();
    cfg.cpu_c2s = 40;
    strcpy(cfg.rps_mask, "1");
    ASSERT_EQ(perf_fit_hw(&cfg, 4, 0xF, 1), PERF_NO_C2S_CPU);
    ASSERT_EQ(cfg.cpu_c2s, -1);
}

static void test_pin_outside_cpu_list(void) {
    /* Six cores, but the container is limited to 2..5: core 1 is not ours. */
    four_core_set();
    cfg.cpu_s2c = 3;
    strcpy(cfg.rps_mask, "1");
    ASSERT_EQ(perf_fit_hw(&cfg, 4, 0x3C, 1), PERF_NO_C2S_CPU);
    ASSERT_EQ(cfg.cpu_c2s, -1);
    ASSERT_EQ(cfg.cpu_s2c, 3);
}

static void test_rps_overlap_trimmed(void) {
    four_core_set();
    strcpy(cfg.rps_mask, "f");           /* all four, threads on 1 and 2 */
    ASSERT_EQ(perf_fit_hw(&cfg, 4, 0xF, 1), PERF_RPS_TRIMMED);
    ASSERT(strcmp(cfg.rps_mask, "9") == 0);
    four_core_set();
    strcpy(cfg.rps_mask, "E");           /* upper case, cores 1..3 */
    ASSERT_EQ(perf_fit_hw(&cfg, 4, 0xF, 1), PERF_RPS_TRIMMED);
    ASSERT(strcmp(cfg.rps_mask, "8") == 0);
}

static void test_rps_only_on_pinned_cores_dropped(void) {
    four_core_set();
    strcpy(cfg.rps_mask, "6");
    ASSERT_EQ(perf_fit_hw(&cfg, 4, 0xF, 1), PERF_RPS_DROPPED);
    ASSERT_EQ(cfg.rps_mask[0], 0);
    ASSERT_EQ(cfg.rt_prio, 10);
}

static void test_rps_trim_follows_dropped_pin(void) {
    /* The pin that went away no longer reserves its core. */
    four_core_set();
    cfg.cpu_s2c = 7;
    strcpy(cfg.rps_mask, "f");
    ASSERT_EQ(perf_fit_hw(&cfg, 4, 0xF, 1), PERF_NO_S2C_CPU | PERF_RPS_TRIMMED);
    ASSERT(strcmp(cfg.rps_mask, "d") == 0);
}

static void test_rps_wide_or_odd_left_alone(void) {
    four_core_set();
    strcpy(cfg.rps_mask, "ff,ffffffff");
    ASSERT_EQ(perf_fit_hw(&cfg, 4, 0xF, 1), 0);
    ASSERT(strcmp(cfg.rps_mask, "ff,ffffffff") == 0);
    four_core_set();
    strcpy(cfg.rps_mask, "1ffffffff");   /* nine digits */
    ASSERT_EQ(perf_fit_hw(&cfg, 4, 0xF, 1), 0);
}

static void test_rps_high_bits_rendered(void) {
    four_core_set();
    cfg.cpu_c2s = 0;
    cfg.cpu_s2c = 31;
    strcpy(cfg.rps_mask, "80000003");
    ASSERT_EQ(perf_fit_hw(&cfg, 32, 0xFFFFFFFFu, 1), PERF_RPS_TRIMMED);
    ASSERT(strcmp(cfg.rps_mask, "2") == 0);
    cfg.cpu_c2s = 1;
    cfg.cpu_s2c = 0;
    strcpy(cfg.rps_mask, "f0000003");
    ASSERT_EQ(perf_fit_hw(&cfg, 32, 0xFFFFFFFFu, 1), PERF_RPS_TRIMMED);
    ASSERT(strcmp(cfg.rps_mask, "f0000000") == 0);
}

/* ---- the core WireGuard sends this tunnel from ---- */

static void test_auto_defaults_top_two_cores(void) {
    memset(&cfg, 0, sizeof(cfg));
    perf_auto_defaults(&cfg, 0xF);
    ASSERT_EQ(cfg.cpu_c2s, 3);
    ASSERT_EQ(cfg.cpu_s2c, 2);
    ASSERT_EQ(cfg.rt_prio, PERF_AUTO_RT);
    ASSERT(strcmp(cfg.rps_mask, "3") == 0);
    /* cpu-list gave cores 2..5 of six */
    memset(&cfg, 0, sizeof(cfg));
    perf_auto_defaults(&cfg, 0x3C);
    ASSERT_EQ(cfg.cpu_c2s, 5);
    ASSERT_EQ(cfg.cpu_s2c, 4);
    ASSERT(strcmp(cfg.rps_mask, "c") == 0);
}

static void test_auto_set_on_each_wg_core(void) {
    /* The bottom of the four cores: only the receive mask changes. */
    memset(&cfg, 0, sizeof(cfg));
    perf_auto_defaults(&cfg, 0xF);
    ASSERT_EQ(perf_avoid_wg_core(&cfg, 1, 0xF), PERF_MOVED_RPS);
    ASSERT_EQ(cfg.cpu_c2s, 3);
    ASSERT_EQ(cfg.cpu_s2c, 2);
    ASSERT(strcmp(cfg.rps_mask, "1") == 0);

    memset(&cfg, 0, sizeof(cfg));
    perf_auto_defaults(&cfg, 0xF);
    ASSERT_EQ(perf_avoid_wg_core(&cfg, 0, 0xF), PERF_MOVED_RPS);
    ASSERT(strcmp(cfg.rps_mask, "2") == 0);

    /* The top two: the thread sitting there has to go. */
    memset(&cfg, 0, sizeof(cfg));
    perf_auto_defaults(&cfg, 0xF);
    ASSERT_EQ(perf_avoid_wg_core(&cfg, 3, 0xF), PERF_MOVED_C2S | PERF_MOVED_RPS);
    ASSERT_EQ(cfg.cpu_c2s, 1);
    ASSERT_EQ(cfg.cpu_s2c, 2);
    ASSERT(strcmp(cfg.rps_mask, "1") == 0);

    memset(&cfg, 0, sizeof(cfg));
    perf_auto_defaults(&cfg, 0xF);
    ASSERT_EQ(perf_avoid_wg_core(&cfg, 2, 0xF), PERF_MOVED_S2C | PERF_MOVED_RPS);
    ASSERT_EQ(cfg.cpu_c2s, 3);
    ASSERT_EQ(cfg.cpu_s2c, 1);
    ASSERT(strcmp(cfg.rps_mask, "1") == 0);
}

static void test_old_configurator_set_on_each_wg_core(void) {
    /* 1/2/9 is what every installation so far carries. The measured case: WireGuard
     * on core 1 gave 110 Mbit/s upload, and this placement gave 217. */
    four_core_set();
    ASSERT_EQ(perf_avoid_wg_core(&cfg, 1, 0xF), PERF_MOVED_C2S | PERF_MOVED_RPS);
    ASSERT_EQ(cfg.cpu_c2s, 3);
    ASSERT_EQ(cfg.cpu_s2c, 2);
    ASSERT(strcmp(cfg.rps_mask, "1") == 0);

    four_core_set();
    ASSERT_EQ(perf_avoid_wg_core(&cfg, 2, 0xF), PERF_MOVED_S2C | PERF_MOVED_RPS);
    ASSERT_EQ(cfg.cpu_c2s, 1);
    ASSERT_EQ(cfg.cpu_s2c, 3);
    ASSERT(strcmp(cfg.rps_mask, "1") == 0);

    /* No collision: the threads stay, the mask only loses WireGuard's core. */
    four_core_set();
    ASSERT_EQ(perf_avoid_wg_core(&cfg, 0, 0xF), PERF_MOVED_RPS);
    ASSERT_EQ(cfg.cpu_c2s, 1);
    ASSERT_EQ(cfg.cpu_s2c, 2);
    ASSERT(strcmp(cfg.rps_mask, "8") == 0);

    four_core_set();
    ASSERT_EQ(perf_avoid_wg_core(&cfg, 3, 0xF), PERF_MOVED_RPS);
    ASSERT(strcmp(cfg.rps_mask, "1") == 0);
}

static void test_avoid_nothing_to_do(void) {
    /* Pins only, no mask, no collision: not a single change. */
    four_core_set();
    cfg.rps_mask[0] = 0;
    ASSERT_EQ(perf_avoid_wg_core(&cfg, 0, 0xF), 0);
    ASSERT_EQ(cfg.rps_mask[0], 0);
    /* A mask made of the taken cores only goes away whole. */
    four_core_set();
    strcpy(cfg.rps_mask, "1");
    ASSERT_EQ(perf_avoid_wg_core(&cfg, 0, 0xF), PERF_MOVED_RPS);
    ASSERT_EQ(cfg.rps_mask[0], 0);
    /* Nonsense from the socket changes nothing. */
    four_core_set();
    ASSERT_EQ(perf_avoid_wg_core(&cfg, -1, 0xF), 0);
    ASSERT_EQ(perf_avoid_wg_core(&cfg, 99, 0xF), 0);
    ASSERT_EQ(cfg.cpu_c2s, 1);
    ASSERT(strcmp(cfg.rps_mask, "9") == 0);
}

static void test_avoid_with_no_free_core(void) {
    /* Nowhere to go: stay put rather than unpin into the unknown. */
    memset(&cfg, 0, sizeof(cfg));
    cfg.cpu_c2s = 0;
    cfg.cpu_s2c = 1;
    ASSERT_EQ(perf_avoid_wg_core(&cfg, 0, 0x3), 0);
    ASSERT_EQ(cfg.cpu_c2s, 0);
    ASSERT_EQ(cfg.cpu_s2c, 1);
}

/* ---- sender threads (AWG_TX_*) ---- */

static void test_auto_defaults_place_sender(void) {
    /* The sender takes the next core down; the receive mask keeps the rest. */
    memset(&cfg, 0, sizeof(cfg));
    cfg.tx_on[0] = 1;
    cfg.cpu_tx[0] = -1;
    perf_auto_defaults(&cfg, 0xF);
    ASSERT_EQ(cfg.cpu_c2s, 3);
    ASSERT_EQ(cfg.cpu_s2c, 2);
    ASSERT_EQ(cfg.cpu_tx[0], 1);
    /* the receiver loses real-time, and RPS joins it on its core */
    ASSERT_EQ(thr_rt(&cfg, 0), 0);
    ASSERT_EQ(thr_rt(&cfg, 1), PERF_AUTO_RT);
    ASSERT_EQ(thr_rt(&cfg, 2), PERF_AUTO_RT);
    ASSERT(strcmp(cfg.rps_mask, "9") == 0);
    /* unset (-1): the auto set turns the sender on */
    memset(&cfg, 0, sizeof(cfg));
    cfg.tx_on[0] = -1;
    cfg.cpu_tx[0] = -1;
    perf_auto_defaults(&cfg, 0xF);
    ASSERT_EQ(cfg.tx_on[0], 1);
    ASSERT_EQ(cfg.cpu_tx[0], 1);
    /* AWG_RT_C2S given by hand stays */
    memset(&cfg, 0, sizeof(cfg));
    cfg.tx_on[0] = 1;
    cfg.cpu_tx[0] = -1;
    cfg.rt_thr[0] = 5 + 1;
    perf_auto_defaults(&cfg, 0xF);
    ASSERT_EQ(thr_rt(&cfg, 0), 5);
    ASSERT(strcmp(cfg.rps_mask, "1") == 0);
    /* An off sender keeps its zeroed pin out of the way. */
    memset(&cfg, 0, sizeof(cfg));
    perf_auto_defaults(&cfg, 0xF);
    ASSERT(strcmp(cfg.rps_mask, "3") == 0);
}

static void test_sender_moved_off_wg_core(void) {
    memset(&cfg, 0, sizeof(cfg));
    cfg.tx_on[0] = 1;
    cfg.cpu_tx[0] = -1;
    perf_auto_defaults(&cfg, 0xF);              /* 3, 2, sender 1, mask 9 */
    ASSERT_EQ(perf_avoid_wg_core(&cfg, 1, 0xF), PERF_MOVED_TXC | PERF_MOVED_RPS);
    ASSERT_EQ(cfg.cpu_tx[0], 0);
    ASSERT(strcmp(cfg.rps_mask, "8") == 0);     /* the receiver's core only */
    /* WireGuard on the receiver's core (the stand on a hAP ax²): it moves to the
     * free core and RPS follows it there. */
    memset(&cfg, 0, sizeof(cfg));
    cfg.tx_on[0] = 1;
    cfg.cpu_tx[0] = -1;
    perf_auto_defaults(&cfg, 0xF);
    ASSERT_EQ(perf_avoid_wg_core(&cfg, 3, 0xF), PERF_MOVED_C2S | PERF_MOVED_RPS);
    ASSERT_EQ(cfg.cpu_c2s, 0);
    ASSERT(strcmp(cfg.rps_mask, "1") == 0);
    /* An off sender never moves, whatever its pin says. */
    memset(&cfg, 0, sizeof(cfg));
    perf_auto_defaults(&cfg, 0xF);
    ASSERT_EQ(perf_avoid_wg_core(&cfg, 0, 0xF), PERF_MOVED_RPS);
    ASSERT_EQ(cfg.cpu_tx[0], 0);
}

static void test_rps_kept_on_non_rt_thread(void) {
    /* RPS loses to a real-time thread only: next to a plain one it may stay. */
    four_core_set();
    cfg.rt_thr[0] = 0 + 1;                      /* AWG_RT_C2S=0 */
    strcpy(cfg.rps_mask, "3");                  /* cores 0, 1; c2s on 1 */
    ASSERT_EQ(perf_fit_hw(&cfg, 4, 0xF, 1), 0);
    ASSERT(strcmp(cfg.rps_mask, "3") == 0);
    ASSERT_EQ(perf_avoid_wg_core(&cfg, 0, 0xF), PERF_MOVED_RPS);
    ASSERT(strcmp(cfg.rps_mask, "2") == 0);
    ASSERT_EQ(thr_rt(&cfg, 0), 0);
    ASSERT_EQ(thr_rt(&cfg, 1), 10);
    ASSERT_EQ(thr_rt(&cfg, 3), 10);             /* senders follow AWG_RT */
}

static void test_weak_hw_drops_sender_pin(void) {
    four_core_set();
    cfg.tx_on[0] = 1;
    cfg.cpu_tx[0] = 3;
    cfg.rt_thr[2] = 11;
    ASSERT_EQ(perf_fit_hw(&cfg, 2, 0x3, 1), PERF_FEW_CPUS);
    ASSERT_EQ(cfg.cpu_tx[0], -1);
    ASSERT_EQ(cfg.tx_on[0], 1);
    ASSERT_EQ(thr_rt(&cfg, 2), 0);
    /* a sender pin outside the cpu-list is unpinned */
    four_core_set();
    cfg.tx_on[1] = 1;
    cfg.cpu_tx[1] = 5;
    perf_fit_hw(&cfg, 4, 0xF, 1);
    ASSERT_EQ(cfg.cpu_tx[1], -1);
}

/* ---- softnet_stat ---- */

static void test_softnet_parse(void) {
    /* 5.6 has no cpu index column; rows are the online CPUs in order. */
    static const char t[] =
        "0001a2b3 00000000 00000004 00000000 00000000 00000000 00000000 00000000 00000000 00000000 00000000\n"
        "0000ffff 0001e8a2 0000000c 00000000 00000000 00000000 00000000 00000000 00000000 00000000 00000000\n"
        "00000010 00000001 00000000 00000000 00000000 00000000 00000000 00000000 00000000 00000000 00000000 00000000 00000002\n";
    softnet_t s;
    ASSERT_EQ(softnet_parse(t, (int)sizeof(t) - 1, &s), 3);
    ASSERT_EQ(s.drop[0], 0u);
    ASSERT_EQ(s.sq[0], 4u);
    ASSERT_EQ(s.drop[1], 125090u);
    ASSERT_EQ(s.sq[1], 12u);
    ASSERT_EQ(s.drop[2], 1u);
    /* a cut-off last row (short read) is not a CPU */
    ASSERT_EQ(softnet_parse(t, 120, &s), 1);
    ASSERT_EQ(softnet_parse("", 0, &s), 0);
}

/* ---- receive buffer sized by the traffic ---- */

#define CAP (16 * 1024 * 1024)

static void test_rb_floor_is_the_old_buffer(void) {
    /* An idle or slow tunnel gets exactly what it had before 1.4.0. */
    ASSERT_EQ(rb_for_rate(0, CAP), RB_FLOOR);
    ASSERT_EQ(rb_for_rate(1000, CAP), RB_FLOOR);
}

static void test_rb_holds_the_queue_in_time(void) {
    /* 10 000 pps (about 120 Mbit/s): 50 ms of it is 500 datagrams, and the
     * kernel doubles the request, so the request is half of 500 * truesize. */
    ASSERT_EQ(rb_for_rate(10000, CAP), 500 * RB_TRUESIZE / 2);
    /* twice the rate, twice the buffer — the queue stays 50 ms */
    ASSERT_EQ(rb_for_rate(20000, CAP), 2 * rb_for_rate(10000, CAP));
}

static void test_rb_ceiling(void) {
    ASSERT_EQ(rb_for_rate(1000000, CAP), CAP);
    ASSERT_EQ(rb_for_rate(0xFFFFFFFFu, CAP), CAP);   /* no overflow */
}

static rb_ctl_t rb;
static uint32_t ctr;

/* one 5 s tick at `pps`; returns what rb_step returned */
static int tick(uint32_t pps) {
    ctr += pps * 5;
    return rb_step(&rb, ctr, 5, CAP);
}

static void rb_reset(void) {
    ctr = 0xFFFFF000u;                   /* wraps on the first busy tick */
    rb.pv_pkts = ctr;
    rb.peak_pps = 0;
    rb.req = RB_FLOOR;
}

static void test_rb_grows_at_once(void) {
    rb_reset();
    ASSERT_EQ(tick(0), 0);               /* idle: stays at the floor, no syscall */
    ASSERT_EQ(tick(10000), rb_for_rate(10000, CAP));
    ASSERT_EQ(rb.req, rb_for_rate(10000, CAP));
}

static void test_rb_wobble_costs_nothing(void) {
    rb_reset();
    tick(10000);
    ASSERT_EQ(tick(10500), 0);           /* +5 %: not worth a setsockopt */
    ASSERT_EQ(tick(9000), 0);            /* peak decays a little, still within a quarter */
}

static void test_rb_decays_slowly(void) {
    rb_reset();
    tick(20000);
    int big = rb.req;
    ASSERT_EQ(tick(0), 0);               /* one quiet tick keeps the room */
    int moved = 0, ticks = 0;
    while (rb.req > RB_FLOOR && ticks < 60) {
        if (tick(0)) moved++;
        ticks++;
    }
    ASSERT(rb.req == RB_FLOOR || rb.req < big / 4);
    ASSERT(ticks >= 4);                  /* not in the next 20 seconds */
    ASSERT(moved <= 12);                 /* and in steps, not a syscall per tick */
}

static void test_rb_regrows_after_decay(void) {
    rb_reset();
    tick(20000);
    for (int i = 0; i < 40; i++) tick(0);
    ASSERT(rb.req < rb_for_rate(20000, CAP));
    ASSERT_EQ(tick(20000), rb_for_rate(20000, CAP));
}

/* A socket connected while the veth address is tentative keeps the link-local
 * source, and the tunnel stays silent for all of AWG_TIMEOUT. */
static int v6_ok(const char *src, const char *dst) {
    struct in6_addr s, d;
    inet_pton(AF_INET6, src, &s);
    inet_pton(AF_INET6, dst, &d);
    return awg_v6_src_usable(&s, &d);
}

static void test_v6_source_not_ready(void) {
    ASSERT(!v6_ok("fe80::1", "2a0a:4cc0:40::1"));
    ASSERT(!v6_ok("::", "2a0a:4cc0:40::1"));
    ASSERT(!v6_ok("::1", "2a0a:4cc0:40::1"));
    ASSERT(v6_ok("fdcc:2222:22::2", "2a0a:4cc0:40::1"));
    ASSERT(v6_ok("2a02:6b8::5", "2a0a:4cc0:40::1"));
}

static void test_v6_source_link_local_peer(void) {
    ASSERT(v6_ok("fe80::1", "fe80::2"));
    ASSERT(v6_ok("::1", "::1"));
}

int main(void) {
    printf("=== perf fit tests ===\n");
    RUN_TEST(rb_floor_is_the_old_buffer);
    RUN_TEST(rb_holds_the_queue_in_time);
    RUN_TEST(rb_ceiling);
    RUN_TEST(rb_grows_at_once);
    RUN_TEST(rb_wobble_costs_nothing);
    RUN_TEST(rb_decays_slowly);
    RUN_TEST(rb_regrows_after_decay);
    RUN_TEST(untouched_on_four_cores);
    RUN_TEST(dropped_whole_on_two_cores);
    RUN_TEST(dropped_whole_on_32bit);
    RUN_TEST(strong_class);
    RUN_TEST(rt_alone_dropped_on_two_cores);
    RUN_TEST(container_cpu_list_counts);
    RUN_TEST(nothing_set_nothing_said);
    RUN_TEST(pin_to_missing_core);
    RUN_TEST(pin_outside_cpu_list);
    RUN_TEST(rps_overlap_trimmed);
    RUN_TEST(rps_only_on_pinned_cores_dropped);
    RUN_TEST(rps_trim_follows_dropped_pin);
    RUN_TEST(rps_wide_or_odd_left_alone);
    RUN_TEST(rps_high_bits_rendered);
    RUN_TEST(auto_defaults_top_two_cores);
    RUN_TEST(auto_set_on_each_wg_core);
    RUN_TEST(old_configurator_set_on_each_wg_core);
    RUN_TEST(avoid_nothing_to_do);
    RUN_TEST(avoid_with_no_free_core);
    RUN_TEST(auto_defaults_place_sender);
    RUN_TEST(sender_moved_off_wg_core);
    RUN_TEST(rps_kept_on_non_rt_thread);
    RUN_TEST(weak_hw_drops_sender_pin);
    RUN_TEST(softnet_parse);
    RUN_TEST(v6_source_not_ready);
    RUN_TEST(v6_source_link_local_peer);
    TEST_MAIN_END();
}
