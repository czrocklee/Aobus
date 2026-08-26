---
id: development.linting
type: development
status: current
domain: development
summary: Defines lint triage, suppression, cleanup, and validation policy.
---
# Linting policy

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

## Scope behavior

- `STRICT` checks apply to production C++ under `lib/`, `app/`, `include/`, and
  `tool/`.
- `RELAXED` checks apply to C++ tests under `test/`. Test mode keeps the same
  baseline but disables test-noisy checks such as unchecked optional access,
  discarded return values, designated
  initializers for positional expected-data tables, cognitive complexity,
  identifier length, magic numbers, C arrays, C varargs, and test-only casts.
- `test/main.cpp` and non-fixture files under `test/integration/lint/` are
  ignored by normal tidy runs.
- Lint checker fixtures under `test/integration/lint/fixture/` are skipped in
  batch scans and checked only when named explicitly. The `./ao test --lint`
  suite owns fixture diagnostic and auto-fix coverage.
- Python files in scope are checked by Ruff and mypy through `./ao tidy` using
  `pyproject.toml`.

## Platform coverage

Clang-format does not depend on a compile database, so the same source can be
formatted on any native host. Linux gets clang-format from Nix, macOS gets it
from the `llvm@22` formula selected by the portal, and Windows resolves it from
the pinned LLVM SDK used by tidy (version pinned in `cmake/LlvmSdk.cmake`).
Clang-tidy must use compiler flags, defines, generated headers, and SDK headers
from a real native compile command. Complete cross-platform C++ lint coverage
is therefore the combination of Linux, macOS, and Windows runs:

- Linux owns PipeWire, ALSA, POSIX, and GTK translation units.
- macOS owns Darwin translation units and independently covers POSIX and shared translation units.
- Windows owns WASAPI and other Windows-only translation units.
- Shared translation units are intentionally checked on every host that builds them.

The macOS managed environment pins Ruff and mypy but accepts the Homebrew
Python at the contracted major/minor version instead of the exact patch. It
checks changed Python during hygiene without establishing the complete tooling
contract; the `tooling` suite on Linux and Windows owns that version and
behavior gate.

Changed-file, folder, and `--all` scopes may defer only files that are incompatible with the current host, such as WinUI or WASAPI code on Linux and GTK, ALSA, or PipeWire code on Windows.
The portal prints those platform deferrals and continues with the native files.
An explicitly selected uncovered file always fails.
If any project file is compatible with the current host but lacks an exact translation-unit command or proven header consumer, every scope fails before running a partial tidy pass.
Run the normal `./ao build` or `./ao check` workflow (`ao.bat` on Windows) to refresh the debug compilation database and Ninja dependencies, then rerun tidy.

A header first uses a same-component implementation with the same stem, including a recognized platform suffix such as `Windows`, `Linux`, or `Posix`.
When no paired implementation exists, the portal reads the native Ninja dependency graph and selects the lexicographically first repository translation unit that actually consumed the header.
With the default build selection, this read-only lookup consults the dedicated tidy Ninja tree and an existing normal debug tree: `debug` on Linux and macOS, or `windows-debug` on Windows.
Each dependency tree uses its own `compile_commands.json` output-to-translation-unit mapping, including when an MSVC dependency record names only an object output and omits the source.
The selected consumer must also exist in the primary tidy compilation database, and the header always borrows the exact command from that primary database rather than flags from the dependency-only tree.
The portal never builds the full product graph merely to populate dependency records.

`-p` and `BUILD_DIR` keep dependency lookup scoped to the selected build state instead of silently consulting the default debug tree.
The audited Visual Studio WinUI companion tree remains the existing exception: an explicit Windows tidy tree uses its `-winui` sibling.
Visual Studio-only WinUI headers use a small audited companion map because that generator does not provide the Ninja dependency graph.

## Fatal-contract source guardrails

