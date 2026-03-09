#ifndef NWPID_KEYBOARD_H
#define NWPID_KEYBOARD_H

#include <stdint.h>

/* Initialize uinput device for keyboard + mouse emulation */
void keyboard_init(void);

/* Clean up uinput device */
void keyboard_cleanup(void);

/* Process a KEY payload (16 hex chars → 64-bit scan bitmap) */
void keyboard_handle(const char *payload);

/* Emit mouse movement if arrows are held in mouse mode.
 * Called on poll timeout for continuous movement. */
void keyboard_emit_mouse(void);

/* Returns non-zero if mouse mode is active and arrows are held */
int keyboard_arrows_held(void);

#endif /* NWPID_KEYBOARD_H */
