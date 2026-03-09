#include "keyboard.h"

#include <linux/uinput.h>
#include <linux/input.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <time.h>

#define NUM_KEYS 53
#define NUM_MODES 2
#define MOUSE_INTERVAL_MS 8   /* ~120 Hz mouse update rate */
#define MOUSE_MIN_SPEED 1
#define MOUSE_MAX_SPEED 4
#define MOUSE_RAMP_MS 600

typedef struct {
	char *name;
	int code[NUM_MODES];
} key;

static key keymap[NUM_KEYS] = {
	{"left",      {KEY_LEFT,        KEY_LEFT}},
	{"up",        {KEY_UP,          KEY_UP}},
	{"down",      {KEY_DOWN,        KEY_DOWN}},
	{"right",     {KEY_RIGHT,       KEY_RIGHT}},
	{"ok",        {BTN_LEFT,        BTN_LEFT}},
	{"back",      {BTN_RIGHT,       BTN_RIGHT}},
	{"home"},
	{"power"},
	{NULL},
	{NULL},
	{NULL},
	{NULL},
	{"shift",     {KEY_LEFTSHIFT,   KEY_LEFTSHIFT}},
	{"alpha",     {KEY_CAPSLOCK,    KEY_CAPSLOCK}},
	{"xnt"},
	{"var"},
	{"toolbox",   {KEY_TAB,         KEY_TAB}},
	{"backspace", {KEY_BACKSPACE,   KEY_ESC}},
	{"A",         {KEY_A,           KEY_F1}},
	{"B",         {KEY_B,           KEY_F2}},
	{"C",         {KEY_C,           KEY_F3}},
	{"D",         {KEY_D,           KEY_F4}},
	{"E ,",       {KEY_E,           KEY_F5}},
	{"F",         {KEY_F,           KEY_F6}},
	{"G",         {KEY_G,           KEY_F7}},
	{"H",         {KEY_H,           KEY_F8}},
	{"I",         {KEY_I,           KEY_F9}},
	{"J",         {KEY_J,           KEY_F10}},
	{"K",         {KEY_K,           KEY_F11}},
	{"L",         {KEY_L,           KEY_F12}},
	{"M 7",       {KEY_M,           KEY_7}},
	{"N 8",       {KEY_N,           KEY_8}},
	{"O 9",       {KEY_O,           KEY_9}},
	{"P (",       {KEY_P,           KEY_LEFTBRACE}},
	{"Q )",       {KEY_Q,           KEY_RIGHTBRACE}},
	{NULL},
	{"R 4",       {KEY_R,           KEY_4}},
	{"S 5",       {KEY_S,           KEY_5}},
	{"T 6",       {KEY_T,           KEY_6}},
	{"U *",       {KEY_U,           KEY_KPASTERISK}},
	{"V /",       {KEY_V,           KEY_KPSLASH}},
	{NULL},
	{"W 1",       {KEY_W,           KEY_1}},
	{"X 2",       {KEY_X,           KEY_2}},
	{"Y 3",       {KEY_Y,           KEY_3}},
	{"Z +",       {KEY_Z,           KEY_KPPLUS}},
	{"space -",   {KEY_SPACE,       KEY_MINUS}},
	{NULL},
	{"? 0",       {KEY_SLASH,       KEY_0}},
	{"! .",       {KEY_DOT,         KEY_SEMICOLON}},
	{"x10^x",     {KEY_LEFTCTRL,    KEY_LEFTCTRL}},
	{"ans",       {KEY_LEFTALT,     KEY_LEFTALT}},
	{"exe",       {KEY_ENTER,       KEY_EQUAL}},
};

static int fd = -1;
static int mouse_mode = 0;
static uint64_t current_scan = 0;
static uint64_t old_scan = 0;
static int mode = 0;
static struct timespec mouse_start;
static int mouse_active = 0;

static void emit(int type, int code, int val)
{
	struct input_event ie = {
		.type = type,
		.code = code,
		.value = val,
	};
	write(fd, &ie, sizeof(ie));
}

static int mouse_speed(void)
{
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	long held_ms = (now.tv_sec - mouse_start.tv_sec) * 1000
	             + (now.tv_nsec - mouse_start.tv_nsec) / 1000000;
	if (held_ms < 0) held_ms = 0;
	if (held_ms > MOUSE_RAMP_MS) held_ms = MOUSE_RAMP_MS;
	return MOUSE_MIN_SPEED + (MOUSE_MAX_SPEED - MOUSE_MIN_SPEED) * held_ms / MOUSE_RAMP_MS;
}

