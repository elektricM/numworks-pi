#include <linux/uinput.h>
#include <string.h>
#include <stdio.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <linux/input.h>
#include <termios.h>
#include <errno.h>
#include <stdlib.h>
#include <signal.h>
#include <poll.h>
#include <time.h>

#define NUM_KEYS 53
#define NUM_MODES 2
#define MOUSE_INTERVAL_MS 8   /* ~120 Hz mouse update rate */
#define DEFAULT_TTY "/dev/ttyS0"

/* Mouse acceleration: ramps from MIN to MAX speed over RAMP_MS */
#define MOUSE_MIN_SPEED 1
#define MOUSE_MAX_SPEED 4
#define MOUSE_RAMP_MS 600

typedef struct {
  char *name;
  int code[NUM_MODES];
} key;

key keymap[NUM_KEYS] = {
  {"left",      {KEY_LEFT,        KEY_LEFT}},
  {"up",        {KEY_UP,          KEY_UP}},
  {"down",      {KEY_DOWN,        KEY_DOWN}},
  {"right",     {KEY_RIGHT,       KEY_RIGHT}},
  {"ok",        {BTN_LEFT,        BTN_LEFT}},
  {"back",      {BTN_RIGHT,       BTN_RIGHT}},
  {"home"},     // firmware intercepts — never reaches daemon
  {"power"},    // toggle mouse mode
  {NULL},
  {NULL},
  {NULL},
  {NULL},
  {"shift",     {KEY_LEFTSHIFT,   KEY_LEFTSHIFT}},
  {"alpha",     {KEY_CAPSLOCK,    KEY_CAPSLOCK}},
  {"xnt"},      // Switch to first keymap
  {"var"},      // Switch to second keymap
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

int fd = -1;
int mouse_mode = 0;
u_int64_t current_scan = 0;
struct timespec mouse_start;  /* when arrows were first held */
int mouse_active = 0;         /* arrows currently held */

void emit(int type, int code, int val)
{
   struct input_event ie;
   ie.type = type;
   ie.code = code;
   ie.value = val;
   ie.input_event_sec = 0;
   ie.input_event_usec = 0;
   write(fd, &ie, sizeof(ie));
}

void sig_handler(int signo)
{
  fprintf(stderr, "received signal %d, cleaning up\n", signo);
  if(fd != -1) {
    ioctl(fd, UI_DEV_DESTROY);
    close(fd);
  }
  exit(0);
}

void input_setup(void)
{
   int i, j;
   struct uinput_setup usetup;

   if(fd != -1) {
     ioctl(fd, UI_DEV_DESTROY);
     close(fd);
     fd = -1;
   }

   fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
   if(fd == -1) {
     perror("open /dev/uinput");
     exit(1);
   }

   ioctl(fd, UI_SET_EVBIT, EV_KEY);
   for(i=0; i<NUM_KEYS; i++) {
     for(j=0; j< NUM_MODES; j++) {
       if(keymap[i].code[j] != 0)
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
   usetup.id.vendor = 0x0000;
   usetup.id.product = 0x0000;
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

/* Compute mouse speed based on how long arrows have been held */
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

/* Emit mouse movement for any arrow keys currently held */
void emit_mouse_movement(void)
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
  if (arrows & (1<<0)) { emit(EV_REL, REL_X, -speed); moved = 1; }
  if (arrows & (1<<1)) { emit(EV_REL, REL_Y, -speed); moved = 1; }
  if (arrows & (1<<2)) { emit(EV_REL, REL_Y,  speed); moved = 1; }
  if (arrows & (1<<3)) { emit(EV_REL, REL_X,  speed); moved = 1; }
  if (moved) emit(EV_SYN, SYN_REPORT, 0);
}

void process(u_int64_t scan) {
  static u_int64_t old_scan = 0;
  static int mode = 0;
  u_int64_t changed = old_scan ^ scan;

  if (!changed) goto done;

  /* Toggle mouse mode on power button (bit 7) press, debounced */
  if ((changed & (1ULL<<7)) && (scan & (1ULL<<7))) {
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
  if (scan & (1ULL<<14))
    mode = 0;
  else if (scan & (1ULL<<15))
    mode = 1;

  /* In mouse mode, skip arrow bits (0-3) — handled by emit_mouse_movement() */
  u_int64_t key_changes = mouse_mode ? (changed & ~0xFULL) : changed;
  int emitted = 0;

  /* Iterate only over changed bits */
  while (key_changes) {
    int bit = __builtin_ctzll(key_changes);
    key_changes &= key_changes - 1;  /* clear lowest set bit */

    if (bit == 7 || bit >= NUM_KEYS) continue;
    if (keymap[bit].code[mode] == 0) continue;

    emit(EV_KEY, keymap[bit].code[mode], (scan & (1ULL << bit)) ? 1 : 0);
    emitted = 1;
  }

  if (emitted)
    emit(EV_SYN, SYN_REPORT, 0);

  /* Reset acceleration when arrows are released in mouse mode */
  if (mouse_mode && (changed & 0xF) && !(scan & 0xF))
    mouse_active = 0;

done:
  current_scan = scan;
  old_scan = scan;
}

void serial_loop(const char *tty_path) {
  struct termios tty;
  int tty_fd = open(tty_path, O_RDWR | O_NOCTTY);

  if (tty_fd < 0) {
    fprintf(stderr, "Error opening %s: %s\n", tty_path, strerror(errno));
    exit(1);
  }

  memset(&tty, 0, sizeof(tty));
  if(tcgetattr(tty_fd, &tty) != 0) {
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
  tcflush(tty_fd, TCIFLUSH);

  if(tcsetattr(tty_fd, TCSANOW, &tty) != 0) {
    fprintf(stderr, "Error from tcsetattr: %s\n", strerror(errno));
    exit(1);
  }

  struct pollfd pfd;
  pfd.fd = tty_fd;
  pfd.events = POLLIN;

  char linebuf[1024];
  int linepos = 0;

  while(1) {
    /* In mouse mode with arrows held, poll with short timeout for continuous movement.
       Otherwise, block until serial data arrives. */
    int arrows_held = mouse_mode && (current_scan & 0xF);
    int timeout_ms = arrows_held ? MOUSE_INTERVAL_MS : -1;

    int ret = poll(&pfd, 1, timeout_ms);

    if(ret > 0 && (pfd.revents & POLLIN)) {
      /* Read available bytes and process complete lines */
      char buf[256];
      int n = read(tty_fd, buf, sizeof(buf));
      if(n <= 0) {
        fprintf(stderr, "Read error: %s\n", strerror(errno));
        exit(1);
      }
      for(int i = 0; i < n; i++) {
        if(buf[i] == '\n') {
          linebuf[linepos] = '\0';
          u_int64_t scan;
          if(linepos > 0 && sscanf(linebuf, ":%16llx", &scan) == 1) {
            process(scan);
          }
          linepos = 0;
        } else if(linepos < (int)sizeof(linebuf)-1) {
          linebuf[linepos++] = buf[i];
        }
      }
    }

    /* Emit mouse movement only on poll timeout (not on serial data arrival) */
    if(ret == 0 && mouse_mode && (current_scan & 0xF)) {
      emit_mouse_movement();
    }
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

   fprintf(stderr, "Starting nwinput on %s\n", tty_path);
   input_setup();
   serial_loop(tty_path);
   return 0;
}
