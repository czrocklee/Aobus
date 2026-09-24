# Frontend composition

Each frontend selects platform paths and an executor, constructs the appropriate runtime, and adapts shared state and commands to its platform.
The [system overview](../overview.md) defines allowed dependency directions; [session lifecycle](../session-lifecycle.md) owns stable runtime placement, restore, checkpoint, and teardown.
This page routes platform-specific behavior rather than duplicating runtime or UIModel policy.

| Frontend | Product and adapter contracts | Development and user tasks |
|---|---|---|
| GTK | [Active library](gtk/active-library-lifecycle.md), [dialogs](gtk/dialog-lifecycle.md), [track detail](gtk/track-detail.md), [Linux MPRIS](mpris.md), and [shared shell](../shell/README.md) | [GTK lifetime](../../development/gtk-lifetime.md), [GTK style](../../development/gtk-style.md), [get started](../../user/get-started.md) |
| WinUI | [Windows desktop](windows.md), [library workflows](windows-library-workflows.md), [layout schema](../../reference/windows/layout-schema.md), [desktop state](../../reference/windows/desktop-state.md) | [Windows development](../../development/windows.md), [use the desktop](../../user/use-windows-desktop.md) |
| AppKit | [Native shell composition](../shell/README.md#appkit-shell-owner), [shared desktop lifecycle](../desktop-library-lifecycle.md), and [session lifecycle](../session-lifecycle.md) | [macOS development](../../development/macos.md) |
| TUI | [Interaction](tui.md), [track authoring](tui-track-authoring.md), [Linux MPRIS](mpris.md), and [commands](../../reference/tui/command.md) | [Use the TUI](../../user/use-tui.md) |
| CLI | [Execution](cli.md) and [commands/output](../../reference/cli/command.md) | [Use the CLI](../../user/use-cli.md) |

## Native target and test boundaries

On Linux, `ao_system_media_linux` is an application platform adapter shared by GTK and TUI.
It owns GIO/D-Bus MPRIS mechanics over frontend-injected runtime, UIModel actions, executors, and host callbacks; it contains no GTK or terminal UI dependency.
Disabling the system-media build feature removes this target and both frontend integrations without changing Core, runtime, or UIModel.

WinUI owns Windows App SDK application/window lifetime, XAML resources, dispatcher adaptation, native pickers, SMTC, and its Modern and Classic shells.
Its Windows-only `aobus-winui-lib` owns compiled frontend implementation, including the shell's schema, dialect, element lattice, style resolution, responsive policy, and native adapters.
The thin `aobus-winui` executable owns the final link and deployed resources.
Windows-owned rules needing a native host run in the Windows suite; separable WinRT-free shell policy also compiles into `ao_core_test` on other hosts without becoming shared UIModel.

AppKit is an incremental native development slice over the same runtime.
Its `ao_appkit` object target supplies both the shipping bundle and an independent native GUI test bundle; scenario code is test-owned.
`LibrarySession` retains runtime and UIModel observers across window presentation changes and tears them down before a successor process opens another library.
AppKit uses native composition rather than the shared layout-document language.

`ao_desktop_launch` supplies pure root, startup, successor-protocol, and detached-launch mechanisms shared by GTK, WinUI, and AppKit.
It owns neither interactive runtime state nor toolkit event loops, checkpoint policy, failure presentation, or graph teardown.
TUI and CLI do not link it.

## Code and evidence

- [`app/CMakeLists.txt`](../../../app/CMakeLists.txt), [`app/platform/media/CMakeLists.txt`](../../../app/platform/media/CMakeLists.txt), and [`desktop/CMakeLists.txt`](../../../app/desktop/CMakeLists.txt) define frontend composition, shared Linux system media, and shared desktop support.
- [`windows-winui/CMakeLists.txt`](../../../app/windows-winui/CMakeLists.txt) defines the WinUI implementation and executable boundary.
- [`macos-appkit/CMakeLists.txt`](../../../app/macos-appkit/CMakeLists.txt) defines the AppKit bundle and native test composition.
- [`ArchitectureAudit.cmake`](../../../app/cmake/ArchitectureAudit.cmake) enforces the application-layer dependency and capability constraints.
- [Validation and native host selection](../../development/test/validation-and-review.md) and [test suites](../../development/test/test-suite.md) explain how to verify a change at these boundaries.
