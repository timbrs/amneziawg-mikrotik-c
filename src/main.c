#include "proxy.h"
#include "cps.h"
#include "log.h"
#include "base64.h"
#include "csprng.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifndef VERSION
#define VERSION "dev"
#endif

static int slen(const char *s) { int n = 0; while (s[n]) n++; return n; }

static int parse_int_str(const char *s) {
    int v = 0, neg = 0;
    if (*s == '-') { neg = 1; s++; }
    while (*s >= '0' && *s <= '9') { v = v * 10 + (*s - '0'); s++; }
    return neg ? -v : v;
}

/* awg-tools writes booleans as on/off and accepts plain numbers, so a .conf
 * carried over verbatim has to parse both. Anything else reads as off. */
static int parse_bool_str(const char *s) {
    if ((s[0] == 'o' || s[0] == 'O') && (s[1] == 'n' || s[1] == 'N') && !s[2]) return 1;
    if ((s[0] == 't' || s[0] == 'T') || (s[0] == 'y' || s[0] == 'Y')) return 1;
    return parse_int_str(s) != 0;
}

static uint32_t parse_uint32(const char *s) {
    uint32_t v = 0;
    while (*s >= '0' && *s <= '9') { v = v * 10 + (uint32_t)(*s - '0'); s++; }
    return v;
}

/* Parse "MIN-MAX" or "VALUE" into hrange_t */
static int parse_hrange(const char *s, hrange_t *r) {
    const char *dash = NULL;
    for (const char *p = s; *p; p++) {
        if (*p == '-') { dash = p; break; }
    }
    if (!dash) {
        r->min = r->max = parse_uint32(s);
    } else {
        char tmp[32];
        int hlen = (int)(dash - s);
        if (hlen >= 32) return -1;
        memcpy(tmp, s, hlen);
        tmp[hlen] = '\0';
        r->min = parse_uint32(tmp);
        r->max = parse_uint32(dash + 1);
        if (r->min > r->max) return -1;
    }
    return 0;
}

static int hex_nibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Decode a 32-byte key given as 64 hex chars (UAPI form) or 44 base64 chars
 * (.conf form). Returns 0 on success. */
static int decode_key32(const char *s, uint8_t out[32]) {
    int len = slen(s);
    if (len == 64) {
        for (int i = 0; i < 32; i++) {
            int hi = hex_nibble(s[i * 2]);
            int lo = hex_nibble(s[i * 2 + 1]);
            if (hi < 0 || lo < 0) return -1;
            out[i] = (uint8_t)((hi << 4) | lo);
        }
        return 0;
    }
    return base64_decode(s, len, out, 32) == 32 ? 0 : -1;
}

static const char *getenv_required(const char *name, int *err_count) {
    const char *v = getenv(name);
    if (!v || !v[0]) {
        const char *parts[] = { name, " is not set" };
        log_msgn("FATAL: ", parts, 2);
        (*err_count)++;
        return "";
    }
    return v;
}

static void fatal(const char *msg) {
    log_msg("FATAL: ", msg);
    _exit(1);
}

/* Profile-chain env names: stage 0 = AWG_*, 1 = AWG_FB_*, 2 = AWG_FB2_*, ... */
static const char *fb_name(char *buf, int stage, const char *suffix) {
    int i = 0;
    if (stage == 0) {
        for (const char *b = "AWG_"; *b; b++) buf[i++] = *b;
    } else {
        for (const char *b = "AWG_FB"; *b; b++) buf[i++] = *b;
        if (stage > 1) buf[i++] = (char)('0' + stage);
        buf[i++] = '_';
    }
    while (*suffix) buf[i++] = *suffix++;
    buf[i] = '\0';
    return buf;
}

static const char *fb_required(char *buf, int stage, const char *suffix) {
    char h1buf[32];
    const char *v = getenv(fb_name(buf, stage, suffix));
    if (!v || !v[0]) {
        const char *parts[] = { buf, " is required when ",
                                fb_name(h1buf, stage, "H1"), " is set" };
        log_msgn("FATAL: ", parts, 4);
        _exit(1);
    }
    return v;
}

static void fb_hrange(char *buf, int stage, const char *suffix, hrange_t *r) {
    if (parse_hrange(fb_required(buf, stage, suffix), r) < 0) {
        const char *parts[] = { buf, ": invalid range" };
        log_msgn("FATAL: ", parts, 2);
        _exit(1);
    }
}

