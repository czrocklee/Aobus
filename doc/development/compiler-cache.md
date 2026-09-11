---
id: development.compiler-cache
type: development
status: current
domain: development
summary: Defines compiler-cache setup, host-local state, overrides, and CI integration.
---
# Compiler cache

Project-managed compiler caching is an explicit host setup action:

```bash
./ao setup compiler-cache
```

Use `ao.bat setup compiler-cache` on Windows.
Setup resolves a ccache version governed by `script/ao/compiler-cache.json`.
Linux obtains the exact pinned version from the Nix shell, macOS installs the current compatible Homebrew formula at or above the governed minimum, and Windows downloads and verifies the exact governed official archive.
A failed version or checksum check does not enable the cache.
Ordinary portal commands never download compiler-cache tooling.
If a saved configuration becomes invalid or its recorded executable moves or changes, the portal warns and names the setup command needed to enable caching again.
Run setup again after a Nixpkgs or Homebrew upgrade replaces ccache, or after deleting the Aobus cache root, which also removes its activation configuration.
An absent configuration stays silent; configure output lists the selected launchers and their managed status.
Windows setup reuses a previously downloaded archive only after verifying its checksum against the current contract.
Local activation checks the saved executable's content digest and enforces the exact Linux or Windows version pin; macOS accepts a release at or above the governed minimum.
Windows setup provisions the managed MSBuild wrapper before publishing its configuration.
It also provisions that wrapper when the setup invocation selects a custom `AOBUS_MSBUILD_CL_TOOL_EXE`, so later shells can activate the managed default; the custom wrapper is left untouched.
Ordinary activation only validates the wrapper's content and never creates, replaces, or changes permissions on it; a missing or changed wrapper requires setup again.

After setup, portal commands that configure or build native code apply the managed launchers automatically.
Reconfiguring an existing Ninja build tree updates its managed launcher without requiring a clean build.
Trees using the former automatic `USE_CCACHE` discovery also adopt the managed launcher after setup; the old `CCACHE_PROGRAM` discovery entry does not prevent migration.
An explicitly configured launcher in the CMake cache remains authoritative when it has no Aobus managed marker, even if it is named `ccache` or `sccache` or its executable is missing.
Update an obsolete explicit override where it was configured; setup does not infer ownership from a filename or take over a broken override.
The Windows Visual Studio tree uses the same verified executable copied to a host-local `cl.exe` wrapper; the wrapper directory is not added to `PATH`, so MSVC discovery remains intact.

## State and capacity

Each native user has one shared Aobus compiler-cache store:

| Host | Default cache directory |
|---|---|
| Linux | `${XDG_CACHE_HOME:-$HOME/.cache}/Aobus/ccache` |
| macOS | `$HOME/Library/Caches/Aobus/ccache` |
| Windows | `%LOCALAPPDATA%\Aobus\cache\ccache` |

`AOBUS_STATE_ROOT` moves managed configuration, tools, and the default cache store together.
`CCACHE_DIR` overrides only the cache store.
Existing legacy per-checkout Linux `.cache/ccache` directories are left in place and are not migrated automatically.

The default capacity is 20 GB (`20G`) across the shared host store.
Setup compares decimal and binary suffixes using ccache's units and retains a larger capacity already recorded in the Aobus config or in that ccache store.
The ccache value `0` remains unlimited.
An explicit `CCACHE_MAXSIZE` remains authoritative for the current invocation.
The project also preserves explicit `CMAKE_C_COMPILER_LAUNCHER`, `CMAKE_CXX_COMPILER_LAUNCHER`, and `AOBUS_MSBUILD_CL_TOOL_EXE` settings.
An explicitly empty C or C++ launcher disables that launcher, both in the environment and in an unmanaged CMake cache entry.
An empty environment override clears an existing managed launcher and its ownership marker; the resulting empty cache entry stays authoritative on later invocations.
To restore managed caching, unset the empty environment override and remove the launcher's CMake cache entry; an explicit nonempty launcher can also replace it.
An existing shell session may still export the former per-checkout `CCACHE_DIR` and `10G` `CCACHE_MAXSIZE` defaults.
Start a fresh shell without those exports or unset the variables before setup to adopt the managed defaults; the portal cannot distinguish inherited legacy values from intentional overrides.

The shared directory gives every workspace access to the same cache entries; reuse still depends on ccache's correctness keys.
Linux optimized builds can reuse compatible entries across equivalent workspaces.
Debug information may retain workspace paths, and MSVC includes the object directory in its hash, so some cross-workspace compilations remain misses.
Aobus does not weaken `hash_dir`, add sloppiness settings, or rewrite debug paths to manufacture hits.

The setup command prints the resolved executable, cache directory, and effective capacity.
It also identifies inherited `CCACHE_DIR` and `CCACHE_MAXSIZE` overrides so their effect on the managed defaults is visible.
Pass the printed directory and capacity back to ccache when detailed local statistics are needed:

```bash
CCACHE_MAXSIZE='<printed-capacity>' <printed-executable> --dir <printed-cache-directory> --show-stats
```

```powershell
$env:CCACHE_MAXSIZE = '<printed-capacity>'; & '<printed-executable>' --dir '<printed-cache-directory>' --show-stats
```

## Continuous integration

GitHub Actions uses sccache from the pinned Mozilla action rather than the local ccache provider.
The workflow reads the sccache version and 20 GB (`20G`) size policy from the same project contract, then uses the portal activation module to bind Ninja launchers and the Windows MSBuild wrapper.
CI places the Windows sccache wrapper under the runner's temporary directory, outside the long-lived managed-tools cache.
`SCCACHE_CACHE_SIZE=20G` governs a local disk backend when one is active and does not enlarge the GitHub Actions remote cache quota or retention.
The action continues to own its GitHub cache endpoint, token, namespace, and transient-error policy.
