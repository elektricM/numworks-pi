# Porting Notes — Differences from zardam's epsilon@rpi

This documents the changes made when porting zardam's Raspberry Pi integration from his Epsilon fork to Upsilon.

## App Constructor

- **zardam:** `App(Container*, Snapshot*)` — container passed explicitly.
- **Upsilon:** `App(Snapshot*)` — container accessed via `sharedAppsContainer()`.

## Build Integration

- **zardam:** RPI code always compiled in, no conditional.
- **Upsilon:** All RPI code gated behind `ENABLE_RPI` flag. Builds without it are unaffected.

## ISR (Interrupt Service Routine)

- **zardam:** RPI EXTI handler placed directly in the vector table.
- **Upsilon:** ISR wrapped in `#ifdef ENABLE_RPI` guards.

## Driver File Location

- **zardam:** `ion/src/device/rpi.cpp`
- **Upsilon:** `ion/src/device/shared/drivers/rpi.cpp`

## Header Split

- **zardam:** Single `rpi.h`.
- **Upsilon:** Split into public `ion/include/ion/rpi.h` (API for apps) and private `drivers/rpi.h` (hardware internals).

## Simulator Stub

- **zardam:** Not addressed; simulator builds with RPI code would fail.
- **Upsilon:** Dummy stub provided so `PLATFORM=simulator ENABLE_RPI=1` compiles cleanly.

## UART

- **zardam:** Worked out of the box because the bench/consoleuart flavor was active by default in his build configuration.
- **Upsilon:** Required an explicit fix to activate the `consoleuart` flavor when `ENABLE_RPI=1` (see `uart-fix.md`).

## Auto-Sleep Prevention

Added `isPowered()` checks in `SuspendTimer` and `BacklightDimmingTimer`. Without these, the STM32 enters Stop mode after idle timeout, cutting power to the Pi via the 3.3V rail.

## PA6 Pull-Up

Enabled the internal pull-up on PA6 (the Pi's interrupt line). zardam had this commented out. Without the pull-up, the floating input triggers spurious EXTI interrupts.

## Register API

Identical between zardam's Epsilon and Upsilon. No changes needed for RCC, GPIO, or USART register access.
