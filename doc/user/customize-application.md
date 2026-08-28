---
id: user.customize-application
type: user-guide
status: current
domain: application-shell
summary: Changes the GTK theme, layout preset, panel state, output preference, and keyboard bindings.
---
# Customize the GTK application

## Outcome

The application uses your selected appearance, shell layout, output device, and keyboard bindings, with panel state saved at the intended level.

## Steps

1. Open **Edit → Preferences...** or press Ctrl+Comma.
2. On **Appearance**, choose the Classic or Modern theme.
3. On **Playback/Output**, choose the output device used by playback.
4. On **Layout**, choose the Classic or Modern default preset.
5. On **Keyboard**, add or remove bindings for the listed actions.
   Resolve any reported conflicts before relying on the new chord.
6. To change shell structure, choose **View → Edit Layout...** or use **Edit Layout...** on the Layout preference page.
   Apply changes to preview them, save to make the authored layout durable, or cancel to restore the pre-editor runtime state.
7. To change a missing-cover design, select the corresponding `track.table`, `track.coverArt`, or `playback.image` component in the Layout Editor and choose `monogram`, `note`, `vinyl`, `equalizer`, or `soul` for its placeholder property.
   Group headings, track detail, and Now Playing are independent choices; this is layout authoring and has no separate preference page.
   Every style draws only its foreground symbol or text over the surrounding surface; the placeholder has no tile background.
8. Resize collapsible panels directly in the workspace.
   Choose **View → Save Current Panel Sizes as Layout Defaults** only when those sizes should become authored defaults.
9. Choose **View → Reset Runtime Layout State** to discard remembered component state and return to the current authored defaults.

Panel state and authored layout are different authorities: ordinary resizing changes versioned runtime component state, while saving defaults promotes current sizes into the layout document.

## Verify the result

- Theme and layout changes appear in the active window immediately.
- The selected output device appears in the playback/output surface.
- A changed shortcut invokes the intended action and no conflict remains in the Keyboard page.
- Missing covers use the selected style in each edited location; existing cover art still replaces the placeholder.
- Restarting Aobus restores saved preferences and state; canceled layout-editor changes do not reappear.

## Related documents

- [Keymap reference](../reference/shell/keymap.md)
- [GTK layout schema reference](../reference/shell/layout-schema.md)
- [Layout state reference](../reference/shell/layout-state.md)
- [Shell layout lifecycle](../spec/shell/layout-lifecycle.md)
- [Shell layout adaptation](../spec/shell/layout-adaptation.md)
