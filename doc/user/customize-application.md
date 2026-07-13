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
7. Resize collapsible panels directly in the workspace.
   Choose **View → Save Current Panel Sizes as Layout Defaults** only when those sizes should become authored defaults.
8. Choose **View → Reset Runtime Layout State** to discard remembered component state and return to the current authored defaults.

Panel state and authored layout are different authorities: ordinary resizing changes versioned runtime component state, while saving defaults promotes current sizes into the layout document.

## Verify the result

- Theme and layout changes appear in the active window immediately.
- The selected output device appears in the playback/output surface.
- A changed shortcut invokes the intended action and no conflict remains in the Keyboard page.
- Restarting Aobus restores saved preferences and state; canceled layout-editor changes do not reappear.

## Related documents

- [Keymap reference](../reference/shell/keymap.md)
- [Layout catalog reference](../reference/shell/layout-catalog.md)
- [Layout state reference](../reference/shell/layout-state.md)
- [Shell layout lifecycle](../spec/shell/layout-lifecycle.md)
- [Shell layout adaptation](../spec/shell/layout-adaptation.md)
