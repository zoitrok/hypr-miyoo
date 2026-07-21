#ifndef HYPR_POWER_H
#define HYPR_POWER_H

#include <stdbool.h>

/* Keeps OnionOS from suspending the device while the radio is playing.
 *
 * Onion's keymon runs an idle timer and suspends after `settings.sleep_timer`
 * minutes without a button press. That is right for a game and wrong for a
 * radio: nobody touches the controls while listening, so the device goes to
 * sleep mid-song precisely because the app is working as intended.
 *
 * keymon checks a flag on each pass and skips suspending when it is set:
 *
 *     if (temp_flag_get("stay_awake")) { timeout = -1; }
 *
 * and temp flags are just files -- temp_flag_get(key) is flag_get("/tmp/", key)
 * -- so the flag is the existence of /tmp/stay_awake. Setting it is a supported
 * interface rather than a trick; Onion's own apps use the same mechanism.
 *
 * We only remove the flag on exit if we were the ones who created it, so
 * quitting the radio cannot cancel a stay-awake that something else wanted.
 *
 * This does not stop the user suspending deliberately with the power button,
 * and it does not survive a kill -9 -- the flag would be left behind until the
 * next reboot clears /tmp. That is the failure direction to prefer: a device
 * that stays awake too long is a nuisance, one that sleeps mid-song is a bug. */

/* Returns true if the flag is now set (or was already). */
bool power_keep_awake(void);

/* Removes the flag, but only if power_keep_awake() created it. */
void power_release(void);

#endif