static int is_pub_sep(char c) {
    return c == ',' || c == ';' || c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

static void add_server_peer_pub(awg_config_t *cfg, const uint8_t pub[32], const char *name) {
    for (int i = 0; i < cfg->server_peer_count; i++) {
        if (memcmp(cfg->server_peer_pubs[i], pub, 32) == 0)
            return;
    }
    if (cfg->server_peer_count >= AWG_MAX_SERVER_PEERS) {
        const char *parts[] = { name, ": too many peers (max ", "256", ")" };
        log_msgn("FATAL: ", parts, 4);
        _exit(1);
    }
    memcpy(cfg->server_peer_pubs[cfg->server_peer_count++], pub, 32);
}

static void parse_server_peer_list(awg_config_t *cfg, const char *name, const char *value) {
    const char *p = value;
    while (*p) {
        while (*p && is_pub_sep(*p)) p++;
        if (!*p) break;

        const char *start = p;
        while (*p && !is_pub_sep(*p)) p++;

        int len = (int)(p - start);
        if (len <= 0) continue;
        if (len >= 128) {
            const char *parts[] = { name, ": peer public key token is too long" };
            log_msgn("FATAL: ", parts, 2);
            _exit(1);
        }

        char token[128];
        uint8_t pub[32];
        memcpy(token, start, len);
        token[len] = '\0';

        if (base64_decode(token, len, pub, 32) != 32) {
            const char *parts[] = { name, ": invalid client public key in peer list" };
            log_msgn("FATAL: ", parts, 2);
            _exit(1);
        }
        add_server_peer_pub(cfg, pub, name);
    }
}

static void load_server_peer_file(awg_config_t *cfg, const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        const char *parts[] = { "AWG_CLIENT_PUBS_FILE: cannot open ", path };
        log_msgn("FATAL: ", parts, 2);
        _exit(1);
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        fatal("AWG_CLIENT_PUBS_FILE: cannot seek");
    }
    long size = ftell(f);
    if (size < 0) {
        fclose(f);
        fatal("AWG_CLIENT_PUBS_FILE: cannot stat");
    }
    if (fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        fatal("AWG_CLIENT_PUBS_FILE: cannot rewind");
    }
    char *buf = (char *)malloc((size_t)size + 1);
    if (!buf) {
        fclose(f);
        fatal("AWG_CLIENT_PUBS_FILE: out of memory");
    }
    if (size > 0 && fread(buf, 1, (size_t)size, f) != (size_t)size) {
        free(buf);
        fclose(f);
        fatal("AWG_CLIENT_PUBS_FILE: cannot read");
    }
    fclose(f);
    buf[size] = '\0';
    parse_server_peer_list(cfg, "AWG_CLIENT_PUBS_FILE", buf);
    free(buf);
}

static proxy_t g_proxy;
static awg_config_t g_config;
/* CPS templates storage, one set per profile-chain stage */
static cps_template_t g_cps_storage[AWG_MAX_PROFILES][5];

/* Load I1-I5 for one chain stage into pr->cps. */
static void load_cps_stage(int stage, awg_profile_t *pr) {
    static const char *suf[5] = { "I1", "I2", "I3", "I4", "I5" };
    char nb[32];

    for (int i = 0; i < 5; i++) {
        const char *name = fb_name(nb, stage, suf[i]);
        const char *v = getenv(name);
        if (!v || !v[0]) continue;
        if (cps_parse(v, &g_cps_storage[stage][i]) < 0) {
            const char *eparts[] = { name, ": invalid CPS template" };
            log_msgn("FATAL: ", eparts, 2);
            _exit(1);
        }
        pr->cps[i] = &g_cps_storage[stage][i];
    }
}

