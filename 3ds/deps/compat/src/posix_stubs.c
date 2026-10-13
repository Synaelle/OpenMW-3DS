/*
 * POSIX functions that newlib/libctru do not provide but dependencies reference.
 * The 3DS has no users or file ownership, so these are harmless no-ops.
 */
#include <sys/types.h>

int fchown(int fd, uid_t owner, gid_t group)
{
    (void)fd;
    (void)owner;
    (void)group;
    return 0;
}

uid_t geteuid(void)
{
    return 0;
}

uid_t getuid(void)
{
    return 0;
}
