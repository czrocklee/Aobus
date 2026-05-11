# Aobus Soul

<p align="center">
  <img src="Soul.svg" width="200" height="200" alt="Aobus Soul">
</p>

The **Aobus Soul** is the brand system for Aobus: a two-part mark built from `a` and `o`. The `a` is the anchor: stable, structural, and unmistakable. The `o` is the soul: breathing, rotating, and able to reflect playback state.

## Brand Story

The name **Aobus** comes from *"Ao-bu"*, an early, deliberate syllable spoken by the creator's daughter. That origin gives the brand its emotional center: the effort to turn raw voice into expression. Aobus applies that same idea to audio, bridging precise digital structure and lived musical feeling.

## Core Visual Idea

- **Anchor (`a`)**: fixed, steady, and always amber.
- **Soul (`o`)**: circular, luminous, and state-aware.
- **Balance**: one half is motionless, one half is alive.
- **Behavior**: the mark is not just decorative; it can respond to playback quality and transport state.

## Color Tokens

| Token | Value | Use |
| :--- | :--- | :--- |
| Brand cyan | `#06B6D4` | Master SVG, documentation, and hero visuals |
| UI cyan | `#00E5FF` | Small GTK rendering where extra luminance improves clarity |
| Anchor amber | `#F97316` | Static `a` anchor |
| Hi-Res purple | `#A855F7` | Bit-perfect and padded lossless states |
| Lossless green | `#10B981` | Lossless float state |
| Intervention orange | `#F59E0B` | Linear intervention state |
| Warning red | `#EF4444` | Clipped or error state |
| Quiet gray | `#6B7280` | Stopped, lossy, or unavailable state |
| Night field | `#111827` | Dark icon tile and presentation background |

## Geometry And Motion

The system is tuned around the Golden Ratio (`phi ~= 1.618`) to keep the animation organic without becoming noisy.

- Base soul radius: `30.0`
- Base soul stroke: `9.0`
- Expanded soul stroke: `14.56`
- Opacity floor: `0.618`
- Breathing period: `5.119s`
- Rotation period: `8.282s`
- Opacity period: `13.401s`
- Aura flow period: `21.683s`
- Primary gradient direction: the state color begins at the lower-right of the soul ring and transitions toward cyan at the upper-left.

## Asset Set

| File | Role |
| :--- | :--- |
| `Soul.svg` | The animated, theme-adaptive master asset. Used as the primary hero visual. |
| `SoulMark.svg` | Static full-color mark on a transparent background. |
| `SoulSymbol.svg` | Static soul-only symbol for tiny or square placements. |
| `SoulMono.svg` | Single-color transparent mark for one-color applications. |

## Usage

- Use `Soul.svg` as the hero asset in GitHub READMEs and primary documentation to showcase the brand's living animation. Its design naturally adapts to both light and dark environments.
- Use `SoulMark.svg` when a static lockup is required (e.g., standard web headers, print drafts).
- Use `SoulSymbol.svg` below `24px` or whenever the slot is square and the full lockup feels cramped.
- Use `SoulMono.svg` for engraving, embossing, monochrome print, or single-ink assets.
- Keep the anchor amber. Do not recolor the `a` independently.
- Avoid extra shadows, outer glows, outlines, or alternate gradients.

## Technical Note

`Soul.svg` is the visual source of truth. Inside the GTK application, the same geometry and timing model are reconstructed natively in `app/linux-gtk/ui/AobusSoul.cpp` so the brand mark can stay sharp, lightweight, and responsive at runtime.

---

**Copyright (c) 2026 YANG LI. All Rights Reserved.**
The "Aobus Soul" design and its mathematical implementation are proprietary brand assets. Usage in third-party projects requires explicit written permission.
