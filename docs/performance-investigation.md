# Display Performance Investigation

Date: 2026-02-13
Driver: drm-spifb v1.2 (NEON), kernel 6.12.62+rpt-rpi-v7
Hardware: Pi Zero 2W, 320x240 SPI display, 640x480 virtual

## Problem

After reflashing Trixie, glxgears measured ~37 FPS vs the ~50 FPS expected. Same driver code, same kernel version, same hardware.

## Resolution

The 37 FPS was from contaminated benchmarks (HDMI dual-output + background IO). After cleanup: 43 FPS with the original 5-chunk SPI, **48 FPS** after fixing the self-imposed chunk limit. This matches the old ~50 FPS (which was itself approximate). The README's "62.5 MHz" claim was for core_freq=250 (older Pi OS); Trixie uses core_freq=400 giving 66.67 MHz — actually faster.

## System Configuration

| Parameter | Value |
|-----------|-------|
| SPI requested speed | 70 MHz |
| Core clock | 400 MHz (stable, no scaling) |
| Actual SPI clock | 66.67 MHz (400/6, nearest even divisor) |
| Frame size | 320 x 240 x 2 = 153,600 bytes |
| Theoretical SPI time | 153,600 x 8 / 66.67M = 18.4 ms (54 FPS) |
| Compositor | labwc 0.9.2 |
| Renderer | pixman (forced by `labwc-pi`, vc4 GPU has no MMU) |
| CPU governor | ondemand (1000 MHz max, 600 MHz min) |
| Throttling | None (0x0, 42°C) |

## Root Causes Found

### 1. SPI DMA chunk overhead (37 FPS → 48 FPS fix)

The driver split each 153 KB frame into **5 x 32 KB SPI transfers** within a single `spi_message`. Each transfer incurs DMA setup overhead in the bcm2835 SPI driver. This added ~4.2 ms per frame.

The 32 KB limit was self-imposed (`#define SPI_CHUNK_SIZE 32768`) with a comment claiming it matched a "BCM2835 DMA limit". This limit does not exist — the bcm2835 DMA engine supports transfers up to 1 GB per descriptor via scatter-gather.

**Fix**: Single transfer per frame (`SPI_CHUNK_SIZE = 163840`).

| Metric | 5 chunks | 1 chunk |
|--------|----------|---------|
| SPI transfer time | 22.6 ms | 20.8 ms |
| glxgears FPS | 43 | **48** |
| DMA overhead | ~4.2 ms | ~2.4 ms |

### 2. Uncached memory reads during scaling (CPU waste, not FPS-limiting)

The CPU scaling function reads the compositor's framebuffer, which is allocated via `drm_gem_dma` (backed by `dma_alloc_wc()`). On ARM without hardware cache coherency, this memory is mapped **write-combine / uncached**. Reads from WC memory are extremely slow.

Reading 1.2 MB (640x480x4 XRGB8888) from uncached memory takes **~10 ms** per frame. This is 23x slower than the same operation in cached userspace memory (0.44 ms).

This does NOT limit FPS because the system is SPI-bound (20.8 ms > 10 ms + compositor time). But it wastes ~50% of one CPU core.

### 3. Previous 37 FPS measurements were contaminated

Early benchmarks showing 37 FPS were affected by:
- HDMI display connected (dual-output compositing overhead)
- Stale background `grep -r` process causing 28% iowait on SD card
- Both were eliminated before the final measurements

## Instrumented Timing Breakdown

Added `ktime_get()` instrumentation to the driver. Steady-state results with single-chunk SPI:

| Phase | Time | Description |
|-------|------|-------------|
| **scale** | 9.9 ms | CPU reads uncached framebuffer, converts XRGB8888→RGB565 BE with 2:1 downscale |
| **wait** | 4.6 ms | Blocked on `wait_for_completion()` for previous SPI transfer |
| **spi** | 20.8 ms | DMA SPI transfer of 153,600 bytes at 66.67 MHz |
| **compositor** | ~6.4 ms | labwc/pixman renders next frame (inferred from frame interval) |

Frame pipeline with double buffering:
```
Frame N:   [--- scale 10ms ---][wait 4.6ms][spi_async→]
                                            [---- SPI N: 20.8ms ----]
                               [compositor ~6.4ms][--- scale 10ms ---][wait...]
Frame N+1:                                        ^                    ^
```

Total frame time: scale + wait + compositor = 10 + 4.6 + 6.4 = ~21 ms = **48 FPS**

The system is **SPI-bound**: frame time ≈ SPI transfer time (20.8 ms).

## NEON SIMD Results

NEON inline assembly for the 2:1 downscale path (8 pixels/iteration with VLD2 deinterleave) showed **no improvement** because the bottleneck is uncached memory reads, not compute:

| Implementation | Userspace (cached) | Kernel (uncached) |
|---------------|-------------------|-------------------|
| Scalar | 459 us/frame | ~10 ms/frame |
| NEON | 438 us/frame (5% faster) | ~10 ms/frame (identical) |

The NEON code is correct (all 15 test patterns pass) but irrelevant when memory access dominates.

## SPI Statistics

From `/sys/class/spi_master/spi0/statistics/`:
- All transfers are `spi_async` (zero `spi_sync`)
- Zero errors, zero timeouts
- DMA-driven (0 SPI IRQs)

## Current Performance (single-chunk driver)

```
glxgears: 48 FPS (steady)
CPU: 8.6% user, 13.2% sys, 77.9% idle
```

## Optimization Opportunities

### Already done
- [x] Single SPI transfer per frame: +5 FPS (43→48)

### Potential (won't increase FPS, will reduce CPU)
- [ ] Cached copy before scaling: memcpy 1.2 MB to kmalloc'd buffer, then scale from cached. Could reduce scale from 10 ms to ~1.5 ms (memcpy ~1 ms + scale ~0.5 ms). Saves ~40% CPU.
- [ ] Native 320x240 rendering: eliminate scaling entirely. Compositor renders at physical resolution. Scale cost → 0. But UI elements become very large.
- [ ] Smaller virtual resolution: 480x360 (1.5x) reads 691 KB instead of 1.2 MB → ~6 ms scale.

### Potential (could increase FPS)
- [ ] Reduce SPI DMA overhead: the 2.4 ms single-transfer overhead may come from bcm2835 SPI driver CS/FIFO setup. Investigating `spi_controller.max_transfer_size` or pre-mapped DMA buffers could help.
- [ ] Higher SPI clock: the STM32 slave may tolerate >70 MHz. Testing 80-100 MHz would directly increase FPS. At 100 MHz (CDIV=4, actual 100 MHz): 153,600 x 8 / 100M = 12.3 ms = ~81 FPS theoretical.
- [ ] Reduce compositor overhead: currently ~6.4 ms for pixman compositing at 640x480. Smaller resolution would reduce this proportionally.

## How to Reproduce

Build instrumented driver:
```bash
# In drm-spifb-core.c, add ktime instrumentation to pipe_update and submit_frame
# See git history for the timing patch
cd /tmp/neon-build && make
sudo insmod drm-spifb.ko
# Run glxgears, check dmesg for timing reports every 500 frames
```