/* Parse one fallback stage (>=1). Returns 0 if the stage is not configured. */
static int parse_fb_stage(awg_config_t *cfg, int stage, awg_profile_t *fb) {
    char nb[32];
    const char *v;

    if (!(v = getenv(fb_name(nb, stage, "H1"))) || !v[0])
        return 0;

    memset(fb, 0, sizeof(*fb));
    fb->s1 = parse_int_str(fb_required(nb, stage, "S1"));
    fb->s2 = parse_int_str(fb_required(nb, stage, "S2"));
    if ((v = getenv(fb_name(nb, stage, "S3"))) && v[0]) fb->s3 = parse_int_str(v);
    if ((v = getenv(fb_name(nb, stage, "S4"))) && v[0]) fb->s4 = parse_int_str(v);
    if ((v = getenv(fb_name(nb, stage, "HP"))) && v[0]) fb->hp_on = parse_int_str(v) != 0;
    if ((v = getenv(fb_name(nb, stage, "RANDOM_TRAILERS"))) && v[0])
        fb->rt = parse_bool_str(v);
    if (fb->hp_on && !cfg->hp_key_set) {
        const char *parts[] = { fb_name(nb, stage, "HP"),
                                "=1 requires AWG_HEADER_PROTECTION_KEY" };
        log_msgn("FATAL: ", parts, 2);
        _exit(1);
    }
    fb_hrange(nb, stage, "H1", &fb->h1);
    fb_hrange(nb, stage, "H2", &fb->h2);
    fb_hrange(nb, stage, "H3", &fb->h3);
    fb_hrange(nb, stage, "H4", &fb->h4);
    load_cps_stage(stage, fb);

    config_compute_profile(fb);
    {
        const char *fberr = NULL;
        if (config_validate_profile(fb, &fberr) < 0) fatal(fberr);
    }
    return 1;
}

/* AWG 3.0 sender-side timing parameters. The proxy forwards packets as they
 * arrive and never paces them, so these are accepted (a v3 .conf carries over
 * unchanged and nothing crashes on unknown keys) and reported as not applied. */
static void report_unemulated_env(void) {
    static const char *names[] = {
        "AWG_REKEY_AFTER", "AWG_REKEY_TIMEOUT", "AWG_KEEPALIVE_TIMEOUT",
        "AWG_MAX_HANDSHAKE_ATTEMPTS", "AWG_CONTENT_PADDING",
    };
    for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        const char *v = getenv(names[i]);
        if (v && v[0])
            log_info3(names[i], ": accepted but not emulated — the proxy relays "
                      "packets as they arrive: ", v);
    }

    /* RejectAfterTime is enforced by the *receiver*: if the server rejects a
     * keypair before MikroTik finishes rekeying (rekey starts at 120s, old
     * keypair lives to 180s), the tunnel breaks periodically. */
    const char *v = getenv("AWG_REJECT_AFTER");
    if (v && v[0]) {
        hrange_t r;
        if (parse_hrange(v, &r) == 0 && r.min < 150 && g_log_level >= LOG_ERROR) {
            const char *parts[] = { "AWG_REJECT_AFTER=", v,
                " has a lower bound below 150s: the peer may start rejecting a "
                "keypair while MikroTik still considers it live (rekey begins "
                "at 120s and takes up to ~15s), so expect periodic drops" };
            log_msgn("WARN: ", parts, 3);
        }
    }
}

