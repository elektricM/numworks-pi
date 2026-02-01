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
#define MOUSE_SPEED 2
#define MOUSE_INTERVAL_MS 20  /* ~50 Hz mouse update rate */

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
  {"home"},     // not handled here
  {"power"},    // toggle mouse mode
  {NULL},
  {NULL},
  {NULL},
  {NULL},
  {"shift",     {KEY_LEFTSHIFT,   KEY_LEFTSHIFT}},
  {"alpha",     {KEY_CAPSLOCK,    KEY_CAPSLOCK}},
  {"xnt"},      // Switch to first keymap
  {"var"},      // Switch to second keymap
  {"toolbox",   {KEY_RIGHTALT,    KEY_RIGHTALT}},
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
  {"P (",       {KEY_P,           KEY_5}},
  {"Q )",       {KEY_Q,           KEY_MINUS}},
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
  {"space -",   {KEY_SPACE,       KEY_KPMINUS}},
  {NULL},
  {"? 0",       {KEY_QUESTION,    KEY_0}},
  {"! .",       {KEY_COMMA,       KEY_SEMICOLON}},
  {"x10^x",     {KEY_LEFTCTRL,    KEY_LEFTCTRL}},
  {"ans",       {KEY_LEFTALT,     KEY_LEFTALT}},
  {"exe",       {KEY_ENTER,       KEY_EQUAL}},
};

int fd = -1;
int mouse_mode = 0;
u_int64_t current_scan = 0;

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
  if (signo == SIGINT) {
    printf("received SIGINT\n");
    if(fd != -1) {
      ioctl(fd, UI_DEV_DESTROY);
      close(fd);
    }
    exit(0);
  }
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
   usetup.id.bustype = BUS_USB;
   usetup.id.vendor = 0x1234;
   usetup.id.product = 0x5678;
   strcpy(usetup.name, "NW Keyboard");

   ioctl(fd, UI_DEV_SETUP, &usetup);
   ioctl(fd, UI_DEV_CREATE);
}

/* Emit mouse movement for any arrow keys currently held */
void emit_mouse_movement(void)
{
  int moved = 0;
  if(current_scan & (1ULL<<0)) { emit(EV_REL, REL_X, -MOUSE_SPEED); moved = 1; }
  if(current_scan & (1ULL<<1)) { emit(EV_REL, REL_Y, -MOUSE_SPEED); moved = 1; }
  if(current_scan & (1ULL<<2)) { emit(EV_REL, REL_Y,  MOUSE_SPEED); moved = 1; }
  if(current_scan & (1ULL<<3)) { emit(EV_REL, REL_X,  MOUSE_SPEED); moved = 1; }
  if(moved) emit(EV_SYN, SYN_REPORT, 0);
}

void process(u_int64_t scan) {
  static u_int64_t old_scan = 0;
  static int mode = 0;
  int index;
  u_int64_t changed = old_scan ^ scan;
  u_int64_t mask;

  /* Toggle mouse mode on power button (bit 7) press */
  if((changed & (1ULL<<7)) && (scan & (1ULL<<7))) {
    mouse_mode = !mouse_mode;
    printf("Mouse mode: %s\n", mouse_mode ? "ON" : "OFF");
  }

  /* Switch keymap mode */
  if(scan & (1ULL<<14))
    mode = 0;
  else if(scan & (1ULL<<15))
    mode = 1;

  if(mouse_mode) {
    /* Arrow keys (bits 0-3): handled by emit_mouse_movement() on timer */
    /* Handle non-arrow key changes normally */
    for(index = 4; index < NUM_KEYS; index++) {
      if(index == 7) continue;
      mask = 1ULL << index;
      if(keymap[index].code[mode] != 0 && (changed & mask))
        emit(EV_KEY, keymap[index].code[mode], (scan & mask) ? 1 : 0);
    }
    if(changed & ~0xFULL)
      emit(EV_SYN, SYN_REPORT, 0);
  } else {
    /* Keyboard mode: all keys as normal */
    for(index = 0; index < NUM_KEYS; index++) {
      if(index == 7) continue;
      mask = 1ULL << index;
      if(keymap[index].code[mode] != 0 && (changed & mask))
        emit(EV_KEY, keymap[index].code[mode], (scan & mask) ? 1 : 0);
    }
    emit(EV_SYN, SYN_REPORT, 0);
  }

  current_scan = scan;
  old_scan = scan;
}

void serial_loop() {
  struct termios tty;
  int tty_fd = open("/dev/ttyS0", O_RDWR | O_NOCTTY);

  if (tty_fd < 0) {
    printf("Error %d opening tty: %s\n", errno, strerror(errno));
    exit(1);
  }

  memset(&tty, 0, sizeof(tty));
  if(tcgetattr(tty_fd, &tty) != 0) {
    printf("Error %d from tcgetattr: %s\n", errno, strerror(errno));
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
    printf("Error %d from tcsetattr: %s\n", errno, strerror(errno));
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
        printf("Read error\n");
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

int main(void)
{
   if (signal(SIGINT, sig_handler) == SIG_ERR) {
     printf("can't catch SIGINT\n");
     exit(1);
   }
   input_setup();
   serial_loop();
   return 0;
}
