#include "util/power.h"
#include "util/log.h"

#include <stdio.h>
#include <unistd.h>

#define TAG "power"

/* Onion's temp flags live in /tmp; temp_flag_get(k) is flag_get("/tmp/", k). */
#define STAY_AWAKE_FLAG "/tmp/stay_awake"

static bool g_we_created_it;

bool power_keep_awake(void)
{
    if (access(STAY_AWAKE_FLAG, F_OK) == 0) {
        /* Something else already wants the device awake. Leave it alone, and
         * remember not to clear it on the way out. */
        LOGI(TAG, "stay-awake flag already set by something else; leaving it");
        g_we_created_it = false;
        return true;
    }

    FILE *f = fopen(STAY_AWAKE_FLAG, "w");
    if (!f) {
        /* Not fatal: the device will simply sleep on its usual timer, which is
         * a degraded experience rather than a broken one. */
        LOGW(TAG, "could not create %s; the device may sleep while playing",
             STAY_AWAKE_FLAG);
        return false;
    }
    fclose(f);

    g_we_created_it = true;
    LOGI(TAG, "auto-sleep suspended while playing (%s)", STAY_AWAKE_FLAG);
    return true;
}

void power_release(void)
{
    if (!g_we_created_it)
        return;

    if (remove(STAY_AWAKE_FLAG) == 0)
        LOGI(TAG, "auto-sleep restored");
    else
        LOGW(TAG, "could not remove %s", STAY_AWAKE_FLAG);

    g_we_created_it = false;
}
