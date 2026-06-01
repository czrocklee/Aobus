# Aobus Aesthetic Design Language (ADL)

This document formalizes the CSS architecture and multi-theme design patterns for the Aobus GTK application. It establishes the rules for how styling should be applied across different layout presets (e.g., Classic vs Modern) without creating parallel CSS systems.

## The Core Paradigm: Variant Class vs. Theme Token

To prevent the CSS codebase from expanding indefinitely, Aobus strictly adheres to the following separation of concerns:

- **Variant Class (`xxx-modern`, `xxx-classic`)**: ONLY used when the **layout structure (DOM/XML)** is fundamentally different.
- **Theme Token (`var(--ao-...)`)**: Used when only the **visual language** (radius, spacing, shadow, color, surface transparency) differs, while the underlying layout structure remains identical.

### 1. Classic Theme (Studio Grade) = Default Environment
The `Classic` layout represents the default, "Studio Grade" design language:
- It prioritizes high information density, function over form, and standard GTK native integration.
- Its styles are defined in `:root` inside `css/_variables.css`.
- Core primitives (e.g., `4px` radius, solid borders, compact spacing) act as the baseline fallback for the entire application.

### 2. Modern Theme (Premium Emotion) = Token Override
The `Modern` layout represents the "Premium Emotion" design language:
- It emphasizes negative space, large typography, glassmorphism, rounded corners, and micro-animations.
- Instead of creating `.ao-[component]-modern` classes for every widget, the Modern theme simply **overrides semantic tokens** under the `.ao-layout-modern` scope selector.

```css
/* _variables.css - Default/Classic Base */
:root {
  --ao-surface-bg: @theme_base_color;
  --ao-surface-border-color: alpha(currentColor, 0.10);
  --ao-surface-shadow: none;
  --ao-card-radius: 4px;
}

/* _modern.css - Token Override */
.ao-layout-modern {
  --ao-surface-bg: alpha(@theme_base_color, 0.50);
  --ao-surface-border-color: transparent;
  --ao-surface-shadow: 0 8px 24px alpha(black, 0.08);
  --ao-card-radius: 16px;
}
```

### 3. Common Base Components
All reusable structural components (e.g., `.ao-track-section-box`, `.ao-boxed-list`, `.ao-list-row`) MUST be placed in `css/_common.css`. 
These components must never use hardcoded padding, border, or radius values. They must consume the semantic tokens defined in `:root`. By doing so, they automatically adapt to the `Modern` theme when the layout engine injects the `.ao-layout-modern` class at the window root.

### 4. Structurally Distinct Variants
Only when a component's XML structure in `modern_layout.yaml` differs significantly from `default_layout.yaml` should a dedicated variant class be used.
- Example: The Modern layout uses a completely different bottom bar structure. Therefore, it uses `.ao-bottom-bar-wrapper-modern`.
- Example: The Classic layout uses a simple strip for playback controls. Therefore, it uses `.ao-playback-strip-classic`.

## CSS Transition Rules
- **Do not use `transition: all`**. It makes debugging difficult by inadvertently animating layout dimensions, colors, shadows, and transforms simultaneously.
- Always specify target properties: `transition: background-color var(--ao-transition-base), transform var(--ao-transition-base);`.

## Glassmorphism Guidelines
- Use heavy shadows and alpha transparency sparingly.
- Apply them only to large container surfaces (Shell, Bottom Bar, Floating Controls, Dialogs).
- **Do not** apply heavy shadows or complex alpha blending to individual list rows (`columnview row`), as this introduces severe visual noise and high rendering overhead in GTK.
