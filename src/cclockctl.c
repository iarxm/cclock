#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "config.h"

static int
socket_path(char *path, size_t size)
{
	const char *runtime = getenv("XDG_RUNTIME_DIR");
	int written;

	if (runtime && runtime[0])
		written = snprintf(path, size, "%s/%s", runtime, CCLOCK_SOCKET_NAME);
	else
		written = snprintf(path, size, "/tmp/cclock-%ld.sock", (long)getuid());
	return written < 0 || (size_t)written >= size ? -1 : 0;
}

int
main(int argc, char *argv[])
{
	struct sockaddr_un address;
	char path[sizeof(address.sun_path)];
	char response[64];
	ssize_t n;
	int fd;

	if (argc != 2 || (strcmp(argv[1], "toggle") && strcmp(argv[1], "show")
		&& strcmp(argv[1], "hide") && strcmp(argv[1], "status"))) {
		fprintf(stderr, "usage: %s {toggle|show|hide|status}\n", argv[0]);
		return 1;
	}
	if (socket_path(path, sizeof(path)) < 0) {
		fputs("cclock socket path is too long\n", stderr);
		return 1;
	}
	if ((fd = socket(AF_UNIX, SOCK_STREAM, 0)) < 0) {
		perror("socket");
		return 1;
	}
	memset(&address, 0, sizeof(address));
	address.sun_family = AF_UNIX;
	memcpy(address.sun_path, path, strlen(path) + 1);
	if (connect(fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
		fprintf(stderr, "failed to connect to cclock: %s\n", strerror(errno));
		close(fd);
		return 1;
	}
	if (dprintf(fd, "%s\n", argv[1]) < 0) {
		perror("write");
		close(fd);
		return 1;
	}
	shutdown(fd, SHUT_WR);
	if ((n = read(fd, response, sizeof(response) - 1)) > 0) {
		response[n] = '\0';
		fputs(response, stdout);
	}
	close(fd);
	return 0;
}