The `aobus_guardrails` target scans production C++ under `app/`, `include/`, `lib/`, and `tool/`
and fails when a source uses the C `assert` macro or raw gsl-lite contract
spelling (`gsl_Expects`, `gsl_Ensures`, or `gsl_Assert`).
The normal completion `./ao check` gate builds this target explicitly; ordinary incremental application builds do not rerun repository-wide source scans.
Production runtime contracts use the AO macros so category, source location,
diagnostic context, and abort behavior remain project-owned and consistent.
Compile-time `static_assert` and third-party or test-source assertions are not
part of this guardrail.

CMake automatically adds `ao_` custom targets ending in `_check`, `_guardrail`,
or `_boundary_report` to `aobus_guardrails`. Use one of those suffixes for a new
completion guardrail; conditional frontend targets are discovered only when
their owning frontend is enabled.

The check is intentionally lexical and admits no per-file production
allowlist. If foreign code must retain a raw spelling, keep that code outside
the production source roots or isolate it behind the owning adapter rather
than suppressing the repository rule.

The same check-owned guardrail rejects the removed general exception surface
(`ao/Exception.h`, `ExceptionFormat`, and `throwException`) and raw fatal
call spellings (`std::terminate`, `std::abort`, `std::quick_exit`, `std::_Exit`,
their explicitly global forms, and `_Exit`).
It also rejects `AO_EXPECTS(false, ...)`, `AO_ENSURES(false, ...)`,
`AO_INVARIANT(false, ...)`, and production `std::unreachable()` so an
unconditional terminal branch has the explicit `AO_FATAL` category.
Tests remain outside that production scan because category death probes and
exhaustive-switch fixtures deliberately exercise those spellings.
Only the Core fatal implementation may invoke the final abort primitive.
Normal CLI parser exits remain `std::exit` and are not fatal-contract calls.

The `aobus-readability-forbid-raw-fatal` AST check is the semantic authority
for that rule. It resolves the standard/global `abort`, `terminate`,
`quick_exit`, and `_Exit` declarations, including imported unqualified calls
and address-taking, while ignoring unrelated project members with the same
leaf name. It also rejects direct production references to the `ao::detail::abortFatal` and `ao::detail::abortRealtime` implementation entry points; public Contract macro expansions are the only exception. The one process-termination backend helper must begin with the exact
`AO_RAW_FATAL_BACKEND()` macro expansion. A direct call to the marker helper,
a nested marker, or a later marker does not qualify. Ordinary tests are outside
production policy; the check's integration fixture remains covered. Do not
suppress this check with `NOLINT`.

The lexical build guard remains as an early failure for common call spellings;
it is intentionally not the source of symbol resolution or the backend
exception policy.

The `aobus-readability-forbid-raw-throw` AST check enforces the
[exception-carrier reference](../reference/failure/exception-carriers.md)
without copying its whitelist here.
In production source, a non-rethrowing `throw` expression is valid only when
its enclosing helper begins with `AO_EXCEPTION_CARRIER(reason)` and that helper
is inventoried by the reference.
The checker recognizes the exact first-statement macro pattern rather than
function or file names; a direct call to its implementation helper, a nested
marker, or a later marker does not qualify.
A `catch (...)`, `catch (std::exception const&)`, or
`catch (std::bad_alloc const&)` must rethrow, enter AO fatal handling, or
explicitly capture the current exception for a later owning boundary with
`std::current_exception()`.
Termination or transfer inside a nested catch, lambda, or only one branch of a
conditional does not discharge the outer catch; every continuation path must
transfer, terminate, or retain the current exception itself.
An adapter that can name a narrower foreign exception catches that exact type.
Ordinary test sources may inject arbitrary exceptions; the check's own
integration fixture remains covered so the production rule cannot regress.

An exceptional boundary that is allowed to continue begins its catch body with
`AO_AUDITED_CATCH(reason)`. The reason identifies exception classification,
best-effort diagnostics during already-safe cleanup, fatal-sink rejection,
platform fallback, or preservation of an active primary exception. The checker
recognizes only that exact first-statement macro pattern; it has no function-name
or file allowlist, and a nested or later marker does not qualify. Every production
use is inventoried in the exception-carrier reference. Do not suppress this check
with `NOLINT`.

