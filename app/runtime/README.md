# Application runtime

`ao_app_runtime` composes Core mechanisms into frontend-neutral application behavior.
GTK, TUI, WinUI, AppKit, and CLI consume this layer directly or through UIModel; toolkit objects and rendering policy belong outside it.

## Work in this layer

- Use `CoreRuntime` for the non-interactive library and command graph; `AppRuntime` adds interactive workspace and playback services.
- Keep application truth here: library commands and publication, sources and projections, workspace/view state, playback, notifications, and managed application state.
- Let composition roots supply platform paths, executors, and platform facilities. Runtime derives per-library locations but does not discover desktop state directories or run a frontend event loop.
- Preserve stable placement and teardown ordering before adding a borrower or subscription. The [session lifecycle contract](../../doc/system/session-lifecycle.md) owns those requirements.

The [system overview](../../doc/system/overview.md) explains dependency direction; [subsystem topics](../../doc/system/README.md) own the detailed behavior and tests.
Use [application-layer review](../../doc/development/application-layer-review.md) for change review.

## Headers and consumers

Repository consumers link `ao_app_runtime`, whose public include directory is `app/include/` and whose API lives under `ao/rt/`.
Implementation-private headers remain under `app/`; focused tests may include those through their private test include path.
These are application interfaces, not an installed or independently versioned Core SDK.
Core headers remain under `include/ao/`; application dependencies must not leak back into Core.

[`CMakeLists.txt`](CMakeLists.txt) owns sources and target visibility; the [architecture audit](../cmake/ArchitectureAudit.cmake) checks layer boundaries across sources and tests.
