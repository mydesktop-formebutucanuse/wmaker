
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/stat.h>

#include <linux/netlink.h>

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <WINGs/WUtil.h>

#include "wconfig.h"
#include "osdep.h"


/*
 * copy argc and argv for an existing process identified by `pid'
 * into suitable storage given in ***argv and *argc.
 *
 * subsequent calls use the same static area for argv and argc.
 *
 * returns 0 for failure, in which case argc := 0 and argv := NULL
 * returns 1 for success
 */
Bool GetCommandForPid(int pid, char ***argv, int *argc)
{
	static char buf[_POSIX_ARG_MAX];
	int fd, i, j;
	ssize_t count;

	*argv = NULL;
	*argc = 0;

	/* cmdline is a flattened series of null-terminated strings */
	snprintf(buf, sizeof(buf), "/proc/%d/cmdline", pid);
	while (1) {
		/* not switching this to stdio yet, as this does not need
		 * to be portable, and i'm lazy */
		fd = open(buf, O_RDONLY);
		if (fd != -1)
			break;
		if (errno == EINTR)
			continue;
		return False;
	}

	while (1) {
		count = read(fd, buf, sizeof(buf));
		if (count != -1)
			break;
		if (errno == EINTR)
			continue;
		close(fd);
		return False;
	}
	close(fd);

	/* count args */
	for (i = 0; i < count; i++)
		if (buf[i] == '\0')
			(*argc)++;

	if (*argc == 0)
		return False;

	*argv = (char **)wmalloc(sizeof(char *) * (*argc + 1 /* term. null ptr */));
	(*argv)[0] = buf;

	/* go through buf, set argv[$next] to the beginning of each string */
	for (i = 0, j = 1; i < count; i++) {
		if (buf[i] != '\0')
			continue;
		if (i < count - 1)
			(*argv)[j++] = &buf[i + 1];
		if (j == *argc)
			break;
	}

	/* the list of arguments must be terminated by a null pointer */
	(*argv)[j] = NULL;
	return True;
}


/*
 * The kernel sends a uevent with SUBSYSTEM=power_supply when a supply
 * changes, on the multicast group 1 of NETLINK_KOBJECT_UEVENT, which any
 * user can listen to.
 */
int wOSOpenPowerEvents(void)
{
	struct sockaddr_nl addr;
	int fd;

	fd = socket(AF_NETLINK, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, NETLINK_KOBJECT_UEVENT);
	if (fd < 0)
		return -1;

	memset(&addr, 0, sizeof(addr));
	addr.nl_family = AF_NETLINK;
	addr.nl_groups = 1;	/* the kernel's events, not udev's */
	if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		close(fd);
		return -1;
	}

	return fd;
}

int wOSReadPowerEvents(int fd)
{
	/* the batteries and the USB ports report often: only the chargers count */
	static const char key[] = "POWER_SUPPLY_TYPE=Mains";
	char buf[8192];
	struct sockaddr_nl from;
	socklen_t fromlen;
	ssize_t len, i;
	int found = 0;

	for (;;) {
		fromlen = sizeof(from);
		len = recvfrom(fd, buf, sizeof(buf), 0, (struct sockaddr *)&from, &fromlen);
		if (len < 0) {
			if (errno == EINTR)
				continue;
			if (errno == EAGAIN || errno == EWOULDBLOCK)
				return found;
			/* events were lost: the supply may have changed */
			if (errno == ENOBUFS) {
				found = 1;
				continue;
			}
			return -1;
		}

		/* only the kernel's own messages */
		if (fromlen != sizeof(from) || from.nl_pid != 0)
			continue;

		/* "ACTION@DEVPATH", then KEY=value strings, each ended by a NUL */
		for (i = 0; i < len; i += strnlen(buf + i, len - i) + 1) {
			if ((size_t)(len - i) >= sizeof(key) &&
			    memcmp(buf + i, key, sizeof(key)) == 0) {
				found = 1;
				break;
			}
		}
	}
}

/* Reads the first line of a file of a power supply into buf */
static Bool read_supply_file(const char *supply, const char *name, char *buf, size_t size)
{
	char path[PATH_MAX];
	ssize_t len;
	int fd;

	snprintf(path, sizeof(path), "/sys/class/power_supply/%s/%s", supply, name);
	fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return False;

	do {
		len = read(fd, buf, size - 1);
	} while (len < 0 && errno == EINTR);
	close(fd);
	if (len <= 0)
		return False;

	buf[len] = '\0';
	buf[strcspn(buf, "\n")] = '\0';
	return True;
}

int wOSMainsOnline(void)
{
	DIR *dir;
	struct dirent *de;
	char value[32];
	int result = -1;

	dir = opendir("/sys/class/power_supply");
	if (dir == NULL)
		return -1;

	while ((de = readdir(dir)) != NULL) {
		if (de->d_name[0] == '.')
			continue;
		if (!read_supply_file(de->d_name, "type", value, sizeof(value)) ||
		    strcmp(value, "Mains") != 0)
			continue;
		if (!read_supply_file(de->d_name, "online", value, sizeof(value)))
			continue;

		if (strcmp(value, "1") == 0) {
			result = 1;
			break;
		}
		result = 0;
	}
	closedir(dir);

	return result;
}
