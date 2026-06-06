#ifndef AWG_APP_LAUNCHER_H
#define AWG_APP_LAUNCHER_H

#include "awg_bundle.h"
#include "routeros_reconcile.h"

#include <sys/types.h>

typedef struct {
    char proxy_bin[256];
    int child_count;
    pid_t child_pids[AWG_APP_MAX_PROFILES];
} launcher_t;

int launcher_start_profiles(launcher_t *launcher, const awg_bundle_t *bundle,
                            const reconcile_options_t *opts);
int launcher_poll(launcher_t *launcher, int *status);
int launcher_wait(launcher_t *launcher);
void launcher_stop(launcher_t *launcher);

#endif
