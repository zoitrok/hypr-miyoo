#ifndef HYPR_CFG_H
#define HYPR_CFG_H

/* Reads extra command-line options from a text file next to the binary.
 *
 * There is no shell on the device and no way to type arguments -- MainUI just
 * runs launch.sh. Without this, changing any option means editing a shell
 * script on the SD card, which is a poor way to ask someone to turn on a
 * diagnostic.
 *
 * The file holds one option per line, exactly as it would be typed:
 *
 *     # lines starting with # are ignored
 *     --verbose
 *     --chaos 0.02
 *     --rotate 0
 *
 * A line may carry an option and its value together; anything after the first
 * space is taken as the value, so URLs and paths need no quoting.
 *
 * Returns the number of arguments appended to out (never more than max), or 0
 * if the file does not exist -- which is the normal case and not an error.
 * Strings are strdup'd and owned by the caller. */
int cfg_load_args(const char *path, char **out, int max);

#endif