The portal copies the selected native compiler flags into a temporary compilation database and checks the header itself as the main file.
On Windows, it removes the translation unit's `/TP` after replacing the input because the header invocation supplies `-x c++-header` explicitly.
A platform-incompatible header without a safe paired implementation, real Ninja consumer, or audited WinUI companion is deferred in a batch scan.
A compatible header without that evidence fails closed and reports that the normal build or check must refresh dependency data.
These rules cover main-file-only checks and prevent clang-tidy's fallback to a nearby but unrelated compile command from producing a false green result.

Windows tidy uses the checkout-specific `windows-tidy` tree below the local
Windows build root and the pinned official LLVM development archive. By
default, build state and the shared verified SDK cache live below
`%LOCALAPPDATA%\Aobus`, even when the source checkout is on a mapped drive. See
`doc/development/windows.md` for the state layout, overrides, and migration
instructions. CMake verifies the archive SHA-256 and builds
`tool/lint/AobusClangTidy.exe` by statically linking the Aobus checks with that
SDK's `clangTidyMain`. The official Windows `clang-tidy.exe` does not export the
symbols required by an out-of-tree DLL, so it cannot load the Linux-style
plugin. Do not substitute `clang-tidy.exe` from Visual Studio or `PATH`; it
would omit every `aobus-*` check.

The Windows portal composes its temporary Clang compilation database from two
native build trees.
The `windows-tidy` Ninja tree owns shared code, the TUI, and the CLI, while the
`windows-winui` Visual Studio tree owns WinUI and its generated C++/WinRT
headers.
WinUI deliberately remains disabled in the Ninja tree because CMake's WinUI
integration requires the Visual Studio generator.
When the selected tidy scope contains a WinUI file, the portal incrementally
builds the Release WinUI target, asks MSBuild's `GetCompileCommands` target for
the exact compiler state, validates that every selected WinUI translation unit
is present, and merges those commands with the Ninja database.
Generated translation units outside the repository source tree are excluded.
`--no-build` skips the incremental WinUI build and requires an already
configured, generated Visual Studio tree.
With `-p` or `BUILD_DIR`, the companion Visual Studio tree is the sibling whose
name appends `-winui`; the default paths remain the checkout-specific
`windows-tidy` and `windows-winui` trees.

Before Clang replay, the portal removes only the exact `/Zc:preprocessor`, `/c`, `/ZW:nostdlib`, and `/GL` driver tokens from the temporary merged database.
The first flag enables the standards-conforming MSVC preprocessor in real product builds, where it remains required.
The next two describe MSVC compile behavior that the clang-tidy driver already establishes.
The Windows tidy host tree is a Release/IPO build, but clang-cl replays one translation unit for analysis and cannot consume the `/GL` link-time code-generation request from the real compile command.
Clang rejects or reports these exact tokens as unused driver arguments during analysis.
Related spellings such as `/Zc:preprocessor-` are not removed.

Header checks reuse the exact native implementation command but remove the
CMake-generated forced PCH before replacing the input with the header.
This avoids redeclaring headers that the PCH itself aggregates.
The portal keeps other forced includes, supplies `-x c++-header`, and suppresses
only the nonportable-include-path compiler diagnostic for WinUI because
generated C++/WinRT headers preserve schema casing while Windows resolves paths
case-insensitively.
Intentional WinUI header-only support files have audited implementation
companions in the portal so the complete WinUI source folder has no deferred
headers.

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

## Filesystem path text boundaries

`aobus-portability-explicit-path-conversion` rejects
ambient narrow conversions in both directions: `path::string()` /
`path::generic_string()` and construction of a path from narrow text. Text and
durable interchange use `pathToUtf8()`, `pathToGenericUtf8()`, or
`pathFromUtf8()`. POSIX APIs that supply native filename bytes enter through
`pathFromNative()`, while calls back into a native filesystem API use
`path::native()`. A narrow-only process API receives explicit UTF-8 under the
Windows executable manifest contract. These boundaries use the facade rather
than site-local `NOLINT` approvals.

