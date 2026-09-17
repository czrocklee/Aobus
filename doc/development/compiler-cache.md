---
id: development.compiler-cache
---
# Compiler cache

Compiler caching is optional host setup. The portal owns activation; ordinary
commands do not discover or download a cache tool implicitly.

## Set up or refresh the cache

Run from the repository root:

```bash
./ao setup compiler-cache
```

On Windows use:

```bat
ao.bat setup compiler-cache
```

The command prints the selected executable, cache directory, capacity, and
whether cross-workspace reuse is active in the current shell. Run it again when
the portal warns that the saved executable or Windows wrapper is missing or has
changed, after a Nixpkgs or Homebrew ccache upgrade, or after deleting the Aobus
state/cache root.

Versions and Windows archive identity come from
`script/ao/compiler-cache.json`. Linux uses the exact ccache in the pinned Nix
shell. macOS installs the governed Homebrew formula and accepts a compatible
version at or above the governed minimum. Windows downloads and verifies the
exact official archive. Setup fails closed on a version or checksum mismatch.

After setup, configure and build commands apply managed C and C++ launchers.
Existing Ninja trees are reconfigured when their managed launcher changes. The
WinUI Visual Studio tree uses a host-local verified `cl.exe` wrapper without
putting its directory on `PATH`.

## State, capacity, and overrides

The default cache store is shared by the current native user:

| Host | Default cache directory |
|---|---|
| Linux | `${XDG_CACHE_HOME:-$HOME/.cache}/Aobus/ccache` |
| macOS | `$HOME/Library/Caches/Aobus/ccache` |
| Windows | `%LOCALAPPDATA%\Aobus\cache\ccache` |

`AOBUS_STATE_ROOT` moves managed configuration, tools, and the default store.
`CCACHE_DIR` changes only the store. The default capacity is `20G`; setup keeps
a larger valid capacity already saved in Aobus state or in that store, and
ccache's `0` remains unlimited. `CCACHE_MAXSIZE` overrides capacity for the
current invocation.

Explicit `CMAKE_C_COMPILER_LAUNCHER`,
`CMAKE_CXX_COMPILER_LAUNCHER`, and `AOBUS_MSBUILD_CL_TOOL_EXE` values remain
authoritative. An explicitly empty CMake launcher disables that language's
launcher. An empty environment override clears an existing managed launcher
and its ownership marker; the resulting empty CMake cache entry remains an
explicit override after the environment variable is unset. To restore managed
caching, unset the empty environment override and remove that launcher's CMake
cache entry; an explicit nonempty launcher can replace it instead. Setup does
not take ownership of an unmanaged launcher merely because its file is named
`ccache` or `sccache`.

Old shells may still export the former checkout-local `CCACHE_DIR` and `10G`
capacity. The setup output identifies inherited overrides. Unset them or start
a fresh shell to adopt the managed defaults.

For detailed local statistics, use the executable, directory, and capacity that
setup printed:

```bash
CCACHE_MAXSIZE='<printed-capacity>' <printed-executable> --dir <printed-cache-directory> --show-stats
```

```powershell
$env:CCACHE_MAXSIZE = '<printed-capacity>'; & '<printed-executable>' --dir '<printed-cache-directory>' --show-stats
```

## Optional cross-workspace reuse

Ordinary setup preserves the saved preference and defaults it to disabled for a
new configuration. Change it explicitly with:

```bash
./ao setup compiler-cache --shared-workspaces
./ao setup compiler-cache --no-shared-workspaces
```

This profile is for equivalent checkouts whose physical source and build paths
differ. It normalizes compiler-visible paths without weakening ccache's source
or argument keys. It does not make different target graphs, compilers, build
basenames, or generated layouts cache-equivalent.

### Linux and macOS

For supported GNU, Clang, and AppleClang Ninja builds, the portal creates an
immutable `source` symlink inside the locked build tree and records ownership
and debugger mappings in `aobus-workspace-cache.json`. It never replaces a
non-link, retargets an existing link, or adopts a tree owned by another
checkout. Source and build trees must be disjoint.

The compiler reports stable source names such as
`source/lib/audio/Decoder.cpp`; debug paths are rooted at `/aobus/build`. Read
`debuggerSourceMap` in the metadata file and apply it with the debugger:

```text
GDB:  set substitute-path <from> <to>
LLDB: settings set target.source-map <from> <to>
```

The portal does not edit debugger or editor configuration. Keep the metadata
with the tree: it remains useful for objects compiled while normalization was
enabled.

Coverage and sanitizer builds are deliberately excluded. Disable the saved
profile for one invocation rather than changing it globally:

```bash
AOBUS_SHARED_WORKSPACES=0 ./ao coverage
AOBUS_SHARED_WORKSPACES=0 ./ao check --asan
AOBUS_SHARED_WORKSPACES=0 ./ao check --tsan
```

`AOBUS_SHARED_WORKSPACES` is only an off switch; it cannot enable an unsaved
profile.

### Windows SSH logons

On Windows, cross-workspace reuse is active only in an isolated SSH logon. Each
portal invocation reserves `S:` for the source checkout and `B:` for the
physical build root in that logon's local DOS-device namespace. Source remains
in place, including on SMB; the portal does not copy it.

Use independent SSH authentication sessions for concurrent workspaces and
disable SSH connection multiplexing when the client supports it. The portal
refuses occupied mappings, LocalSystem, remote build storage, overlapping
source/build roots, and ambient `CL` or `_CL_` options. It never changes a
global drive mapping.

Normal completion retires the mappings after child processes return. If a build
is interrupted, an application cannot be reaped, or the portal terminates
abruptly, end that SSH logon before retrying; do not reuse its `S:` or `B:`
views. Fixed-view trees must always be built and consumed through the portal.
Coverage and sanitizer builds remain excluded.

Outside SSH, an enabled preference falls back to the ordinary per-workspace
profile and setup reports that fact. Automatic fixed-view builds use a separate
`shared-workspaces-v1` root. With `-p` or `BUILD_DIR`, keep the selected build
parent disjoint from the source tree; a composite Windows `check` still derives
its WinUI sibling from that primary tree.

Each fixed-view tree contains `aobus-windows-workspace-cache.json`. Its
`debuggerSourceMap` maps compiler source paths rooted at `S:/` back to the
physical checkout; apply that mapping when debugging outside the owning SSH
logon. Keep the metadata with the tree because it also records the ownership
needed to reject adoption from another source or output location.

WinUI keeps its precompiled header under this profile to avoid reparsing the
large framework include graph. Because the profile uses empty ccache
`sloppiness`, ccache declines PCH compilations and MSVC compiles them normally;
shared-workspace mode therefore does not make every WinUI translation unit
cacheable.

## Windows compiler file tracking

The portal enables normal compiler file tracking only for its verified managed
ccache wrapper when cache-owned writable directories are local, excluded from
native tracking, and disjoint from source and build trees. Custom wrappers,
sccache, and unverified layouts retain the legacy tracking behavior and produce
a notice. This prevents MSBuild `Clean` from treating cache files as project
outputs. The decision is per WinUI build and is not a persistent CMake override.

## Continuous integration

GitHub Actions uses the sccache action and version pinned by
`script/ao/compiler-cache.json`; local setup remains ccache-only. The workflow
uses the same `20G` local-size policy, but that value does not change GitHub's
remote cache quota or retention. CI endpoint, credentials, namespace, and
transient-error behavior remain owned by the action and workflow.
