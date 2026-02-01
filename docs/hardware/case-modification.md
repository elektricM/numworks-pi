# Case Modification Notes

## Dimensions Reference

| Component          | Dimensions                |
|--------------------|---------------------------|
| N0100 exterior     | 160mm x 82mm x 10mm      |
| Internal height    | ~6-7mm after PCB          |
| Pi Zero 2 W       | 65mm x 30mm x ~5mm       |
| Original battery   | 3.1mm thick               |

## Internal Layout

**Side-by-side arrangement**: the battery and Pi Zero 2 W sit adjacent horizontally inside the back cavity. The Pi is placed on the HDMI connector side to keep the SD card slot accessible (or at least not blocked by the battery).

With a 6mm battery and a 5mm Pi, both components exceed the ~6-7mm internal clearance. A modified back cover is required for a fully enclosed build.

> Zardam's note: "too thick to replace the original cover" -- case modification is needed.

## Modified Back Cover

Options:

1. **3D-printed cover with bulge** -- add a raised section over the Pi area to provide clearance.
2. **Cutout with window** -- remove material from the back cover above the Pi and optionally cover with a thin panel.
3. **Extended back** -- print an entirely new back cover that is 2-3mm thicker overall.

## 3D Model Source

NumWorks publishes CAD files for the calculator cases:

- Repository: <https://github.com/numworks/dieter>
- Contains STL files for both N0100 and N0110 models
- Use these as the starting point for modified back cover designs

## Opening the Calculator

1. Peel off the **4 rubber feet** on the back to reveal T5 Torx screws underneath.
2. Remove all **6 T5 Torx screws** (4 under feet + 2 exposed).
3. Carefully separate the back cover from the front shell -- start from one corner with a plastic spudger.
4. **Disconnect the battery** cable before doing any work.

## Pi Mounting

- Use **double-sided foam tape** (1mm thick) on the HDMI connector side of the Pi.
- The foam provides cushioning and electrical insulation against the back cover.
- Do not use hard adhesive -- you need to be able to remove the Pi for maintenance.

## Wire Routing

- Route wires along the edges of the case, away from the center.
- **Avoid interference with the keyboard flex cable** -- it runs from the front PCB through the middle of the case.
- Use small drops of hot glue or kapton tape to secure wires in place.
