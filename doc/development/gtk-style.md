---
id: development.gtk-style
type: development
status: current
domain: development
summary: Defines GTK theme-token, structural-variant, shared-component, transition, and visual-complexity contributor rules.
---
# GTK style

## Scope

This guide owns contributor policy for GTK CSS and theme adaptation.
It does not define semantic UI behavior or shell structure; those belong to presentation and application-shell documents.

## Policy

Classic is the default dense theme and establishes semantic tokens in `css/_variables.css`.
Modern overrides visual tokens under the `ao-theme-modern` root class.

Use a theme token when widget structure is shared and only color, radius, spacing, shadow, opacity, typography, or motion differs.
Use a classic/modern variant class only when the built widget/layout structure is genuinely different.
Reusable structural selectors belong in `css/_common.css` and consume semantic tokens rather than repeating theme-specific values.

Keep responsibilities distinct:

- layout documents choose structure, expansion, alignment, and semantic CSS classes;
- C++ owns intrinsic measurement, allocation behavior, and theme-class application;
- CSS owns visual styling and hit-target appearance.

Name transitions by property instead of adding new `transition: all` declarations.
The current stylesheets still contain legacy `transition: all` sites; treat them as cleanup debt, not precedent for new rules.
Apply alpha compositing and heavy shadows to large surfaces such as shells, bars, floating controls, and dialogs, not every virtualized list row.

Compact artwork in a control bar cannot set the bar's natural height from its source image.
Use the shell image component's square target and parent allocation policy so the surrounding transport row remains the height authority.

## Workflow

1. Determine whether the change is structural or purely visual.
2. Add/reuse a semantic token for a shared structure.
3. Add a variant selector only when the GTK tree differs.
4. Verify both Classic and Modern roots and all affected top-level windows/dialogs.
5. Add a GTK style/theme test when class application or runtime theme switching changes.

User CSS is an override layer and must not become the source of built-in product invariants.

## Validation

Run the focused `GtkStyleRuntime`, `ThemeCoordinator`, or component test for the changed owner.
Then use the repository's normal validation gate.
Do not rely on a screenshot alone when a class/token mapping can be asserted directly.

## Troubleshooting

If Modern requires many component-specific copies, first check whether the missing concept is a semantic token.
If a CSS value changes widget minimum size unexpectedly, move structural intent back to layout/C++ allocation policy.
If a transient window misses theme classes, register its top-level lifetime with `ThemeCoordinator` and retain the registration token until the window closes.

## Related documents

- [GTK lifetime and wiring](gtk-lifetime.md)
- [Presentation architecture](../architecture/presentation.md)
- [Application shell architecture](../architecture/application-shell.md)
- [Shell layout adaptation](../spec/shell/layout-adaptation.md)
- [GTK dialog lifecycle](../spec/linux-gtk/dialog-lifecycle.md)
