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

The ordinary setup above preserves the previously saved shared-workspace preference and defaults it to disabled for a new configuration.
Enable or disable that preference explicitly with:

```bash
./ao setup compiler-cache --shared-workspaces
./ao setup compiler-cache --no-shared-workspaces
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

## Shared workspaces on POSIX hosts

The optional shared-workspace profile lets equivalent Linux and macOS Ninja builds reuse ccache entries when their checkout and build directories have unrelated physical layouts.
The portal creates an immutable `source` symbolic link inside each locked build tree and configures CMake through that link.
It never replaces a non-link, retargets a link, or adopts a tree owned by another checkout.
Source and build trees must be disjoint.
The portal resolves symbolic links in a POSIX build path and uses that physical directory consistently for CMake and the ownership record.

The profile applies only to GNU, Clang, or AppleClang C and C++ compilation in the current native target graph.
It uses these ordered mappings, where `<build>` is the selected physical build directory:

```text
-fdebug-prefix-map=<build>=/aobus/build
-ffile-prefix-map=<build>/=
```

Consequently, `__FILE__` and `std::source_location` report stable names such as `source/lib/audio/Decoder.cpp`; generated files under the build tree also have relative names.
Debug information uses `/aobus/build`.
Each configured tree contains `aobus-workspace-cache.json`, which records the original checkout, the immutable alias, and the debugger mapping back to the selected build tree.
Use its `debuggerSourceMap` values as follows:

```text
GDB:  set substitute-path <from> <to>
LLDB: settings set target.source-map <from> <to>
```

The `<to>/source` link then resolves source records to the original checkout.
The portal does not write `.gdbinit`, LLDB settings, or editor configuration.
The JSON file remains as build-tree ownership and mapping metadata if the preference is later disabled, which is also useful for existing objects built under the profile.

Every compiler invocation supplies ccache with a build-local `base_dir`, the profile namespace, `hash_dir=true`, and empty `sloppiness`.
These arguments are stored in Ninja rules rather than in ambient shell state, so IDE and raw Ninja builds retain the policy.
A configured user namespace is preserved as the prefix of the Aobus policy namespace.
Activation reads it with the effective managed cache directory and any explicit `CCACHE_CONFIGPATH` or `CCACHE_NAMESPACE` override already applied.
The suffix includes a digest of the logical layout, mapping algorithm, and CMake implementation; changing any of those inputs cannot reuse entries under the old mapping policy.
Raw Ninja regeneration checks the saved module digest and stops with a portal-rerun instruction before applying changed cache rules under an old namespace.

Coverage and sanitizer builds are excluded because their source-reporting contracts have not been validated with normalized paths.
If the preference is enabled, those commands stop and name the process-scoped opt-out:

```bash
AOBUS_SHARED_WORKSPACES=0 ./ao coverage
AOBUS_SHARED_WORKSPACES=0 ./ao check --asan
AOBUS_SHARED_WORKSPACES=0 ./ao check --tsan
```

The environment variable is deliberately an off switch; it cannot enable an unsaved profile.
Setup reports the effective state in the current shell and explains when an off switch or the Windows SSH requirement leaves a saved enabled preference inactive.
Native sanitizer commands reject the shared profile before inspecting existing build-tree ownership, including commands that reuse binaries without rebuilding.
Dedicated clang-tidy and analyzer compile databases use the profile on supported POSIX Ninja toolchains.

## Shared workspaces in Windows SSH logons

The same setup preference enables fixed compiler paths for Windows commands launched in an isolated SSH logon.
Each invocation reserves `S:` for its authoritative source checkout and `B:` for its physical build root in that logon's local DOS-device namespace.
The compiler sees stable paths while portal locks, logs, executables, and ownership records retain their physical locations.
Analyzer and tidy diagnostics map those compiler paths back to the checkout before repository filtering and deduplication; WinUI command selection and header matching use the same physical identity without rewriting compiler arguments.
Source files remain in the original checkout, including SMB checkouts; the portal does not copy them.
The ordinary shell profile remains the fallback outside SSH, and the portal prints a notice when it selects that fallback.

Use independent SSH authentication sessions for concurrent workspaces; disable connection multiplexing with `-o ControlMaster=no -o ControlPath=none` when the client supports it.
The portal refuses occupied `S:` or `B:` drives, LocalSystem, remote build storage, overlapping source/build roots, and ambient `CL` or `_CL_` options.
It never retargets an occupied drive or modifies a global drive mapping.
The reservation mutex protects only acquisition and retirement, so independent logons can compile concurrently.
Normal completion retires the exact owned mappings after child commands return.
`ao run` handles Ctrl+C by terminating and reaping the application before retiring its mappings.
After an interrupted build or an unsuccessful application reap, mappings remain because children may still be running; end that SSH logon before retrying.
An abrupt process termination likewise requires ending the logon, not reusing its drive aliases.

Automatic build selection adds `shared-workspaces-v1` beneath the normal checkout-specific build root and preserves existing ordinary trees.
An explicit `-p` or `BUILD_DIR` maps its parent to `B:` and retains its directory basename, allowing composite checks to use a sibling WinUI tree.
The mapped parent itself must be disjoint from the source tree; for example, use `C:\builds\debug` alongside `C:\src\Aobus`, rather than `C:\src\build-debug`.
`hygiene -p` uses the selected tree for both LLVM SDK provisioning during formatting and subsequent tidy checks.
Equivalent builds must retain the same compiler-visible basename and graph layout to share entries.
Configured trees carry `aobus-windows-workspace-cache.json`; an existing tree cannot be adopted from another physical source or output location, even by a no-build consumer.
Disabling the profile selects ordinary default trees; an explicitly selected fixed-view tree still requires the profile.

Ninja and Visual Studio builds use the verified managed ccache executable with `hash_dir=true`, empty `base_dir` and `sloppiness`, a versioned policy namespace, and MSVC's `/experimental:deterministic` option.
Both C and C++ must use the native MSVC compiler; a compiler that only emulates its command-line interface is not admitted.
The policy does not ignore source paths or compiler arguments to obtain hits.
WinUI retains its precompiled header to avoid reparsing its large framework include graph during edits.
With the profile's empty `sloppiness`, ccache declines those PCH compilations and MSVC compiles them normally; sharing does not imply every WinUI translation unit is cacheable.
Release WinUI declares an identity mapping for its already-fixed `B:\` root so MSVC's deterministic LTCG checks accept the embedded PCH and object paths.
Source identities embedded in objects use `S:/`; the ownership record supplies a debugger source mapping to the physical checkout.
Apply that mapping when debugging outside the owning SSH logon.
Fixed-view trees must be built and consumed through the portal so their mappings remain alive for CMake, compilers, analysis tools, and executables.
Coverage and sanitizer exclusions also apply to this profile.
GitHub Actions sccache namespaces and backend settings remain unchanged.

## Windows compiler file tracking

The portal enables normal MSBuild compiler file tracking only for the verified managed ccache wrapper whose effective writable directories are inside native tracker exclusions and disjoint from source and build trees.
Compiler-visible source and build paths must also remain outside those exclusions.
Ordinary build trees under LocalAppData retain legacy tracking; fixed `S:` and `B:` compiler views remain eligible because native tracking compares their lexical paths.
It inspects ccache's effective configuration, including paths selected by a config file, rather than relying only on environment overrides.
That allows unchanged builds to skip compiler invocations while preserving consumed-header and generated-input invalidation.
Custom wrappers, sccache, unverified configurations, and cache outputs outside the protected folders retain the legacy compiler-tracking behavior and produce a notice.
This restriction prevents MSBuild from treating cache files as project outputs that `Clean` can delete.
The decision is scoped to each actual WinUI/MSBuild build after its compiler views exist; Ninja builds and other portal commands do not query the MSBuild tracking policy.
It is not a persistent CMake override.

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
Without a supported shared-workspace profile, debug information retains workspace paths and cross-workspace reuse is not expected.
MSVC includes the object directory in its hash; fixed Windows compiler views keep that directory stable without disabling the check.

Test-resource path definitions are scoped to the source files that use them, so unrelated test translation units do not inherit checkout-specific resource locations.
The consuming sources retain their absolute paths so test binaries can still locate fixtures when launched outside the checkout.
For Ninja builds where the input can be expressed relative to the compiler's top-level build directory, gperf receives that relative path and runs from the same directory; its generated `#line` provenance remains resolvable there.
Other generators and Windows paths on different drives retain absolute gperf provenance.
Matching relative source/build layouts can therefore produce identical generated headers, but this does not make arbitrary workspace layouts cache-equivalent.

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