Ordinary tests remain outside this policy. Shared, POSIX, and Windows-only
production translation units are all covered; host-specific tidy runs provide
the compile commands for their respective sources.

## Header function definitions

`aobus-readability-header-function-definition` keeps concrete implementation out of headers so ordinary builds, tests, and lint runs do not repeatedly parse and instantiate the same implementation.
The [C++ coding style](coding-style.md) owns the exact pure-AST contract.
The checker does not infer getters or setters, count source lines or tokens, or treat explicit `inline` as permission.

The checker diagnoses definitions in the selected project header while ignoring system and generated headers, implicit compiler declarations, and lambda call operators.
Included headers are not diagnosed as though they were the selected main file.
Moving a definition also requires choosing an owning implementation target, so the checker is diagnostic-only and does not offer an automatic fix.

Keep the repository at zero findings rather than adding a baseline allowlist.
A function does not receive an exception merely because it appears small or is assumed to be performance-sensitive.
Move it to the owning implementation file and validate optimized behavior with the `release` workflow in [Optimized builds](optimized-builds.md).
Suppress this checker only after a stable benchmark demonstrates a material regression that IPO does not recover.
Such a suppression must name the benchmark and summarize the measured result in an adjacent English comment; file-level suppression and project allowlists are not acceptable.

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
- For naming findings, follow `doc/development/naming-convention.md`; for language and
  style findings, follow `doc/development/coding-style.md`. Do not rename public API,
  framework-required names, or vocabulary names just to appease a generic rule.
- If the tool is consistently wrong for a project pattern, consider narrowing
  the check configuration or custom rule. Do not scatter many identical
suppressions across the tree.

### Cancellation handling in coroutine catches

The `aobus-async-cancellation-guard` check protects broad handlers in
coroutines from turning cancellation into failure. A broad coroutine handler
must begin with one of three forms:

1. Call `ao::async::rethrowIfOperationCancelled(error)` before handling the
   remaining exception.
2. At a boundary that owns mandatory terminal bookkeeping, use an exhaustive
   `if (ao::async::isOperationCancelled(error)) { ... } else { ... }` as the
   first statement. Both branches must be non-empty, and the predicate must
   inspect that handler's catch variable.
3. Assign `std::current_exception()` to `std::exception_ptr` state declared
   outside the handler when cleanup must finish before propagation. Use
   separate state for separate cleanup stages when failure priority matters.
   The owner must subsequently call `ao::async::rethrowException()`, or pass
   the retained exception to a fatal terminal boundary.

The check validates only that the handler immediately transfers ownership of
the active exception. It does not perform cross-statement dataflow to prove the
later rethrow or fatal disposition; that remains an invariant of the owning
workflow and its review.

The second form is for workflows that must retain cancellation as local state
long enough to publish a terminal event, retire an in-flight request, or reset
owner state before cancellation propagates. It is not permission to swallow
cancellation or continue normal work.

A `catch (...)` has no typed catch variable to classify, so it must begin with
either `ao::async::rethrowIfOperationCancelled()` or the deferred-exception
form. A sibling catch cannot receive an exception rethrown from another
handler; use local classification or deferral when the same workflow owns
cleanup that cannot be skipped.

`bugprone-throwing-static-initialization` and `bugprone-exception-escape` are
disabled for all source modes. On MSVC they are dominated by standard-library
implementation details such as `std::map` allocating its sentinel node, while
explicit `noexcept` paths also report every potentially allocating error or
buffer operation. These diagnostics are not actionable enough to justify local
suppressions or data-structure churn. Review and tests remain responsible for
the project's intentional fail-fast boundaries.

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

`./ao tidy` rejects a named `NOLINT` when that check is disabled for the file's
`STRICT` or `RELAXED` mode. Such a directive cannot suppress a diagnostic and
is stale by definition. When a check is disabled or moved out of a mode, remove
the corresponding local suppressions in the same change.

## Repository-wide suppression governance

Classify every suppression in this order:

1. **Stale:** the configured mode cannot emit the diagnostic, or the current
   code no longer triggers it. Delete the directive without changing code.
