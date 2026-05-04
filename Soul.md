# Aobus Soul: Brand Essence and Mathematical Aesthetics

<p align="center">
  <img src="Soul.svg" width="200" height="200" alt="Aobus Soul">
</p>

This document records the design philosophy, mathematical foundations, and dynamic art implementation logic of the Aobus brand mark (the Aobus Soul).

## 1. Brand Philosophy: Motion and Stillness (Anchor & Soul)

The Aobus logo consists of the characters 'a' and 'o'. Its core design philosophy lies in "Balance":

*   **The Anchor ('a')**: Represents the brand's foundation. It is absolutely static and stable, rendered in Amber (#F97316). Visually, it serves as the "steadying needle," ensuring the solidity of brand recognition.
*   **The Soul ('o')**: Represents the audio engine. It is dynamic and evolving, reflecting playback status and audio quality through changes in color and form.

## 2. Geometric Aesthetics: The Divine Proportion

Every detail of the Aobus Soul is calibrated by the Golden Ratio ($\phi \approx 1.618$):

### 2.1 Basic Geometry
*   **Stroke Ratio**: The standard stroke width is `9.0` with a circle radius of `30.0`.
*   **Gradient Distribution**: The ring uses an asymmetric gradient.
    *   **The Core (38.2%)**: Represents the current audio quality state (Purple/Green/etc.).
    *   **The Aura (61.8%)**: Represents the primary brand Cyan.
    *   **Logic**: The brand color occupies the dominant Golden position ($1/\phi$), ensuring that the Aobus identity remains sovereign even as colors shift.

### 2.2 Breathing Tension (Breathing Amplitude)
*   **Expansion Limit**: During breathing, the stroke width expands from the base `9.0` to a maximum of `14.56` (i.e., $9.0 \times 1.618$).
*   **Visual Impact**: This 61.8% expansion simulates natural lung breathing, creating a swelling sensation full of vitality.

## 3. Dynamic System: Quad-Golden Cycles

To ensure the animation never repeats perfectly and maintains an "organic" feel, we have established a system of mutually non-divisible Golden Ratio periods:

| Dimension | Period | Mathematical Logic | Physical Metaphor |
| :--- | :--- | :--- | :--- |
| **Breathing** | $5.119s$ | Base Period ($T_1$) | Inhalation and exhalation of energy |
| **Rotation** | $8.282s$ | $T_1 \times \phi$ | Continuous rotation of the audio stream |
| **Opacity** | $13.401s$ | $T_1 \times \phi^2$ | The rhythm of life and presence |
| **Aura Flow** | $21.683s$ | $T_1 \times \phi^3$ | Evolution of essence and hue shift |

### 3.1 Opacity Floor
The opacity oscillates between `0.618` and `1.0`. This ensures the Soul never fully disappears, maintaining a minimum presence at the Golden Ratio point.

### 3.2 Color Antagonism
During the **Aura Flow** cycle, the two ends of the gradient undergo opposite hue shifts ($\pm 10^\circ$):
*   When one end warms, the other cools.
*   This "tug-of-war" style color interaction simulates the flow of high-energy plasma.

## 4. Technical Implementation: High-Precision Engineering

*   **GPU Acceleration**: Implemented via GTK4 GSK (Graphene). All paths are constructed in memory via `GskPathBuilder`, avoiding unstable string parsing.
*   **Temporal Sampling**: Uses double-precision floating point (`double`) for `std::fmod` operations, eliminating "shakes" or jitter caused by precision loss over long running times.
*   **Cross-Platform Consistency**: The core logic is mirrored 100% via pure SVG SMIL filters, ensuring an identical visual experience across Web and Desktop.

---

**The Aobus Soul is more than a logo; it is a living entity woven from mathematics and music.**
