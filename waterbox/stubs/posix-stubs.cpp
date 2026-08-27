/* The few POSIX calls Flycast makes that a sandbox has no syscall for.
 *
 * A waterbox guest talks to the host through a small, deliberate surface: the
 * files the frontend mounted, and nothing else. There is no kernel behind it,
 * so a call like access() does not fail - it stops the machine, because the
 * sandbox refuses to guess what the guest meant.
 *
 * These are answered in terms of what the sandbox DOES have. They are honest
 * answers, not silent successes: a file the frontend did not mount is reported
 * absent, which is exactly what it is.
 */
#include <cstdio>
#include <cerrno>
#include <unistd.h>

extern "C" int access(const char *path, int mode)
{
	/* Existence and readability are the same question here: the sandbox hands
	 * the guest read-only mounts, so a file that opens is readable and one
	 * that does not is absent. Writability is answered no - the machine's
	 * writes go through the save-data channel, never through a path. */
	if (mode & W_OK)
	{
		errno = EACCES;
		return -1;
	}
	FILE *f = fopen(path, "rb");
	if (f == nullptr)
	{
		errno = ENOENT;
		return -1;
	}
	fclose(f);
	return 0;
}
