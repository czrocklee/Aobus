# Linting Policy

This document is the contributor policy for lint findings in Aobus. It defines
how to triage `clang-tidy`, Ruff, and mypy findings, when suppressions are
acceptable, and how to clean up existing suppressions without changing behavior.

Use project commands as the public entry points:

- `./ao tidy` runs C++ `clang-tidy` plus Python Ruff and mypy for files in
  scope.
- `./ao hygiene` is the check-only commit gate: format check first, then tidy.
- `./ao test --lint` tests the Aobus `clang-tidy` plugin fixtures; it is not the
  Python lint command.

On native Windows use the corresponding `ao.bat` commands. The scope and policy
are the same; the portal selects host-specific tools and build trees.

Do not call `clang-tidy`, Ruff, or mypy directly during normal repository work.
The `./ao` commands own scope discovery, strict/relaxed check modes, plugin
loading, include paths, fix filtering, and diagnostic de-duplication. Keep lint
work scoped to the task; do not turn a feature, bug fix, or test change into a
drive-by lint sweep.

## Scope Behavior

- `STRICT` checks apply to production C++ under `lib/`, `app/`, `include/`, and
  `tool/`.
- `RELAXED` checks apply to C++ tests under `test/`. Test mode keeps the same
  baseline but disables test-noisy checks such as unchecked optional access,
  discarded return values, throwing static initialization, designated
  initializers for positional expected-data tables, cognitive complexity,
  identifier length, magic numbers, C arrays, C varargs, and test-only casts.
- `test/main.cpp` and non-fixture files under `test/integration/lint/` are
  ignored by normal tidy runs.
- Lint checker fixtures under `test/integration/lint/fixture/` are skipped in
  batch scans and checked only when named explicitly. The `./ao test --lint`
  suite owns fixture diagnostic and auto-fix coverage.
- Python files in scope are checked by Ruff and mypy through `./ao tidy` using
  `pyproject.toml`.

## Platform Coverage

Clang-format does not depend on a compile database, so the same source can be
formatted on either host. Linux gets clang-format from Nix; Windows resolves
the formatter from the pinned LLVM 22.1.8 SDK used by tidy and never falls back
to `PATH`. Clang-tidy must use compiler flags, defines, generated headers, and
SDK headers from a real native compile command. A complete Aobus C++ lint gate
is therefore the combination of Linux and Windows runs:

- Linux owns PipeWire, ALSA, POSIX, and GTK translation units.
- Windows owns WASAPI and other Windows-only translation units.
- Shared translation units are intentionally checked on both hosts.

Changed-file, folder, and `--all` scopes print a deferred-files summary for
translation units absent from the current host's `compile_commands.json`, then
continue with the files that are natively covered. Explicitly requesting an
uncovered translation unit fails. A header uses a same-component implementation
with the same stem (including a recognized platform suffix such as `Windows`,
`Linux`, or `Posix`). The portal copies that implementation's native compiler
flags into a temporary compilation database and checks the header itself as the
main file. On Windows, it removes the translation unit's `/TP` after replacing
the input because the header invocation supplies `-x c++-header` explicitly. A
header without a safe companion is deferred. These rules cover main-file-only
checks and prevent clang-tidy's fallback to a nearby but unrelated compile
command from producing a false green result.

Windows tidy uses the checkout-specific `windows-tidy` tree below the local
Windows build root and the pinned official LLVM 22.1.8 development archive. By
default, build state and the shared verified SDK cache live below
`%LOCALAPPDATA%\Aobus`, even when the source checkout is on a mapped drive. See
`doc/dev/windows-development.md` for the state layout, overrides, and migration
instructions. CMake verifies the archive SHA-256 and builds
`tool/lint/AobusClangTidy.exe` by statically linking the Aobus checks with that
SDK's `clangTidyMain`. The official Windows `clang-tidy.exe` does not export the
symbols required by an out-of-tree DLL, so it cannot load the Linux-style
plugin. Do not substitute `clang-tidy.exe` from Visual Studio or `PATH`; it
would omit every `aobus-*` check.

The portal also removes the exact `/Zc:preprocessor` token from a temporary copy
of the Windows compilation database before Clang replay. The flag enables the
standards-conforming MSVC preprocessor in real product builds, where it remains
required, but it has no Clang equivalent and Clang rejects it as an unused
driver argument. No other `/Zc:preprocessor*` spelling is removed.

