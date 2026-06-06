#include "launcher.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

static char *envdup(const char *key, const char *value) {
    size_t len = strlen(key) + strlen(value) + 2;
    char *out = malloc(len);
    if (!out) return NULL;
    snprintf(out, len, "%s=%s", key, value);
    return out;
}

static void add_env(char **envv, int *n, const char *key, const char *value) {
    if (value && value[0]) envv[(*n)++] = envdup(key, value);
}

static void free_envv(char **envv, int n) {
    for (int i = 0; i < n; i++) free(envv[i]);
}

static void first_dns(const char *dns, char *out, size_t out_len) {
    out[0] = '\0';
    if (!dns || !dns[0]) return;
    while (*dns == ' ' || *dns == '\t' || *dns == ',') dns++;
    size_t n = 0;
    while (dns[n] && dns[n] != ',' && dns[n] != ' ' && dns[n] != '\t' && n + 1 < out_len)
        n++;
    snprintf(out, out_len, "%.*s", (int)n, dns);
}

static int start_profile(launcher_t *launcher, const awg_profile_t *p, const reconcile_options_t *opts) {
    char listen[64], proxy_port[16];
    snprintf(proxy_port, sizeof(proxy_port), "%d", profile_proxy_port(p, opts));
    snprintf(listen, sizeof(listen), ":%s", proxy_port);

    char *envv[64];
    int n = 0;
    for (char **e = environ; *e && n < 16; e++) {
        if (strncmp(*e, "PATH=", 5) == 0 || strncmp(*e, "HOME=", 5) == 0)
            envv[n++] = strdup(*e);
    }

    add_env(envv, &n, "AWG_LISTEN", listen);
    add_env(envv, &n, "AWG_REMOTE", p->peer_endpoint);
    add_env(envv, &n, "AWG_JC", p->jc);
    add_env(envv, &n, "AWG_JMIN", p->jmin);
    add_env(envv, &n, "AWG_JMAX", p->jmax);
    add_env(envv, &n, "AWG_S1", p->s1);
    add_env(envv, &n, "AWG_S2", p->s2);
    add_env(envv, &n, "AWG_H1", p->h1);
    add_env(envv, &n, "AWG_H2", p->h2);
    add_env(envv, &n, "AWG_H3", p->h3);
    add_env(envv, &n, "AWG_H4", p->h4);
    add_env(envv, &n, "AWG_SERVER_PUB", p->peer_public_key);

    add_env(envv, &n, "AWG_CLIENT_PUB", p->routeros_public_key);

    add_env(envv, &n, "AWG_S3", p->s3);
    add_env(envv, &n, "AWG_S4", p->s4);
    for (int i = 0; i < 5; i++) {
        char key[16];
        snprintf(key, sizeof(key), "AWG_I%d", i + 1);
        add_env(envv, &n, key, p->i[i]);
    }
    char dns[128];
    first_dns(p->interface_dns, dns, sizeof(dns));
    add_env(envv, &n, "AWG_DNS", dns);
    envv[n] = NULL;

    pid_t pid = fork();
    if (pid < 0) {
        free_envv(envv, n);
        return -1;
    }
    if (pid == 0) {
        char *argv[] = { launcher->proxy_bin, NULL };
        execve(launcher->proxy_bin, argv, envv);
        _exit(127);
    }

    free_envv(envv, n);
    launcher->child_pids[launcher->child_count++] = pid;
    fprintf(stderr, "started profile %s pid=%ld listen=%s remote=%s\n",
            p->name, (long)pid, listen, p->peer_endpoint);
    return 0;
}

int launcher_start_profiles(launcher_t *launcher, const awg_bundle_t *bundle,
                            const reconcile_options_t *opts) {
    memset(launcher, 0, sizeof(*launcher));
    const char *proxy_bin = getenv("AWG_PROXY_BIN");
    snprintf(launcher->proxy_bin, sizeof(launcher->proxy_bin), "%s", proxy_bin && proxy_bin[0] ? proxy_bin : "/awg-proxy");

    for (int i = 0; i < bundle->count; i++) {
        const awg_profile_t *p = &bundle->profiles[i];
        if (!p->enabled) continue;
        if (start_profile(launcher, p, opts) < 0) {
            launcher_stop(launcher);
            return -1;
        }
    }
    return launcher->child_count > 0 ? 0 : -1;
}

void launcher_stop(launcher_t *launcher) {
    for (int i = 0; i < launcher->child_count; i++) {
        if (launcher->child_pids[i] > 0) kill(launcher->child_pids[i], SIGTERM);
    }
    for (int i = 0; i < launcher->child_count; i++) {
        if (launcher->child_pids[i] > 0) waitpid(launcher->child_pids[i], NULL, 0);
    }
}

int launcher_poll(launcher_t *launcher, int *status) {
    pid_t pid = waitpid(-1, status, WNOHANG);
    if (pid == 0) return 0;
    if (pid < 0) {
        if (errno == ECHILD) return 0;
        return -1;
    }
    for (int i = 0; i < launcher->child_count; i++) {
        if (launcher->child_pids[i] == pid) {
            launcher->child_pids[i] = launcher->child_pids[launcher->child_count - 1];
            launcher->child_count--;
            break;
        }
    }
    return 1;
}

int launcher_wait(launcher_t *launcher) {
    int status = 0;
    while (launcher->child_count > 0) {
        pid_t pid = wait(&status);
        if (pid < 0) return -1;
        for (int i = 0; i < launcher->child_count; i++) {
            if (launcher->child_pids[i] == pid) {
                launcher->child_pids[i] = launcher->child_pids[launcher->child_count - 1];
                launcher->child_count--;
                break;
            }
        }
        if (WIFEXITED(status) || WIFSIGNALED(status)) {
            launcher_stop(launcher);
            return status;
        }
    }
    return status;
}
