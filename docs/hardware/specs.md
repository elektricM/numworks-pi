# Hardware Technical Specifications

## SPI Display Interface

| Parameter        | Value                                              |
|------------------|----------------------------------------------------|
| Bus              | SPI0                                               |
| Chip Select      | CS0 (active low)                                   |
| Mode             | `SPI_MODE_0` (CPOL=0, CPHA=0)                     |
| Requested clock  | 70 MHz                                             |
| Effective clock  | 62.5 MHz (BCM2835 divider: 250 MHz / 4)           |
| Resolution       | 320 x 240 pixels                                   |
| Pixel format     | RGB565 (16-bit, big-endian on the wire)            |
| Frame size       | 320 x 240 x 2 = 153,600 bytes                     |
| Transfer method  | 5 sequential DMA transfers of 32,768 bytes (32 KB) |
| Byte order       | `cpu_to_be16` swap before transmission             |

The BCM2835 SPI peripheral only supports power-of-two clock dividers against its
250 MHz core clock. Requesting 70 MHz results in a divider of 4, yielding
62.5 MHz effective. This is the fastest rate the hardware can produce below the
requested value.

Each frame is split into five 32 KB SPI messages because the Linux `spidev`
kernel driver limits a single transfer to 32 KB by default. The fifth transfer
carries the remaining 153,600 - (4 x 32,768) = 22,528 bytes.

PA6 (normally SPI1_MISO on the STM32) is repurposed as the chip-select signal.
There is no MISO line; the display link is unidirectional (Pi to NumWorks).


## UART Keyboard Interface

| Parameter  | Value                                     |
|------------|-------------------------------------------|
| Peripheral | USART3 (STM32F730)                        |
| TX pin     | PD8 (AF7)                                 |
| RX pin     | PC11 (AF7)                                |
| Baud rate  | 115,200                                   |
| Config     | 8N1 (8 data bits, no parity, 1 stop bit)  |
| Direction  | STM32 TX to Pi RX (unidirectional)        |

### Wire format

```
:%016llX\r\n
```

A colon prefix, followed by the 64-bit key bitmap as a zero-padded 16-character
uppercase hexadecimal string, terminated by `\r\n`. Example with only the Right
key pressed (bit 3):

```
:0000000000000008\r\n
```

Messages are sent only when the key state changes, not continuously. The Pi
receives on GPIO15 (UART RX, pin 10).


## Key Bitmap

The key state is encoded as a 64-bit unsigned integer. Each bit corresponds to
one key. A bit value of 1 means the key is pressed. Bits marked "undefined" are
unused and always 0. There are 53 valid keys.

| Bit | Key               | Bit | Key               |
|-----|-------------------|-----|-------------------|
|  0  | Left              | 27  | Pi (J)            |
|  1  | Up                | 28  | Sqrt (K)          |
|  2  | Down              | 29  | Square (L)        |
|  3  | Right             | 30  | Seven (M)         |
|  4  | OK                | 31  | Eight (N)         |
|  5  | Back              | 32  | Nine (O)          |
|  6  | Home              | 33  | LeftParen (P)     |
|  7  | OnOff             | 34  | RightParen (Q)    |
|  8  | *undefined*       | 35  | *undefined*       |
|  9  | *undefined*       | 36  | Four (R)          |
| 10  | *undefined*       | 37  | Five (S)          |
| 11  | *undefined*       | 38  | Six (T)           |
| 12  | Shift             | 39  | Multiply (U)      |
| 13  | Alpha             | 40  | Divide (V)        |
| 14  | XNT               | 41  | *undefined*       |
| 15  | Var               | 42  | One (W)           |
| 16  | Toolbox           | 43  | Two (X)           |
| 17  | Backspace         | 44  | Three (Y)         |
| 18  | Exp (A)           | 45  | Plus (Z)          |
| 19  | Ln (B)            | 46  | Minus (Space)     |
| 20  | Log (C)           | 47  | *undefined*       |
| 21  | Imaginary (D)     | 48  | Zero (?)          |
| 22  | Comma (E)         | 49  | Dot (,)           |
| 23  | Power^ (F)        | 50  | EE (Ctrl)         |
| 24  | Sin (G)           | 51  | Ans (Alt)         |
| 25  | Cos (H)           | 52  | EXE (Enter)       |
| 26  | Tan (I)           | 53-63 | *undefined*     |

The letter in parentheses is the character produced when the Alpha modifier is
active.


## MOSFET Power Control Circuit

The Raspberry Pi is powered through a P-channel MOSFET (NTR1P02LT1 or similar
SOT-23 package) so the STM32 can switch the Pi on and off.

| Component       | Connection                                |
|-----------------|-------------------------------------------|
| MOSFET source   | Battery positive (Vbat)                   |
| MOSFET drain    | Raspberry Pi 5V rail (pin 2 or pin 4)    |
| MOSFET gate     | STM32 PB9                                 |
| Pull-up resistor| 10 k-ohm between gate and Vbat            |

- **PB9 LOW** (output driven low): gate pulled below source, MOSFET conducts,
  Pi receives power.
- **PB9 HIGH or Hi-Z** (reset/default): gate pulled to Vbat by the 10 k-ohm
  resistor, V_GS ~ 0 V, MOSFET is off.

The MOSFET and pull-up are soldered onto the pads of the removed SD card slot
on the NumWorks PCB. This provides convenient solder points with traces that
lead to the battery rail.


## STM32 Pin Summary

| STM32 Pin | Function        | Direction    | Connects To              |
|-----------|-----------------|--------------|--------------------------|
| PA5       | SPI1_SCK        | Out to Pi    | GPIO11 (Pi pin 23, SCLK) |
| PA6       | Chip select     | Out to Pi    | GPIO8 (Pi pin 24, CE0)   |
| PA7       | SPI1_MOSI       | Out to Pi    | GPIO10 (Pi pin 19, MOSI) |
| PD8       | USART3_TX (AF7) | Out to Pi    | GPIO15 (Pi pin 10, RXD)  |
| PC11      | USART3_RX (AF7) | In from Pi   | GPIO14 (Pi pin 8, TXD)   |
| PB9       | GPIO output     | Out to MOSFET| MOSFET gate              |

All listed STM32 pins are 5V-tolerant. The Raspberry Pi GPIO operates at 3.3V.
No level shifting is required.
