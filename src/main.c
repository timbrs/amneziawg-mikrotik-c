#include "proxy.h"
#include "cps.h"
#include "csprng.h"
#include "log.h"
#include "base64.h"
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

static const char *fb_required(const char *name) {
    const char *v = getenv(name);
    if (!v || !v[0]) {
        const char *parts[] = { name, " is required when AWG_FB_H1 is set" };
        log_msgn("FATAL: ", parts, 2);
        _exit(1);
    }
    return v;
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
/* CPS templates storage */
static cps_template_t g_cps_storage[5];

int main(void) {
    int errs = 0;
    const char *v;
    awg_config_t *cfg = &g_config;
    memset(cfg, 0, sizeof(*cfg));

    /* Packet padding must be unpredictable (it is also the header-protection
     * nonce), so fail early and loudly if the kernel gives us no entropy. */
    if (csprng_init() < 0)
        fatal("no entropy source: getrandom() unavailable and /dev/urandom missing");

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

    /* Optional v3 param: header protection key (HeaderProtectionKey on the
     * server). Empty = v2 behaviour, packet headers stay in the clear. */
    if ((v = getenv("AWG_HP_KEY")) && v[0]) {
        int len = base64_decode(v, slen(v), cfg->hp_key, CHACHA20_KEY_SIZE);
        if (len != CHACHA20_KEY_SIZE)
            fatal("AWG_HP_KEY: must decode to 32 bytes");
        cfg->has_hp = 1;
    }

    {
        const char *cfg_err = NULL;
        if (config_validate(cfg, &cfg_err) < 0)
            fatal(cfg_err);
    }

    /* CPS templates I1-I5 */
    const char *inames[] = { "AWG_I1", "AWG_I2", "AWG_I3", "AWG_I4", "AWG_I5" };
    for (int i = 0; i < 5; i++) {
        v = getenv(inames[i]);
        if (v && v[0]) {
            if (cps_parse(v, &g_cps_storage[i]) < 0) {
                const char *eparts[] = { inames[i], ": invalid CPS template" };
                log_msgn("FATAL: ", eparts, 2);
                _exit(1);
            }
            cfg->cps[i] = &g_cps_storage[i];
        }
    }

    /* Compute derived fields */
    config_compute(cfg);

    /* --- Dual-profile fallback (site-to-site backward compatibility) ---
     * Primary profile = the AWG_* config above (typically v2). Optional v1
     * fallback in AWG_FB_*: an initiator probes it when the remote stays
     * silent, a responder adopts it when a peer handshakes with it. */
    cfg->profile_count = 1;
    cfg->active_profile = 0;
    cfg->fb_after = 20;
    if ((v = getenv("AWG_FB_AFTER")) && v[0]) {
        cfg->fb_after = parse_int_str(v);
        if (cfg->fb_after < 5) cfg->fb_after = 5;
    }
    config_snapshot_profile(cfg, &cfg->profiles[0]);

    v = getenv("AWG_FB_H1");
    if (v && v[0]) {
        if (cfg->mode == AWG_MODE_SERVER)
            fatal("AWG_FB_* fallback is not supported in server mode");
        if (cfg->has_hp)
            fatal("AWG_FB_* fallback is not supported with AWG_HP_KEY (v1 profile has no padding)");
        if (cfg->s4 != 0)
            fatal("AWG_FB_* fallback requires AWG_S4=0");
        awg_profile_t *fb = &cfg->profiles[1];
        memset(fb, 0, sizeof(*fb));
        fb->s1 = parse_int_str(fb_required("AWG_FB_S1"));
        fb->s2 = parse_int_str(fb_required("AWG_FB_S2"));
        fb->s3 = 0;
        if ((v = getenv("AWG_FB_S3")) && v[0]) fb->s3 = parse_int_str(v);
        fb->s4 = 0;
        if (parse_hrange(fb_required("AWG_FB_H1"), &fb->h1) < 0) fatal("AWG_FB_H1: invalid range");
        if (parse_hrange(fb_required("AWG_FB_H2"), &fb->h2) < 0) fatal("AWG_FB_H2: invalid range");
        if (parse_hrange(fb_required("AWG_FB_H3"), &fb->h3) < 0) fatal("AWG_FB_H3: invalid range");
        if (parse_hrange(fb_required("AWG_FB_H4"), &fb->h4) < 0) fatal("AWG_FB_H4: invalid range");
        /* fb->cps left NULL: v1 fallback carries no CPS templates */
        config_compute_profile(fb);
        {
            const char *fberr = NULL;
            if (config_validate_profile(fb, &fberr) < 0) fatal(fberr);
        }
        cfg->profile_count = 2;
    }

    /* Timeout */
    cfg->timeout = 180;
    if ((v = getenv("AWG_TIMEOUT")) && v[0])
        cfg->timeout = parse_int_str(v);

    /* Periodic DNS re-resolve interval (hostname remotes only) */
    cfg->dns_refresh = 60;
    if ((v = getenv("AWG_DNS_REFRESH")) && v[0])
        cfg->dns_refresh = parse_int_str(v);
    if (cfg->dns_refresh < 0) cfg->dns_refresh = 0;

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

    /* No GRO */
    cfg->no_gro = 0;
    if ((v = getenv("AWG_NO_GRO")) && v[0])
        cfg->no_gro = parse_int_str(v);

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
    if (cfg->s3 > 0 || cfg->s4 > 0 ||
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
    if (cfg->profile_count > 1) {
        awg_profile_t *fb = &cfg->profiles[1];
        char ab[12], s1b[12], s2b[12];
        const char *parts[] = { "config: v1 fallback enabled, probe after ",
            u32_to_str(ab, cfg->fb_after), "s: FB_S1=", u32_to_str(s1b, fb->s1),
            " FB_S2=", u32_to_str(s2b, fb->s2) };
        log_infon(parts, 6);
        char h1b[12], h2b[12], h3b[12], h4b[12];
        const char *hparts[] = { "config: FB_H1=", u32_to_str(h1b, fb->h1.min),
            " FB_H2=", u32_to_str(h2b, fb->h2.min), " FB_H3=", u32_to_str(h3b, fb->h3.min),
            " FB_H4=", u32_to_str(h4b, fb->h4.min) };
        log_infon(hparts, 8);
    }
    if (cfg->no_gro)
        log_info("config: UDP GRO disabled (AWG_NO_GRO=1)");
    if (cfg->no_df)
        log_info("config: DF bit cleared on UDP sockets (AWG_NO_DF=1)");
    if (cfg->cpu_c2s >= 0 || cfg->cpu_s2c >= 0 || cfg->busy_poll > 0) {
        char c2sb[12], s2cb[12], bpb[12];
        const char *parts[] = {
            "perf: cpu_c2s=", cfg->cpu_c2s >= 0 ? u32_to_str(c2sb, cfg->cpu_c2s) : "auto",
            " cpu_s2c=", cfg->cpu_s2c >= 0 ? u32_to_str(s2cb, cfg->cpu_s2c) : "auto",
            " busy_poll=", cfg->busy_poll > 0 ? u32_to_str(bpb, cfg->busy_poll) : "off"
        };
        log_infon(parts, 6);
    }

    /* Init and run proxy */
    if (proxy_init(&g_proxy, cfg, listen_str, remote_str, src_port) < 0) {
        fatal("proxy init failed");
    }

    int ret = proxy_run(&g_proxy);
    return ret;
}
