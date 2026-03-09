/*
 * nwpid — NumWorks Pi Daemon
 *
 * Handles bidirectional UART communication between the NumWorks
 * calculator and Raspberry Pi using the NWPI protocol.
 *
 * Message format: CMD:PAYLOAD\n
 * Legacy format:  :HEXDATA\n  (treated as KEY:HEXDATA)
 */

#include "protocol.h"
#include "keyboard.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <poll.h>
#include <termios.h>

#define DEFAULT_TTY "/dev/ttyS0"
#define MOUSE_INTERVAL_MS 8

static int tty_fd = -1;

/* Send a response over UART: CMD:PAYLOAD\n */
void nwpid_send(const char *cmd, const char *payload)
{
	if (tty_fd < 0)
		return;

	char buf[NWPI_MAX_MSG];
	int len;

	if (payload && payload[0])
		len = snprintf(buf, sizeof(buf), "%s:%s\n", cmd, payload);
	else
		len = snprintf(buf, sizeof(buf), "%s:\n", cmd);

	if (len > 0 && len < (int)sizeof(buf))
		write(tty_fd, buf, len);
}

/* Route a parsed message to the appropriate handler */
static void route_message(const char *cmd, const char *payload)
{
	if (strcmp(cmd, CMD_KEY) == 0 || cmd[0] == '\0') {
		/* KEY command or legacy format (empty cmd from ':' prefix) */
		keyboard_handle(payload);
	} else if (strcmp(cmd, CMD_AI) == 0 ||
		   strcmp(cmd, CMD_AIV) == 0 ||
		   strcmp(cmd, CMD_AIA) == 0) {
		fprintf(stderr, "AI query (not implemented): %s\n", payload);
		nwpid_send(CMD_ERR, "NOTIMPL:AI");
	} else if (strcmp(cmd, CMD_CAM) == 0) {
		fprintf(stderr, "Camera command (not implemented): %s\n", payload);
		nwpid_send(CMD_ERR, "NOTIMPL:CAM");
	} else if (strcmp(cmd, CMD_SYS) == 0) {
		fprintf(stderr, "System command (not implemented): %s\n", payload);
		nwpid_send(CMD_ERR, "NOTIMPL:SYS");
	} else {
		fprintf(stderr, "Unknown command: %s\n", cmd);
		nwpid_send(CMD_ERR, "BADCMD");
	}
}

/*
 * Parse a complete line and route it.
 *
 * Formats:
 *   CMD:PAYLOAD  → route_message(CMD, PAYLOAD)
 *   :HEXDATA     → route_message("", HEXDATA)  (legacy keyboard)
 */
static void parse_line(char *line, int len)
{
	if (len <= 0)
		return;

	/* Legacy format: :HEXDATA */
	if (line[0] == ':') {
		keyboard_handle(line + 1);
		return;
	}

	/* NWPI format: CMD:PAYLOAD */
	char *colon = strchr(line, ':');
	if (!colon) {
		fprintf(stderr, "Malformed message (no colon): %s\n", line);
		return;
	}

	*colon = '\0';
	route_message(line, colon + 1);
}

static int serial_open(const char *path)
{
	struct termios tty;

	int fd = open(path, O_RDWR | O_NOCTTY);
	if (fd < 0) {
		fprintf(stderr, "Error opening %s: %s\n", path, strerror(errno));
		exit(1);
	}

	memset(&tty, 0, sizeof(tty));
	if (tcgetattr(fd, &tty) != 0) {
		fprintf(stderr, "Error from tcgetattr: %s\n", strerror(errno));
		exit(1);
	}

	cfsetospeed(&tty, B115200);
	cfsetispeed(&tty, B115200);
	tty.c_cflag &= ~PARENB;
	tty.c_cflag &= ~CSTOPB;
	tty.c_cflag &= ~CSIZE;
	tty.c_cflag |= CS8;
	tty.c_cflag &= ~CRTSCTS;
	tty.c_cc[VMIN] = 1;
	tty.c_cc[VTIME] = 5;
	tty.c_cflag |= CREAD | CLOCAL;
	cfmakeraw(&tty);
	tcflush(fd, TCIFLUSH);

	if (tcsetattr(fd, TCSANOW, &tty) != 0) {
		fprintf(stderr, "Error from tcsetattr: %s\n", strerror(errno));
		exit(1);
	}

	return fd;
}

static void sig_handler(int signo)
{
	fprintf(stderr, "received signal %d, cleaning up\n", signo);
	keyboard_cleanup();
	if (tty_fd != -1)
		close(tty_fd);
	exit(0);
}

static void main_loop(void)
{
	struct pollfd pfd = {
		.fd = tty_fd,
		.events = POLLIN,
	};

	char linebuf[NWPI_MAX_MSG];
	int linepos = 0;

	while (1) {
		int timeout_ms = keyboard_arrows_held() ? MOUSE_INTERVAL_MS : -1;
		int ret = poll(&pfd, 1, timeout_ms);

		if (ret > 0 && (pfd.revents & POLLIN)) {
			char buf[256];
			int n = read(tty_fd, buf, sizeof(buf));
			if (n <= 0) {
				fprintf(stderr, "Read error: %s\n", strerror(errno));
				exit(1);
			}
			for (int i = 0; i < n; i++) {
				if (buf[i] == '\n') {
					linebuf[linepos] = '\0';
					parse_line(linebuf, linepos);
					linepos = 0;
				} else if (linepos < (int)sizeof(linebuf) - 1) {
					linebuf[linepos++] = buf[i];
				}
			}
		}

		if (ret == 0 && keyboard_arrows_held())
			keyboard_emit_mouse();
	}
}

int main(int argc, char *argv[])
{
	const char *tty_path = (argc > 1) ? argv[1] : DEFAULT_TTY;

	if (signal(SIGINT, sig_handler) == SIG_ERR ||
	    signal(SIGTERM, sig_handler) == SIG_ERR) {
		fprintf(stderr, "Failed to register signal handlers\n");
		exit(1);
	}

	fprintf(stderr, "Starting nwpid on %s\n", tty_path);
	keyboard_init();
	tty_fd = serial_open(tty_path);
	main_loop();
	return 0;
}
