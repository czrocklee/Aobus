# app/runtime

Shared application runtime for the Aobus desktop and CLI frontends.

## Purpose

This layer sits between the core `lib/` (public API in `include/ao/`) and the
two official frontends (`app/cli/` and `app/linux-gtk/`). It provides:

- Event bus and state management
- Services (playback, library mutation, workspace, view, notification)
- Track sources and projections
- Configuration persistence

## Visibility

**These headers are NOT part of the public library API.** They are internal to
the Aobus repository. Headers are not installed or exposed via `include/ao/`.
Tests include them via the private `${CMAKE_SOURCE_DIR}/app` include path.

Third-party frontends should build against `include/ao/` and `lib/` only.
