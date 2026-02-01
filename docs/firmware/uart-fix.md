# UART Fix — Calculator to Pi Communication

## Problem

The calculator's RPI app sends keyboard state over UART, but no data arrives at the Pi's `/dev/ttyS0`.

## Root Cause

Two independent issues prevented UART output on N0100 builds with `ENABLE_RPI=1`.

### 1. `console_uart.cpp` not linked

The build system uses a "flavor" mechanism to select console backends. The `consoleuart` flavor was only active for bench builds, so UART console code was never compiled into the RPI-enabled firmware.

**Fix** — in `build/targets.mak`, activate the `consoleuart` flavor when `ENABLE_RPI=1`:

```makefile
ifdef ENABLE_RPI
  $(eval $(call flavor_for_target,consoleuart,$(1)))
endif
```

### 2. USART3 peripheral clock not enabled

The N0100 `initClocks()` function never enables the USART3 clock on APB1. Without the clock, the peripheral is dead and transmits nothing.

**Fix** — in `board.cpp` (N0100 `initClocks()`), enable the USART3 clock:

```cpp
#ifdef ENABLE_RPI
  RCC.APB1ENR()->setUSART3EN(true);
#endif
```

## Result

Both fixes are gated behind `ENABLE_RPI` so they have zero effect on normal builds.

**Status:** Fixed and verified — UART data confirmed flowing on Pi.
