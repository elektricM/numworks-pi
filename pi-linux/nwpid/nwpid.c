/*
 * nwpid — NumWorks Pi Daemon
 *
 * Handles bidirectional UART communication between the NumWorks
 * calculator and Raspberry Pi using the NWPI protocol.
 *
 * Message format: CMD:PAYLOAD\n
 * Legacy format:  :HEXDATA\n  (treated as KEY:HEXDATA)
 *
 * Also negotiates display protocol 2 with the calculator and the drm-spifb
 * driver, and offers a control FIFO: every line written to /run/nwpid.ctl
 * is sent to the calculator; non-keyboard lines from the calculator are
 * appended to /run/nwpid.log (see nwpi-ctl).
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
#include <sys/stat.h>
#include <grp.h>

#define DEFAULT_TTY "/dev/ttyS0"
#define MOUSE_INTERVAL_MS 8
#define CTL_FIFO "/run/nwpid.ctl"
#define CALC_LOG "/run/nwpid.log"
#define CALC_LOG_MAX (64 * 1024)
#define DRIVER_PROTOCOL "/sys/module/drm_spifb/parameters/protocol"
/* Write 1: the driver resends the whole screen (drm-spifb 1.6+) */
#define DRIVER_REPAINT "/sys/module/drm_spifb/parameters/repaint"
/* Present: keep the display on the legacy full-frame protocol */
#define DISPLAY_LEGACY_FLAG "/etc/nwpi/display-legacy"

static int tty_fd = -1;
static int ctl_fd = -1;

enum dsp_state { DSP_LEGACY, DSP_ENABLING, DSP_V2 };
static enum dsp_state dsp_state = DSP_LEGACY;

/* Send a response over UART: CMD:PAYLOAD\n */
/*
 * The calculator polls its UART between keyboard scans and has no receive
 * buffer, so back-to-back bytes overrun it and whole lines get dropped.
 * Send one byte at a time with a gap; 0.2 ms was enough in testing.
 */
#define TX_GAP_US 500

static void tty_write_paced(const char *buf, int len)
{
	for (int i = 0; i < len; i++) {
		if (write(tty_fd, &buf[i], 1) != 1)
			return;
		tcdrain(tty_fd);
		usleep(TX_GAP_US);
	}
}

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
		tty_write_paced(buf, len);
}

static void calc_log(const char *cmd, const char *payload)
{
	struct stat st;
	FILE *f;

	if (stat(CALC_LOG, &st) == 0 && st.st_size > CALC_LOG_MAX)
		truncate(CALC_LOG, 0);
	f = fopen(CALC_LOG, "a");
	if (!f)
		return;
	fprintf(f, "%s:%s\n", cmd, payload);
	fclose(f);
}

static int set_driver_protocol(int proto)
{
	FILE *f = fopen(DRIVER_PROTOCOL, "w");
	int ok;

	if (!f) {
		fprintf(stderr, "Cannot set display protocol: %s\n", strerror(errno));
		return 0;
	}
	ok = fprintf(f, "%d\n", proto) > 0;
	ok = (fclose(f) == 0) && ok;
	return ok;
}

/*
 * The calculator fills the panel grey whenever it opens the Pi app, and an
 * idle desktop sends nothing: have the driver resend the screen. Older
 * drivers lack the parameter; a synthetic key makes some damage instead.
 */
static void driver_repaint(void)
{
	FILE *f = fopen(DRIVER_REPAINT, "w");

	if (!f) {
		keyboard_nudge();
		return;
	}
	fputs("1\n", f);
	fclose(f);
}

/*
 * Protocol switches never let a frame in one protocol reach the calculator
 * in the other one as pixels: going up, the calculator switches first (it
 * drops legacy frames as bad headers); going down, the driver does.
 */
static void dsp_handle(const char *payload)
{
	if (strcmp(payload, "2") == 0) {
		/* Bridge (re)started on legacy frames, and speaks protocol 2.
		 * The driver may still be on protocol 2 from before (the app was
		 * closed and reopened): its headers would land as pixels. */
		if (dsp_state != DSP_LEGACY) {
			set_driver_protocol(0);
			dsp_state = DSP_LEGACY;
		}
		if (access(DISPLAY_LEGACY_FLAG, F_OK) == 0) {
			driver_repaint();
			return;
		}
		/* An older driver (or none for this kernel) can't follow: keep
		 * the calculator on legacy frames rather than drop them all */
		if (access(DRIVER_PROTOCOL, W_OK) != 0) {
			fprintf(stderr, "Driver lacks protocol 2, staying on legacy\n");
			driver_repaint();
			return;
		}
		dsp_state = DSP_ENABLING;
		nwpid_send(CMD_DSP, "ON");
	} else if (strcmp(payload, "OK") == 0 && dsp_state == DSP_ENABLING) {
		if (!set_driver_protocol(2)) {
			nwpid_send(CMD_DSP, "OFF");
			dsp_state = DSP_LEGACY;
			return;
		}
		dsp_state = DSP_V2;
		driver_repaint();	/* first frame in protocol 2 */
		fprintf(stderr, "Display protocol 2 active\n");
	}
}