The Windows analysis command also defines `_USE_STD_VECTOR_ALGORITHMS=0` for
clang-tidy only. This works around
[microsoft/STL#6294](https://github.com/microsoft/STL/issues/6294), where the
Visual Studio 18 STL sends three-byte element types to a vectorized helper that
supports only one-, two-, four-, and eight-byte elements. It is an MSVC STL
header issue, not an LLVM 22 incompatibility, and the define does not affect any
Aobus product build. Remove the workaround after the corresponding STL fix is
available in the required Build Tools baseline.

Set `AOBUS_LLVM_SDK_CACHE_ROOT` to relocate the automatically managed shared
cache. Set the distinct `AOBUS_LLVM_SDK_ROOT` CMake cache option at configure
time to use one already extracted copy of the exact archive, for example on an
offline machine. A pre-extracted root must contain the LLVM and Clang CMake
packages, static libraries, tools, and resource headers; configuration fails
closed when any required SDK file is missing.

## Triage

Start by deciding whether the warning points at a real code issue, a project
style issue, a tool false positive, or an unavoidable external API shape.

LLVM upgrades can add checks to an enabled wildcard family, rename checks from
another policy family into one Aobus enables, or broaden an existing check.
Review the release notes and the resulting diagnostic classes as a policy
change: explicitly disable rules that conflict with project architecture, tune
new options that restore the intended scope, and fix findings that match Aobus
policy. Do not convert a toolchain-wide policy mismatch into repeated local
suppressions.

- Treat correctness, lifetime, ownership, optional access, and special-member
  warnings as real problems unless the local code proves otherwise.
- Fix readability findings when the change makes the code clearer to a future
  reader. Prefer named constants, early returns, clearer expressions, or a small
  local helper over mechanical churn.
- Fix include findings by adding the direct header that provides the used
  symbol. Do not rely on transitive includes.
- For RAII guards, explicitly delete copy/move or define the needed operations.
- For naming findings, follow `doc/dev/naming-conventions.md`; for language and
  style findings, follow `doc/dev/coding-style.md`. Do not rename public API,
  framework-required names, or vocabulary names just to appease a generic rule.
- If the tool is consistently wrong for a project pattern, consider narrowing
  the check configuration or custom rule. Do not scatter many identical
  suppressions across the tree.

## Suppressions

Use `NOLINT` only when the warning is caused by an external API shape, a clear
false positive, or a test-only pattern where the fix would be worse than the
warning.

- Prefer `NOLINTNEXTLINE(check-name)` or inline `NOLINT(check-name)` at the
  exact expression.
- Include the specific check name. Avoid bare `NOLINT`.
- Add a short English reason when the boundary is not obvious from the code.
- Use `NOLINTBEGIN/END` only for a compact, contiguous region that cannot be
  made clearer locally.

Common acceptable cases include GTKmm ownership handoff such as
`Glib::make_refptr_for_instance(new T)`, GLib/GTK macros, C varargs or C arrays
at an API boundary, unavoidable `reinterpret_cast` in tests, framework-required
method names, and `clang-tidy` false positives around framework or template
code.

## Things To Avoid

- Do not disable checks directory-wide or file-wide.
- Do not add umbrella includes to satisfy include-cleaner unless the external
  library requires that umbrella header.
- Do not add global constants for one-use literals.
- Do not hide a one-off C API warning behind an abstraction that has no design
  value.
- Do not split clear local logic into many single-use functions just to reduce a
  metric.
- Do not mix include cleanup with behavioral lint cleanup unless the task
  explicitly asks for both.

## NOLINT Cleanup Playbook

When reducing existing suppressions, use the smallest semantic-preserving edit
and re-run tidy on the touched files before widening scope.

1. Delete stale suppressions first. If the line no longer warns, keep only the
   deletion.
2. Keep include-cleaner work separate when the task excludes include cleanup.
3. Replace a suppression with clearer code when the fix is local and
   behavior-preserving.
4. Keep a targeted suppression when the clean code would be less readable or
   would obscure an external API contract.

Useful cleanup patterns:

- Replace unexplained protocol, binary-layout, or UI-policy literals with named
  `constexpr` values when the name carries real domain meaning.
- For binary-layout assertions, prefer a named byte-count constant on the layout
  type over suppressing a raw size literal.
- For unused overload parameters, use comment names such as `Type& /*value*/`
  instead of suppressing `readability-named-parameter`.
- For strict full-string unsigned parsing, prefer `std::from_chars` over
  `strtoul`; it avoids C output-parameter suppressions and preserves
  no-leading-space behavior.
- At C API pointer boundaries in tests, prefer existing helpers such as
  `utility::layout::asLegacyPtr<T>(ptr)` when they express the boundary
  directly. Otherwise keep a narrow suppression at the boundary.
- For C structs used by framework tests, prefer `std::array`, `std::to_array`,
  `std::span`, or a tiny local designated-initializer helper when that is
  clearer than raw arrays and macro initializers.
- Iterator trait aliases such as `value_type`, `difference_type`, `reference`,
  `pointer`, and `iterator_category` are STL vocabulary names. Keep them
  allowlisted in lint configuration instead of suppressing each alias.
- GTKmm/glibmm ownership boundaries are usually acceptable suppressions. Do not
  hide them behind helpers unless the local class design already supports that
  helper cleanly.
- Binary or protocol literals can be cleaned with named constants, but if the
  named constant reads worse than the documented format literal, keep a narrow
  suppression or revisit the rule.

## Include-Cleaner Triage

Add the direct include where the symbol is used.

- If a symbol appears in a public header, the public header must include the
  provider.
- If a symbol is used only in a `.cpp`, add the provider include to the `.cpp`
  instead of relying on a paired header's transitive includes.
- For standard library symbols, include the standard header that owns the symbol.
- For GTKmm, GLib, PipeWire, LLVM, and other Nix-provided libraries, use the
  package's headers and build configuration to find the provider. From the repo
  root, `nix-shell --run "pkg-config --cflags <lib>"` is useful for libraries
  that publish pkg-config metadata.
- For Clang/LLVM internals, inspect the compile database under
  `/tmp/build/debug-clang-tidy/compile_commands.json` on Linux or the resolved
  checkout-specific `windows-tidy` build tree on Windows. The Windows portal
  prints that local path. On Linux, `llvm-config --cxxflags` is also useful.

Suppress `misc-include-cleaner` only when the tool genuinely cannot model the
provider, such as required umbrella headers or C macros from framework headers.

## Python Hygiene

Ruff and mypy findings should be fixed with the same bias as C++ lint: prefer a
local code or typing improvement, keep the public shape stable unless the task
requires an API change, and avoid broad ignores.

Linux runs Ruff and mypy from the project shell. Windows uses the locked tools
in the checkout-specific environment bootstrapped by `ao.bat`; it does not use
ambient `PATH` installations. The selected Python files and project
configuration are otherwise the same on both hosts.

- Use `./ao format` for Python formatting changes. `./ao tidy --fix` applies
  only exported `clang-tidy` replacements.
- Use targeted `# noqa: RULE` or `# type: ignore[code]` only when the tool cannot
  express the real contract. Add a short reason when it is not obvious.
- Do not silence mypy by widening types to `Any` unless the value is genuinely
  dynamic at that boundary.

## Automatic Fixes

Automatic fixes can be useful, but they can also leave the working tree in a
large or confusing state. Treat them as an optional recovery-friendly shortcut,
not as the normal lint workflow.

Consider automatic fixes only when the working tree is clean or otherwise easy
to revert, and when the diagnostic is mechanical enough that the generated diff
will be straightforward to review. They are most defensible for simple repeated
edits, checker fixtures, obvious local modernization, or a large batch of
low-judgment changes that would be more error-prone to perform by hand.

Prefer hand edits when the warning involves ownership, public API shape,
behavior, naming, readability tradeoffs, or framework boundaries. Never run
automatic fixes across the whole repository. After any automatic fix, review the
diff before continuing and run the same verification you would run for a manual
edit.

## Verification

After C++ lint edits, re-run the narrowest `./ao tidy` scope that covers the
modified C++ files. After Python hygiene edits, re-run `./ao tidy` for the
modified Python files and `./ao test --tooling` when tooling behavior changed.
Run focused build or test validation when a lint fix changes behavior,
ownership, public API shape, or test semantics.

For a change that touches platform-specific C++, run the corresponding native
tidy pass. For a cross-platform change, both native passes are required; neither
host can validate translation units that its build does not generate.