int main(void) {
    int errs = 0;
    const char *v;
    awg_config_t *cfg = &g_config;
    memset(cfg, 0, sizeof(*cfg));

    /* Padding, junk and CPS bodies all come from the kernel CSPRNG. If there is
     * no entropy source (a container built without /dev and on a kernel without
     * getrandom(2)), fail here rather than put a predictable pattern on the
     * wire for the lifetime of the tunnel. */
    if (csprng_init() < 0)
        fatal("no entropy source: getrandom(2) and /dev/urandom both unavailable");

    /* Required env vars */
    const char *listen_str = getenv_required("AWG_LISTEN", &errs);
    const char *remote_str = getenv_required("AWG_REMOTE", &errs);
    const char *jc_str  = getenv_required("AWG_JC", &errs);
    const char *jmin_str = getenv_required("AWG_JMIN", &errs);
    const char *jmax_str = getenv_required("AWG_JMAX", &errs);
    const char *s1_str  = getenv_required("AWG_S1", &errs);
    const char *s2_str  = getenv_required("AWG_S2", &errs);
    const char *h1_str  = getenv_required("AWG_H1", &errs);
    const char *h2_str  = getenv_required("AWG_H2", &errs);
    const char *h3_str  = getenv_required("AWG_H3", &errs);
    const char *h4_str  = getenv_required("AWG_H4", &errs);
    const char *spub_str = getenv_required("AWG_SERVER_PUB", &errs);
    const char *cpub_str = getenv("AWG_CLIENT_PUB");
    const char *cpubs_str = getenv("AWG_CLIENT_PUBS");
    const char *cpubs_file_str = getenv("AWG_CLIENT_PUBS_FILE");

    if (errs > 0) {
        fatal("missing required environment variables (see above)");
    }

    /* AWG_MODE: normal (default), reverse, server */
    cfg->mode = AWG_MODE_NORMAL;
    v = getenv("AWG_MODE");
    if (v && v[0]) {
        if (v[0] == 'r') cfg->mode = AWG_MODE_REVERSE;
        else if (v[0] == 's') cfg->mode = AWG_MODE_SERVER;
    }

    if (cfg->mode != AWG_MODE_SERVER && (!cpub_str || !cpub_str[0])) {
        log_msg("FATAL: ", "AWG_CLIENT_PUB is not set");
        _exit(1);
    }
    if (cfg->mode == AWG_MODE_SERVER && (!cpub_str || !cpub_str[0]) &&
        (!cpubs_str || !cpubs_str[0]) && (!cpubs_file_str || !cpubs_file_str[0])) {
        fatal("server mode requires AWG_CLIENT_PUB or AWG_CLIENT_PUBS/AWG_CLIENT_PUBS_FILE");
    }

    /* Parse integers */
    cfg->jc = parse_int_str(jc_str);
    cfg->jmin = parse_int_str(jmin_str);
    cfg->jmax = parse_int_str(jmax_str);
    cfg->s1 = parse_int_str(s1_str);
    cfg->s2 = parse_int_str(s2_str);

    /* Parse H ranges */
    if (parse_hrange(h1_str, &cfg->h1) < 0) fatal("AWG_H1: invalid range");
    if (parse_hrange(h2_str, &cfg->h2) < 0) fatal("AWG_H2: invalid range");
    if (parse_hrange(h3_str, &cfg->h3) < 0) fatal("AWG_H3: invalid range");
    if (parse_hrange(h4_str, &cfg->h4) < 0) fatal("AWG_H4: invalid range");

    /* Decode server public key */
    {
        int len = base64_decode(spub_str, slen(spub_str), cfg->server_pub, 32);
        if (len != 32) fatal("AWG_SERVER_PUB: must decode to 32 bytes");
    }

    /* Decode client public key (legacy single-peer / fallback key) */
    if (cpub_str && cpub_str[0]) {
        int len = base64_decode(cpub_str, slen(cpub_str), cfg->client_pub, 32);
        if (len != 32) fatal("AWG_CLIENT_PUB: must decode to 32 bytes");
    }

    if (cfg->mode == AWG_MODE_SERVER) {
        if (cpubs_str && cpubs_str[0])
            parse_server_peer_list(cfg, "AWG_CLIENT_PUBS", cpubs_str);
        if (cpubs_file_str && cpubs_file_str[0])
            load_server_peer_file(cfg, cpubs_file_str);
    }

    /* Optional v2 params */
    if ((v = getenv("AWG_S3")) && v[0]) cfg->s3 = parse_int_str(v);
    if ((v = getenv("AWG_S4")) && v[0]) cfg->s4 = parse_int_str(v);

    /* AWG 3.0 header protection key — the single thing that distinguishes v3
     * on the wire. Absent (or all-zero) key means v2 semantics byte for byte,
     * exactly as in amneziawg-go. */
    if ((v = getenv("AWG_HEADER_PROTECTION_KEY")) && v[0]) {
        if (decode_key32(v, cfg->hp_key) < 0)
            fatal("AWG_HEADER_PROTECTION_KEY: must be 44 base64 or 64 hex chars (32 bytes)");
        static const uint8_t zero32[32] = {0};
        cfg->hp_key_set = memcmp(cfg->hp_key, zero32, 32) != 0;
        cfg->hp_on = cfg->hp_key_set;
    }

    /* AWG 3.1 — RandomTrailers: a random tail on every handshake, response and
     * cookie, so their sizes stop being a fingerprint. Both peers must agree:
     * a 3.0 peer measures sizes exactly and drops a padded handshake. */
    if ((v = getenv("AWG_RANDOM_TRAILERS")) && v[0])
        cfg->rt = parse_bool_str(v);

    /* AWG 3.1 — DisableCookies: the interface answers no handshake with a
     * cookie reply, so the proxy drops the one the local WireGuard produced
     * instead of forwarding it. */
    if ((v = getenv("AWG_DISABLE_COOKIES")) && v[0])
        cfg->disable_cookies = parse_bool_str(v);

    {
        const char *cfg_err = NULL;
        if (config_validate(cfg, &cfg_err) < 0)
            fatal(cfg_err);
    }

    /* Compute derived fields; also mirrors the flat fields into profiles[0] */
    load_cps_stage(0, &cfg->profiles[0]);
    for (int i = 0; i < 5; i++) cfg->cps[i] = cfg->profiles[0].cps[i];
    config_compute(cfg);

    /* --- Profile fallback chain (v3 → v2 → v1.5 → v1) ---
     * Primary profile = the AWG_* config above. Optional stages in AWG_FB_*,
     * AWG_FB2_*, AWG_FB3_*: an initiator probes the next stage when the remote
     * stays silent, a responder adopts the stage a peer handshakes with. */
    cfg->fb_after = 20;
    if ((v = getenv("AWG_FB_AFTER")) && v[0]) {
        cfg->fb_after = parse_int_str(v);
        if (cfg->fb_after < 5) cfg->fb_after = 5;
    }

    for (int stage = 1; stage < AWG_MAX_PROFILES; stage++) {
        if (!parse_fb_stage(cfg, stage, &cfg->profiles[stage]))
            break;
        cfg->profile_count = stage + 1;
    }
    config_compute_max_s4(cfg);

    /* How long the remote may stay silent while the tunnel is trying to
     * handshake before the run reconnects. Was 180 s back when the watchdog
     * acted on plain silence and a quiet-but-healthy tunnel could trip it; it
     * now waits for an unanswered handshake init, which a working tunnel never
     * produces, so the wait can be short without risking a needless reconnect
     * — and a real outage is noticed three times sooner. */
    cfg->timeout = 60;
    if ((v = getenv("AWG_TIMEOUT")) && v[0])
        cfg->timeout = parse_int_str(v);

    /* Periodic DNS re-resolve interval (hostname remotes only) */
    cfg->dns_refresh = 60;
    if ((v = getenv("AWG_DNS_REFRESH")) && v[0])
        cfg->dns_refresh = parse_int_str(v);
    if (cfg->dns_refresh < 0) cfg->dns_refresh = 0;

    /* Happy Eyeballs head start for IPv4 (RFC 8305 uses 250 ms) — only ever
     * consulted when AWG_REMOTE resolves to both an A and an AAAA record. */
    cfg->he_delay = 250;
    if ((v = getenv("AWG_HE_DELAY")) && v[0]) {
        cfg->he_delay = parse_int_str(v);
        if (cfg->he_delay < 0) cfg->he_delay = 0;
        if (cfg->he_delay > 5000) cfg->he_delay = 5000;
    }

    /* Learned transport preference. Default is plain Happy Eyeballs: IPv4 is
     * tried first and IPv6 only takes over when IPv4 stays silent. The first
     * time that fallback succeeds the proxy records "prefer IPv6" on disk
     * (AWG_STATE_FILE) so subsequent starts skip the dead-IPv4 head start
     * outright. The file is written at most once per learn — the router's flash
     * is small and must not be hammered. */
    cfg->state_file = "/etc/awg-proxy.state";
    if ((v = getenv("AWG_STATE_FILE")) && v[0])
        cfg->state_file = v;

    /* Log level */
    cfg->log_level = LOG_INFO;
    v = getenv("AWG_LOG_LEVEL");
    if (v) {
        if (v[0] == 'n') cfg->log_level = LOG_NONE;
        else if (v[0] == 'e') cfg->log_level = LOG_ERROR;
        else if (v[0] == 'i') cfg->log_level = LOG_INFO;
        else if (v[0] == 'd') cfg->log_level = LOG_DEBUG;
    }
    g_log_level = cfg->log_level;

    /* Socket buffer */
    cfg->socket_buf = 16 * 1024 * 1024;
    if ((v = getenv("AWG_SOCKET_BUF")) && v[0])
        cfg->socket_buf = parse_int_str(v);

    /* Source port: auto (default), fixed N, or "random" (kernel-ephemeral) */
    int src_port = 0;
    if ((v = getenv("AWG_SRC_PORT")) && v[0]) {
        if (strcmp(v, "random") == 0)
            src_port = -1;
        else
            src_port = parse_int_str(v);
    }

    /* CPU affinity */
    cfg->cpu_c2s = -1;
    cfg->cpu_s2c = -1;
    if ((v = getenv("AWG_CPU_C2S")) && v[0])
        cfg->cpu_c2s = parse_int_str(v);
    if ((v = getenv("AWG_CPU_S2C")) && v[0])
        cfg->cpu_s2c = parse_int_str(v);

    /* Busy poll */
    cfg->busy_poll = 0;
    if ((v = getenv("AWG_BUSY_POLL")) && v[0])
        cfg->busy_poll = parse_int_str(v);

    /* Spin-drain: how long a reader keeps retrying a non-blocking read before
     * it lets itself be put to sleep. Unlike SO_BUSY_POLL this asks the kernel
     * for no privilege at all, so it still works in a container the router
     * hands no CAP_NET_ADMIN to. */
    cfg->spin_us = 0;
    cfg->spin_auto = 0;
    if ((v = getenv("AWG_SPIN")) && v[0]) {
        if (v[0] == 'a' || v[0] == 'A') {
            cfg->spin_auto = 1;
            cfg->spin_us = SPIN_START_US;
        } else {
            cfg->spin_us = parse_int_str(v);
        }
    }

    /* No GRO */
    cfg->no_gro = 0;
    if ((v = getenv("AWG_NO_GRO")) && v[0])
        cfg->no_gro = parse_int_str(v);

    /* Periodic line with throughput, our own drops and the kernel's. Off by
     * default: it is a diagnostic, and on a router the log is precious. */
    cfg->stats_interval = 0;
    if ((v = getenv("AWG_STATS")) && v[0])
        cfg->stats_interval = parse_int_str(v);

    /* No DF: clear the Don't-Fragment bit on UDP sockets (some DPI/middleboxes
     * mishandle DF=1 UDP; opt-in, changes on-wire IP header behavior) */
    cfg->no_df = 0;
    if ((v = getenv("AWG_NO_DF")) && v[0])
        cfg->no_df = parse_int_str(v);

    /* DNS resolver for hostname resolution */
    v = getenv("AWG_DNS");
    if (v && v[0]) {
        FILE *f = fopen("/etc/resolv.conf", "w");
        if (f) {
            fprintf(f, "nameserver %s\n", v);
            fclose(f);
            log_info2("DNS resolver: ", v);
        }
    }

    /* Determine protocol mode */
    const char *mode = "v1";
    if (cfg->rt || cfg->disable_cookies) {
        mode = "v3.1";
    } else if (cfg->hp_key_set) {
        mode = "v3";
    } else if (cfg->s3 > 0 || cfg->s4 > 0 ||
        cfg->h1.min != cfg->h1.max || cfg->h2.min != cfg->h2.max ||
        cfg->h3.min != cfg->h3.max || cfg->h4.min != cfg->h4.max) {
        mode = "v2";
    } else if (cfg->cps[0] || cfg->cps[1] || cfg->cps[2] || cfg->cps[3] || cfg->cps[4]) {
        mode = "v1.5";
    }

    /* Startup log */
    {
        const char *awg_mode_str = cfg->mode == AWG_MODE_REVERSE ? "reverse" :
                                   cfg->mode == AWG_MODE_SERVER  ? "server" : "normal";
        const char *parts[] = { "awg-proxy ", VERSION, " linux/c proto=", mode,
                                " awg_mode=", awg_mode_str };
        log_infon(parts, 6);
    }
    {
        char spb[12];
        const char *parts[] = { "listen=", listen_str, " remote=", remote_str,
            " src_port=", src_port > 0 ? u32_to_str(spb, src_port) :
                          (src_port < 0 ? "random" : "auto") };
        log_infon(parts, 6);
    }
    {
        char jcb[12], jminb[12], jmaxb[12];
        const char *parts[] = { "config: JC=", u32_to_str(jcb, cfg->jc),
            " JMIN=", u32_to_str(jminb, cfg->jmin),
            " JMAX=", u32_to_str(jmaxb, cfg->jmax) };
        log_infon(parts, 6);
    }
    {
        char s1b[12], s2b[12], s3b[12], s4b[12];
        const char *parts[] = { "config: S1=", u32_to_str(s1b, cfg->s1),
            " S2=", u32_to_str(s2b, cfg->s2),
            " S3=", u32_to_str(s3b, cfg->s3),
            " S4=", u32_to_str(s4b, cfg->s4) };
        log_infon(parts, 8);
    }
    {
        const char *parts[] = { "config: H1=", h1_str, " H2=", h2_str,
            " H3=", h3_str, " H4=", h4_str };
        log_infon(parts, 8);
    }
    {
        awg_profile_t *pr0 = &cfg->profiles[0];
        int any = pr0->cps[0] || pr0->cps[1] || pr0->cps[2] || pr0->cps[3] || pr0->cps[4];
        if (any) {
            const char *parts[] = { "config: CPS I1-I5:",
                pr0->cps[0] ? " I1" : "", pr0->cps[1] ? " I2" : "",
                pr0->cps[2] ? " I3" : "", pr0->cps[3] ? " I4" : "",
                pr0->cps[4] ? " I5" : "" };
            log_infon(parts, 6);
        } else {
            log_info("config: CPS I1-I5: none");
        }
    }
    if (cfg->hp_key_set)
        log_info("config: AWG 3.0 header protection enabled (ChaCha20 over the packet header)");
    if (cfg->rt)
        log_info("config: AWG 3.1 random trailers enabled (handshakes get a random tail)");
    if (cfg->disable_cookies)
        log_info("config: AWG 3.1 cookie replies disabled (outbound cookies are dropped)");
    if (cfg->profile_count > 1) {
        char ab[12], cb[12];
        const char *parts[] = { "config: fallback chain of ",
            u32_to_str(cb, cfg->profile_count), " profiles, probe after ",
            u32_to_str(ab, cfg->fb_after), "s" };
        log_infon(parts, 5);
        for (int i = 1; i < cfg->profile_count; i++) {
            awg_profile_t *fb = &cfg->profiles[i];
            char ib[12], s1b[12], s2b[12], s3b[12], s4b[12];
            const char *sparts[] = { "config: FB stage ", u32_to_str(ib, i),
                ": S1=", u32_to_str(s1b, fb->s1), " S2=", u32_to_str(s2b, fb->s2),
                " S3=", u32_to_str(s3b, fb->s3), " S4=", u32_to_str(s4b, fb->s4),
                fb->hp_on ? " HP=on" : " HP=off",
                fb->rt ? " RT=on" : " RT=off" };
            log_infon(sparts, 12);
            char h1b[12], h2b[12], h3b[12], h4b[12];
            const char *hparts[] = { "config: FB stage ", u32_to_str(ib, i),
                ": H1=", u32_to_str(h1b, fb->h1.min),
                " H2=", u32_to_str(h2b, fb->h2.min), " H3=", u32_to_str(h3b, fb->h3.min),
                " H4=", u32_to_str(h4b, fb->h4.min) };
            log_infon(hparts, 10);
        }
    }
    report_unemulated_env();
    if (cfg->no_gro)
        log_info("config: UDP GRO disabled (AWG_NO_GRO=1)");
    if (cfg->no_df)
        log_info("config: DF bit cleared on UDP sockets (AWG_NO_DF=1)");
    if (cfg->cpu_c2s >= 0 || cfg->cpu_s2c >= 0 || cfg->busy_poll > 0 ||
        cfg->spin_us > 0) {
        char c2sb[12], s2cb[12], bpb[12], spb[12];
        const char *parts[] = {
            "perf: cpu_c2s=", cfg->cpu_c2s >= 0 ? u32_to_str(c2sb, cfg->cpu_c2s) : "auto",
            " cpu_s2c=", cfg->cpu_s2c >= 0 ? u32_to_str(s2cb, cfg->cpu_s2c) : "auto",
            " busy_poll=", cfg->busy_poll > 0 ? u32_to_str(bpb, cfg->busy_poll) : "off",
            " spin=", cfg->spin_auto ? "auto" :
                            (cfg->spin_us > 0 ? u32_to_str(spb, cfg->spin_us) : "off")
        };
        log_infon(parts, 8);
    }

    /* Init and run proxy */
    if (proxy_init(&g_proxy, cfg, listen_str, remote_str, src_port) < 0) {
        fatal("proxy init failed");
    }

    int ret = proxy_run(&g_proxy);
    return ret;
}