static void dsp_disable(void)
{
	if (dsp_state == DSP_LEGACY)
		return;
	set_driver_protocol(0);
	usleep(50000);		/* let an in-flight protocol 2 frame finish */
	nwpid_send(CMD_DSP, "OFF");
	dsp_state = DSP_LEGACY;
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
	} else if (strcmp(cmd, CMD_DSP) == 0) {
		calc_log(cmd, payload);
		dsp_handle(payload);
	} else if (strcmp(cmd, CMD_SYS) == 0) {
		calc_log(cmd, payload);
		if (strcmp(payload, "DFU") == 0) {
			/* The calculator is waiting to be flashed; the flash ends
			 * with a reset that cuts our power. Halt cleanly first. */
			fprintf(stderr, "Calculator entered DFU, powering off\n");
			dsp_state = DSP_LEGACY;
			if (fork() == 0) {
				execl("/usr/bin/systemctl", "systemctl", "poweroff", (char *)NULL);
				_exit(1);
			}
		} else {
			fprintf(stderr, "System command (not implemented): %s\n", payload);
			nwpid_send(CMD_ERR, "NOTIMPL:SYS");
		}
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
	/* The calculator ends lines with \r\n */
	while (len > 0 && line[len - 1] == '\r')
		line[--len] = '\0';
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
	dsp_disable();
	keyboard_cleanup();
	if (tty_fd != -1)
		close(tty_fd);
	exit(0);
}

/* Control FIFO: lines are forwarded to the calculator as-is */
static void ctl_open(void)
{
	unlink(CTL_FIFO);
	if (mkfifo(CTL_FIFO, 0620) != 0) {
		fprintf(stderr, "mkfifo %s: %s\n", CTL_FIFO, strerror(errno));
		return;
	}
	/* Writable by the dialout group, like the UART itself (so nwpi-ctl
	 * needs no sudo); mkfifo's mode is subject to the umask */
	struct group *gr = getgrnam("dialout");
	if (gr)
		chown(CTL_FIFO, 0, gr->gr_gid);
	chmod(CTL_FIFO, 0620);
	/* O_RDWR: never see EOF when writers come and go */
	ctl_fd = open(CTL_FIFO, O_RDWR | O_NONBLOCK);
}

static void ctl_read(void)
{
	static char line[128];
	static int pos;
	char buf[128];
	int n = read(ctl_fd, buf, sizeof(buf));

	for (int i = 0; i < n; i++) {
		if (buf[i] == '\n') {
			line[pos++] = '\n';
			if (pos > 1)
				tty_write_paced(line, pos);
			pos = 0;
		} else if (pos < (int)sizeof(line) - 2) {
			line[pos++] = buf[i];
		}
	}
}

static void main_loop(void)
{
	struct pollfd pfds[2] = {
		{ .fd = tty_fd, .events = POLLIN },
		{ .fd = ctl_fd, .events = POLLIN },
	};
	struct pollfd *pfd = &pfds[0];

	char linebuf[NWPI_MAX_MSG];
	int linepos = 0;

	/* Ask the calculator for its display protocol; it answers "DSP:2".
	 * A bare newline first ends any partial line left in its buffer.
	 * The calculator may still be in protocol 2 from before a Pi reboot,
	 * and SPI clock glitches during boot leave its receiver out of
	 * half-word alignment for good: go through legacy (whose per-frame
	 * slave deselect realigns it) before switching up again. Its OK
	 * reply is ignored while we are in DSP_LEGACY. */
	tty_write_paced("\n", 1);
	nwpid_send(CMD_DSP, "OFF");
	usleep(100000);
	nwpid_send(CMD_DSP, "?");

	while (1) {
		int timeout_ms = keyboard_arrows_held() ? MOUSE_INTERVAL_MS : -1;
		int ret = poll(pfds, ctl_fd >= 0 ? 2 : 1, timeout_ms);

		if (ret > 0 && (pfds[1].revents & POLLIN))
			ctl_read();

		if (ret > 0 && (pfd->revents & POLLIN)) {
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
	ctl_open();
	main_loop();
	return 0;
}
