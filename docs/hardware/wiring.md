# Wiring Guide

## Pin Mapping

| Signal       | STM32 Pin | Pi GPIO | Pi Header Pin | Notes                        |
|--------------|-----------|---------|---------------|------------------------------|
| SPI CLK      | PA5       | GPIO11  | 23            | SPI0 SCLK                   |
| SPI CS       | PA6       | GPIO8   | 24            | SPI0 CE0 (PA6 repurposed)   |
| SPI MOSI     | PA7       | GPIO10  | 19            | SPI0 MOSI                   |
| UART TX->RX  | PD8       | GPIO15  | 10            | STM32 TX to Pi RX            |
| UART RX<-TX  | PC11      | GPIO14  | 8             | Pi TX to STM32 RX (unused)   |
| Power        | PB9       | --      | --            | MOSFET gate control          |
| Ground       | GND       | GND     | 6 (or any)    | Common ground                |
| 5V supply    | --        | --      | 2 or 4        | MOSFET drain to Pi 5V rail   |

PA6 is repurposed as a chip-select output. It is not used as SPI MISO; the
display link is one-directional (STM32 to Pi). Only the UART TX line (PD8 to
GPIO15) carries data from the calculator to the Pi. PC11/GPIO14 is wired but
not actively used.

All STM32 pins listed are 5V-tolerant. The Pi GPIO is 3.3V. No level shifters
are needed.


## Raspberry Pi Header Pinout

Active pins are marked with `>>>` or `<<<`. Arrow direction indicates signal
flow (out from Pi or into Pi).

```
                     Pi Header (top-down view)
                     ========================

                 3V3 [ 1]  [ 2] 5V <<<-------- MOSFET drain
           SDA1/GP2 [ 3]  [ 4] 5V <<<-------- (alt 5V)
           SCL1/GP3 [ 5]  [ 6] GND <<<-------- NumWorks GND
                GP4 [ 7]  [ 8] GP14/TXD        (PC11, unused)
                 GND [ 9]  [10] GP15/RXD <<<--- PD8 (UART TX)
               GP17 [11]  [12] GP18
               GP27 [13]  [14] GND
               GP22 [15]  [16] GP23
                3V3 [17]  [18] GP24
     MOSI/GP10 [19] <<<--- PA7  [20] GND
               GP9  [21]  [22] GP25
     SCLK/GP11 [23] <<<--- PA5  [24] GP8/CE0 <<<--- PA6 (CS)
                GND [25]  [26] GP7
               GP0  [27]  [28] GP1
               GP5  [29]  [30] GND
               GP6  [31]  [32] GP12
              GP13  [33]  [34] GND
              GP19  [35]  [36] GP16
              GP26  [37]  [38] GP20
                GND [39]  [40] GP21
```

Summary of connected pins: 2 (5V), 6 (GND), 10 (RXD), 19 (MOSI), 23 (SCLK),
24 (CE0).


## MOSFET Power Circuit

P-channel MOSFET (NTR1P02LT1 or equivalent SOT-23).

```
    Vbat (Battery +)
        |
        |----+----------+
        |    |          |
        |  [10kR]       |
        |    |        Source
        |    +------- Gate (PB9)
        |             Drain
        |               |
        +               |
        |          Pi 5V (pin 2/4)
        |
       GND (common)
```

### Operating states

| PB9 State       | Gate Voltage | V_GS     | MOSFET  | Pi Power |
|-----------------|-------------|----------|---------|----------|
| Driven LOW      | ~0V         | -Vbat    | ON      | On       |
| HIGH or Hi-Z    | Vbat        | ~0V      | OFF     | Off      |

When PB9 is driven low, the gate is pulled well below the source (which sits at
Vbat), turning the P-channel MOSFET on and supplying power to the Pi through the
drain. The 10 k-ohm pull-up resistor ensures the MOSFET defaults to off when the
STM32 is in reset or the pin is not actively driven.

The MOSFET and resistor are soldered onto the pads of the removed SD card slot
on the NumWorks PCB.


## SPI Wiring Detail

```
  STM32                   Raspberry Pi
  -----                   ------------
  PA5  (SPI1_SCK)  -----> GPIO11 / SCLK  (pin 23)
  PA6  (CS output)  -----> GPIO8  / CE0   (pin 24)
  PA7  (SPI1_MOSI) -----> GPIO10 / MOSI  (pin 19)
```

There is no MISO connection. The framebuffer data flows only from the STM32 to
the Pi. PA6 is configured as a general-purpose output on the STM32 side and is
directly connected to the Pi SPI0 CE0 input.


## UART Wiring Detail

```
  STM32                   Raspberry Pi
  -----                   ------------
  PD8  (USART3_TX) -----> GPIO15 / RXD   (pin 10)
  PC11 (USART3_RX) <----- GPIO14 / TXD   (pin 8)    [not used]
```

Only the TX direction (PD8 to GPIO15) is used. The calculator sends key state
changes as hex-encoded 64-bit bitmaps over this line at 115200 baud 8N1.