void keyboard_init(void)
{
	struct uinput_setup usetup;

	if (fd != -1) {
		ioctl(fd, UI_DEV_DESTROY);
		close(fd);
	}

	fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
	if (fd == -1) {
		perror("open /dev/uinput");
		exit(1);
	}

	ioctl(fd, UI_SET_EVBIT, EV_KEY);
	for (int i = 0; i < NUM_KEYS; i++) {
		for (int j = 0; j < NUM_MODES; j++) {
			if (keymap[i].code[j] != 0)
				ioctl(fd, UI_SET_KEYBIT, keymap[i].code[j]);
		}
	}

	ioctl(fd, UI_SET_KEYBIT, BTN_LEFT);
	ioctl(fd, UI_SET_KEYBIT, BTN_MIDDLE);
	ioctl(fd, UI_SET_KEYBIT, BTN_RIGHT);
	ioctl(fd, UI_SET_EVBIT, EV_REL);
	ioctl(fd, UI_SET_RELBIT, REL_X);
	ioctl(fd, UI_SET_RELBIT, REL_Y);

	memset(&usetup, 0, sizeof(usetup));
	usetup.id.bustype = BUS_VIRTUAL;
	snprintf(usetup.name, sizeof(usetup.name), "NW Keyboard");

	if (ioctl(fd, UI_DEV_SETUP, &usetup) < 0) {
		perror("ioctl UI_DEV_SETUP");
		exit(1);
	}
	if (ioctl(fd, UI_DEV_CREATE) < 0) {
		perror("ioctl UI_DEV_CREATE");
		exit(1);
	}
}

void keyboard_cleanup(void)
{
	if (fd != -1) {
		ioctl(fd, UI_DEV_DESTROY);
		close(fd);
		fd = -1;
	}
}

void keyboard_handle(const char *payload)
{
	uint64_t scan;
	if (sscanf(payload, "%16llx", (unsigned long long *)&scan) != 1)
		return;

	uint64_t changed = old_scan ^ scan;
	if (!changed)
		goto done;

	/* Toggle mouse mode on power button (bit 7) press */
	if ((changed & (1ULL << 7)) && (scan & (1ULL << 7))) {
		static struct timespec last_toggle = {0, 0};
		struct timespec now;
		clock_gettime(CLOCK_MONOTONIC, &now);
		long elapsed_ms = (now.tv_sec - last_toggle.tv_sec) * 1000
		                + (now.tv_nsec - last_toggle.tv_nsec) / 1000000;
		if (elapsed_ms > 300) {
			mouse_mode = !mouse_mode;
			mouse_active = 0;
			last_toggle = now;
			fprintf(stderr, "Mouse mode: %s\n", mouse_mode ? "ON" : "OFF");
		}
	}

	/* Switch keymap mode */
	if (scan & (1ULL << 14))
		mode = 0;
	else if (scan & (1ULL << 15))
		mode = 1;

	/* In mouse mode, skip arrow bits (0-3) */
	uint64_t key_changes = mouse_mode ? (changed & ~0xFULL) : changed;
	int emitted = 0;

	while (key_changes) {
		int bit = __builtin_ctzll(key_changes);
		key_changes &= key_changes - 1;

		if (bit == 7 || bit >= NUM_KEYS) continue;
		if (keymap[bit].code[mode] == 0) continue;

		emit(EV_KEY, keymap[bit].code[mode], (scan & (1ULL << bit)) ? 1 : 0);
		emitted = 1;
	}

	if (emitted)
		emit(EV_SYN, SYN_REPORT, 0);

	if (mouse_mode && (changed & 0xF) && !(scan & 0xF))
		mouse_active = 0;

done:
	current_scan = scan;
	old_scan = scan;
}

void keyboard_emit_mouse(void)
{
	int arrows = current_scan & 0xF;
	if (!arrows) {
		mouse_active = 0;
		return;
	}
	if (!mouse_active) {
		clock_gettime(CLOCK_MONOTONIC, &mouse_start);
		mouse_active = 1;
	}
	int speed = mouse_speed();
	int moved = 0;
	if (arrows & (1 << 0)) { emit(EV_REL, REL_X, -speed); moved = 1; }
	if (arrows & (1 << 1)) { emit(EV_REL, REL_Y, -speed); moved = 1; }
	if (arrows & (1 << 2)) { emit(EV_REL, REL_Y,  speed); moved = 1; }
	if (arrows & (1 << 3)) { emit(EV_REL, REL_X,  speed); moved = 1; }
	if (moved) emit(EV_SYN, SYN_REPORT, 0);
}

int keyboard_arrows_held(void)
{
	return mouse_mode && (current_scan & 0xF);
}
