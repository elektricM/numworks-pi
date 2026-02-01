# Battery Upgrade Guide

## Original Battery

| Spec       | Value                          |
|------------|--------------------------------|
| Model      | JFT 315572SE-C                 |
| Voltage    | 3.8V nominal                   |
| Capacity   | 1820mAh                        |
| Dimensions | 3.1mm x 55mm x 72mm (T x W x L) |

## Constraint

Maximum battery thickness: **6mm**. Beyond this the N0100 back cover will not close without case modification.

## LiPo Naming Convention

LiPo cells follow the pattern **TTWWLL**:

- **TT** = thickness in 0.1mm increments
- **WW** = width in mm
- **LL** = length in mm

Example: **606090** = 6.0mm thick, 60mm wide, 90mm long.

## Recommended Upgrade: 606090

| Spec       | Value                            |
|------------|----------------------------------|
| Size code  | 606090                           |
| Voltage    | 3.7V nominal                     |
| Capacity   | 4000mAh                          |
| Dimensions | 6mm x 60mm x 90mm (92mm with protection circuit) |
| Capacity increase | 2.76x over original       |

This is the largest cell that fits the 6mm thickness constraint while remaining within the case footprint.

## Alternative Batteries

| Size Code | Approx. Capacity | Notes                        |
|-----------|-------------------|------------------------------|
| 604060    | ~1800mAh          | Drop-in size, no gain        |
| 605080    | ~3000mAh          | Good middle ground           |
| 606090    | 4000mAh           | Recommended, best capacity   |
| 6060100   | ~4500mAh          | May be too long for the case |

## Runtime Estimates (606090, 4000mAh)

| Mode       | Estimated Runtime |
|------------|-------------------|
| Idle       | ~30 hours         |
| Light load | ~15 hours         |
| Heavy load | ~8-9 hours        |

## Power Notes

The Pi Zero 2 W is powered **directly from the battery** through a boost converter to the 5V pin:

- LiPo operating range: 3.0V-4.2V
- WiFi requires at least 3.0V to function reliably
- Do **NOT** tap power from the N0100's internal 2.8V regulator -- it cannot supply enough current for the Pi

## Sourcing

- **Amazon** -- fastest shipping, higher price
- **AliExpress** -- cheapest, 2-4 week shipping
- **RobotShop** -- good selection of hobbyist LiPo cells

When ordering, confirm the cell includes a protection circuit (overcharge/overdischarge/short). Bare cells without protection are not recommended.
