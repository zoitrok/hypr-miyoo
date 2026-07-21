#include "util/proc.h"

#include <dirent.h>
#include <stdio.h>
#include <unistd.h>

long proc_rss_kb(void)
{
    FILE *f = fopen("/proc/self/statm", "r");
    if (!f)
        return 0;

    /* Fields are in pages: total, resident, shared, ... */
    long total = 0, resident = 0;
    int got = fscanf(f, "%ld %ld", &total, &resident);
    fclose(f);

    if (got != 2)
        return 0;

    long page_kb = sysconf(_SC_PAGESIZE) / 1024;
    return resident * page_kb;
}

int proc_open_fds(void)
{
    DIR *d = opendir("/proc/self/fd");
    if (!d)
        return -1;

    int n = 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] != '.')
            n++;
    }
    closedir(d);

    /* opendir itself holds one; report what the process has otherwise. */
    return n > 0 ? n - 1 : n;
}
