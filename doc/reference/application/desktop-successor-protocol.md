---
id: reference.application.desktop-successor-protocol
---
# Desktop successor protocol

## Scope and version

This reference is the exhaustive command-line surface carried from an Aobus
GTK, WinUI, or AppKit parent to its successor process. It describes arguments after
`argv[0]`.

The protocol is private to one installed build and has no external compatibility
version. The owning behavior is the
[desktop library lifecycle specification](../../system/desktop-library-lifecycle.md).

## Code boundary

`ao_desktop_launch` owns parsing and encoding through
[`LibrarySuccessorProtocol.h`](../../../app/include/ao/desktop/LibrarySuccessorProtocol.h).
The [system architecture](../../system/overview.md) and
[interactive session lifecycle architecture](../../system/session-lifecycle.md)
own its dependency and composition placement.

## Surface

The canonical encoded form is:

```text
--aobus-successor --library-root <absolute-utf8-path> [--scan-after-open]
```

| Argument | Cardinality | Value | Meaning |
|---|---:|---|---|
| `--aobus-successor` | exactly one in successor mode | none | Marks the invocation as a private parent-created successor. |
| `--library-root` | exactly one in successor mode | next argument | Supplies the target library root. |
| `--library-root=<value>` | alternative spelling | suffix after `=` | Supplies the same target root without a separate argument. |
| `--scan-after-open` | zero or one | none | Requests a scan after the successor activates. |

The marker and either root spelling form one required pair. Their order, and
the optional scan argument's order, are not significant. The encoder always
emits the canonical split root form in the order shown above.

The shared parser consumes only these arguments and preserves every other
argument in original order. Consumers handle that remainder separately:

- GTK passes it to its Aobus/GTK option partition.
- WinUI accepts no remainder and reports its first unknown argument.
- AppKit accepts `--state-root <path>` to select its native application-state root. Its parent appends that option to preserve the same state root in the successor. AppKit also accepts `--library <path>` only when no request has already been selected; it cannot override or accompany a parsed successor request. Other or incomplete native options fail startup.

AppKit's native options are not shared protocol tokens. Their paths are resolved to absolute paths by the AppKit entry point; the shared `--library-root` still requires an absolute value at parsing.

## Validation rules

- The marker, root, and scan arguments may not be duplicated.
- Marker without root and root without marker are invalid.
- Scan without the marker/root pair is invalid.
- A split root requires a non-empty following argument that does not begin with
  `-`. An equals-form root requires a non-empty suffix.
- The root value must be valid UTF-8 for the host path conversion and must be
  absolute. Parsing makes it absolute/lexically normalized but does not require
  it to exist; startup planning performs the directory check.
- Encoding requires an absolute non-empty path, emits its normalized UTF-8
  native spelling, and does not emit `argv[0]`.
- The standard GTK `--gapplication-replace` argument is not part of this
  protocol and remains in the parser remainder.

## Compatibility and versioning

The surface coordinates two processes from the same Aobus installation. It is
not a user command-line API, serialized state, IPC compatibility boundary, or
multi-version handshake. Changes must update all three desktop consumers, this
reference, and parser/encoder round-trip tests atomically.

## Examples

Canonical GTK/Linux request:

```text
--aobus-successor --library-root /home/listener/Music --scan-after-open
```

AppKit request preserving its native state root (shown as shell-quoted arguments):

```text
--aobus-successor --library-root /Users/listener/Music --state-root '/Users/listener/Library/Application Support/Aobus/macos'
```

Canonical WinUI request before native UTF-16 command-line escaping:

```text
--aobus-successor --library-root C:\Users\Listener\Music
```

Accepted equals spelling with an unrelated GTK argument preserved:

```text
--display=:1 --library-root=/srv/music --aobus-successor
```

## Implementation authority

- [`LibrarySuccessorProtocol.cpp`](../../../app/desktop/LibrarySuccessorProtocol.cpp)
  is the parser and encoder authority.
- [`GtkStartupPlan.cpp`](../../../app/linux-gtk/app/GtkStartupPlan.cpp) consumes
  the shared parse while preserving GTK arguments.
- [`ProcessLauncher.cpp`](../../../app/windows-winui/platform/ProcessLauncher.cpp)
  converts the native Win32 command line to UTF-8 and rejects parser remainder.
- AppKit [`DesktopMain.mm`](../../../app/macos-appkit/DesktopMain.mm) parses the shared request and native remainder; [`DesktopApplication.mm`](../../../app/macos-appkit/DesktopApplication.mm) encodes the request and appends the state root after releasing the session and application-state lease.

## Test authority

- [`LibrarySuccessorProtocolTest.cpp`](../../../test/unit/desktop/LibrarySuccessorProtocolTest.cpp)
  protects round trip, preservation, pairing, duplicate, relative-root, and
  scan-admission rules on every host.
- [`GtkStartupPlanTest.cpp`](../../../test/unit/linux-gtk/app/GtkStartupPlanTest.cpp)
  protects GTK-specific remainder partitioning.
- [`DetachedProcessLauncherWindowsTest.cpp`](../../../test/unit/desktop/DetachedProcessLauncherWindowsTest.cpp)
  protects exact UTF-8-to-Windows argv transport for spaces, quotes, trailing
  backslashes, and Unicode.
- [`AppKitDesktopScenario.mm`](../../../test/integration/macos/AppKitDesktopScenario.mm) exercises successor reentry through production startup and launcher invocation under the native supervisor; it is not an exhaustive malformed-native-option test.

## Related documents

- [Desktop library lifecycle specification](../../system/desktop-library-lifecycle.md)
- [GTK active-library lifecycle specification](../../system/frontend/gtk/active-library-lifecycle.md)
- [Windows desktop shell specification](../../system/frontend/windows.md)