2. **Code issue:** a local, behavior-preserving edit expresses the contract more
   clearly. Fix the code and remove the directive.
3. **Aobus checker mismatch:** an `aobus-*` rule misunderstands a reusable
   project pattern. Refine the checker and add a lint integration fixture that
   proves both the positive and negative boundary.
4. **Upstream policy mismatch:** an upstream check is systematically wrong for
   a project-wide pattern. Prefer the narrowest supported check option. Disable
   the check only when the whole diagnostic class conflicts with Aobus design;
   add a configuration test and rationale.
5. **Necessary local boundary:** an external ABI, platform API, framework macro,
   implementation-dependent standard-library type, or deliberate contract test
   requires the construct. Keep the smallest named suppression and explain the
   boundary when it is not evident.

Current policy examples follow this split. Cognitive-complexity analysis ignores
macro expansions because the caller does not own the macro's control flow, while
ordinary function bodies remain checked. The derived-method-shadowing diagnostic
is disabled because Aobus uses CRTP customization points from LLVM
`RecursiveASTVisitor` and standard range view interfaces; those methods refine a
non-virtual fallback by design. Neither policy should be represented by repeated
local suppressions.

## Things to avoid

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

## NOLINT cleanup playbook

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

## Include-Cleaner triage

Add the direct include where the symbol is used.

- If a symbol appears in a public header, the public header must include the
  provider.
- If a symbol is used only in a `.cpp`, add the provider include to the `.cpp`
  instead of relying on a paired header's transitive includes.
- For standard library symbols, include the standard header that owns the symbol.
- For GTKmm, GLib, PipeWire, LLVM, and other Linux Nix-provided libraries, use
  the package's headers and build configuration to find the provider. From the
  repo root, `nix-shell --run "pkg-config --cflags <lib>"` is useful for
  libraries that publish pkg-config metadata. On macOS, inspect the active
  vcpkg installation recorded in the dependency report.
- For Clang/LLVM internals, inspect the compile database under
  `/tmp/build/<project-directory>/debug-clang-tidy/compile_commands.json` on
  Linux or the resolved checkout-specific `windows-tidy` build tree on Windows.
  The Windows portal prints that local path. On Linux,
  `llvm-config --cxxflags` is also useful.

Suppress `misc-include-cleaner` only when the tool genuinely cannot model the
provider, such as required umbrella headers or C macros from framework headers.
When a required header is consumed only through a specialization or registration
side effect that include-cleaner cannot observe, keep the include and add the
narrowest project-level `IgnoreHeaders` entry instead of repeated local suppressions.

## Python hygiene

Ruff and mypy findings should be fixed with the same bias as C++ lint: prefer a
local code or typing improvement, keep the public shape stable unless the task
requires an API change, and avoid broad ignores.

Linux runs Ruff and mypy from the project shell. Windows uses the locked tools
in the checkout-specific environment bootstrapped by `ao.bat`; it does not use
ambient `PATH` installations. The selected Python files and project
configuration are otherwise the same on both hosts. Both environments must
match the exact versions in `script/ao/toolchain.json`: Nix checks this during
evaluation, while the Windows bootstrap and tooling tests probe the managed
environment.

- Use `./ao format` for Python formatting changes. `./ao tidy --fix` applies
  only exported `clang-tidy` replacements.
- Use targeted `# noqa: RULE` or `# type: ignore[code]` only when the tool cannot
  express the real contract. Add a short reason when it is not obvious.
- Do not silence mypy by widening types to `Any` unless the value is genuinely
  dynamic at that boundary.

## Automatic fixes

Automatic fixes can be useful, but they can also leave the working tree in a
large or confusing state. Treat them as an optional recovery-friendly shortcut,
not as the normal lint workflow.

This section addresses human contributors. Agent sessions do not run
`./ao tidy --fix` or apply exported replacements at all; agents make explicit,
reviewable hand edits (see the `use-clang-tidy` skill). The lint integration
suite's fixture auto-fix stage is separate and unaffected.

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
