---
id: development.lint.checker-development
---
# Aobus clang-tidy checker development

This page serves maintainers of `tool/lint/` and its integration fixtures.
The contributor-facing [linting policy](../linting.md) owns triage, suppressions, scope, and fix policy.
The [coding style](../coding-style.md), [naming conventions](../naming-convention.md), and product specifications own the rules that checkers enforce; a checker must not become a second policy owner.

## Custom checker development

Checkers live in `tool/lint/check/`.
Follow the owning check's namespace and register new sources in `tool/lint/AobusLintModule.cpp` and `tool/lint/CMakeLists.txt`.
Keep a helper local until at least two current checks need the same stable contract.
Existing `AstHelpers.h`, `CalleeQualificationHelpers.h`, and `RaiiHeuristics.h` own shared AST operations.

Identify symbols by declarations and qualified names rather than source-text substrings.
Identify objects by canonical declarations or bound nodes rather than spelling.
For FixIt changes, cover macro boundaries, same-named foreign symbols, and cross-object cases where relevant.
A macro diagnostic may be valid while its replacement is unsafe; use `aobus::isInMacro` and omit an unsafe FixIt.

Use `clang-query` or a raw AST dump when a matcher shape is uncertain, with the affected native compile database and toolchain.
Linux tools come from the project Nix environment, macOS uses the portal-selected LLVM, and Windows uses the governed LLVM SDK.
An unchanged matcher already exercised by fixtures does not need another AST investigation for a diagnostic-text correction.

Common AST traps include:

- `ignoringParenImpCasts` does not unwrap constructor or materialization nodes; inspect the implicit-node chain before choosing a traversal.
- The node bound by a matcher must match the type passed to `getNodeAs`.
- `DeclRefExpr` nodes for one object differ; compare their declarations.
- Ranges algorithms may be function objects in implementation inline namespaces. Their calls use `CXXOperatorCallExpr`, whose first argument is the callee object.
- Argument counts include default arguments; count explicit arguments when that is the contract.
- C++20 rewritten comparisons contain synthesized operators. Use `aobus::isWithinRewrittenOperator` or match the source comparison to avoid duplicate or inverted diagnostics.
- Initializer-list constructor detection uses the first parameter's type; parent traversal requires `clang/AST/ParentMapContext.h`.

### Fixture contract

Place fixtures under `test/integration/lint/fixture/<check-alias>/`; the directory selects the check.
The [test-suite reference](../test/test-suite.md) owns the complete runner behavior.
Use these markers immediately before the source line:

- `// POSITIVE` requires a diagnostic;
- `// NEGATIVE` forbids a diagnostic; and
- `// POSITIVE: FIX-TO: <fixed line>` requires a diagnostic and the stated replacement.

Only fixtures with `FIX-TO` enter the auto-fix stage, and the runner syntax-checks the fixed temporary copies.
Markers assert normalized file and line identity plus FixIt output, not diagnostic wording.
A wording-only change may reuse the fixture and inspect emitted text.
Extend an owning fixture rather than creating a parallel case for a covered contract.

For Objective-C++ `.mm` fixtures, local `.h` context headers also participate in diagnostic assertions.
A marker-bearing context header must be reached through unconditional literal quoted includes or imports in that directory; the runner follows them recursively and checks each file's own markers.
For other fixture types, the runner loads marker expectations only from the main fixture file; compiling an included header does not make its markers assertions.
A header diagnostic cannot satisfy or hide a source marker.
Keep conditional and generated include graphs out of marker-bearing context headers.
Header `FIX-TO` markers require a standalone fixture; context headers support `POSITIVE` and `NEGATIVE` only.
The runner snapshots copied source and context headers before clang-tidy can change line locations.

Objective-C++ `.mm` fixtures enable blocks for diagnostics, FixIts, and fixed-output syntax checks.
Fixed copies use `OBJCXX` when supplied, otherwise the managed `clang++`; they do not inherit a C++-only GCC `CXX` selection.
Declaration-only fixtures require no Foundation link.

Use `./ao test --lint` for the integration gate.
For focused diagnosis with an already prepared native plugin and compile database, use:

```bash
./ao tidy --no-build --check <alias> <fixture>
```

## Semantic function and value naming

The [naming-check reference](naming-checks.md) owns the checker inventory, framework identity proof, dependent-template shapes, and bounded automatic inference for bool, Task, Result, pointer, optional, chrono, and record naming.
Link the complete proof model instead of maintaining another inventory; local comments should explain the particular positive or negative boundary a fixture demonstrates.

## Header function definitions

`aobus-readability-header-function-definition` keeps concrete implementation out of headers so ordinary builds, tests, and lint runs do not repeatedly parse and instantiate it.
The [C++ coding style](../coding-style.md#formatting-and-source-layout) owns the exact contract.
The checker uses direct AST statements: it does not infer getters or setters, count source lines or tokens, or treat explicit `inline` as permission.

The checker diagnoses definitions in the selected project header while ignoring system and generated headers, implicit compiler declarations, and lambda call operators.
Included headers are not diagnosed as though they were the selected main file.
Moving a definition requires choosing an owning implementation target, so the check is diagnostic-only and offers no automatic fix.

Keep the repository at zero findings rather than adding a baseline allowlist.
A function receives no exception merely because it looks small or performance-sensitive.
Move it to the owning implementation and validate optimized behavior with the `release` workflow in [optimized builds](../optimized-builds.md).
Suppress this checker only after a stable benchmark demonstrates a material regression that IPO does not recover.
The adjacent English comment must name the benchmark and summarize the measured result; file-level suppression and project allowlists are not acceptable.

## Fatal-contract source guardrails

The `aobus_guardrails` target lexically scans production C++ under `app/`, `include/`, `lib/`, and `tool/`.
It rejects the C `assert` macro and raw gsl-lite contract spellings (`gsl_Expects`, `gsl_Ensures`, and `gsl_Assert`).
The completion `./ao check` gate builds this target explicitly; ordinary incremental application builds do not rerun repository-wide source scans.
Production code uses AO macros so category, source location, diagnostic context, and abort behavior remain project-owned.
`static_assert`, third-party source, and test-source assertions remain outside this lexical rule.

CMake automatically adds `ao_` custom targets ending in `_audit`, `_check`, `_guardrail`, or `_boundary_report` to `aobus_guardrails`.
Use one of those suffixes for a new completion guardrail.
Conditional frontend targets are discovered only when their owner is enabled.
The lexical check has no per-file production allowlist; isolate foreign code behind its owning adapter or outside production roots rather than suppressing the rule.

The same guardrail rejects:

- the removed general exception surface: `ao/Exception.h`, `ExceptionFormat`, and `throwException`;
- raw fatal call spellings for `std::terminate`, `std::abort`, `std::quick_exit`, `std::_Exit`, their explicitly global forms, and `_Exit`;
- `AO_EXPECTS(false, ...)`, `AO_ENSURES(false, ...)`, and `AO_INVARIANT(false, ...)`; and
- production `std::unreachable()`.

An unconditional terminal branch uses the explicit `AO_FATAL` category.
Tests remain outside the production scan because death probes and exhaustive-switch fixtures deliberately exercise these spellings.
Only the Core fatal implementation may invoke the final abort primitive.
Normal CLI parser exits remain `std::exit` and are not fatal-contract calls.

### Raw-fatal AST authority

`aobus-readability-forbid-raw-fatal` is the semantic authority.
It resolves standard and global `abort`, `terminate`, `quick_exit`, and `_Exit` declarations, including imported unqualified calls and address-taking, while ignoring unrelated project members with the same leaf name.
It also rejects direct production references to `ao::detail::abortFatal` and `ao::detail::abortRealtime`; public Contract macro expansions are the only exception.

The one process-termination backend helper must begin with the exact `AO_RAW_FATAL_BACKEND()` macro expansion.
A direct call to the marker helper, a nested marker, or a later marker does not qualify.
Ordinary tests are outside production policy; the integration fixture remains covered.
Do not suppress this check with `NOLINT`.
The lexical guard remains an early failure for common spellings, not the authority for symbol resolution or backend exceptions.

### Raw-throw and catch authority

`aobus-readability-forbid-raw-throw` enforces the [exception-carrier reference](../../system/failure/exception-carriers.md) without copying its whitelist.
In production, a non-rethrowing `throw` is valid only when its enclosing helper begins with `AO_EXCEPTION_CARRIER(reason)` and that helper is inventoried by the reference.
The checker recognizes the exact first-statement macro pattern rather than function or file names; a direct call to the implementation helper, a nested marker, or a later marker does not qualify.

A `catch (...)`, `catch (std::exception const&)`, or `catch (std::bad_alloc const&)` must rethrow, enter AO fatal handling, or explicitly retain the current exception for a later owning boundary with `std::current_exception()`.
Termination or transfer in a nested catch, lambda, or only one conditional branch does not discharge the outer catch.
Every continuation path must transfer, terminate, or retain the current exception itself.
An adapter that can name a narrower foreign exception catches that exact type.
Ordinary tests may inject arbitrary exceptions; the check's integration fixture remains covered.

An exceptional boundary allowed to continue begins its catch body with `AO_AUDITED_CATCH(reason)`.
The reason identifies exception classification, best-effort diagnostics during already-safe cleanup, fatal-sink rejection, platform fallback, or preservation of an active primary exception.
Only the exact first-statement macro pattern qualifies; there is no function-name or file allowlist, and a nested or later marker is insufficient.
Every production use is inventoried in the exception-carrier reference.
Do not suppress this check with `NOLINT`.

## Cancellation handling in coroutine catches

`aobus-async-cancellation-guard` prevents a broad coroutine handler from converting cancellation into failure.
Such a handler must begin with one of these forms:

1. Call `ao::async::rethrowIfOperationCancelled(error)` before handling the remaining exception.
2. At a boundary that owns mandatory terminal bookkeeping, use an exhaustive `if (ao::async::isOperationCancelled(error)) { ... } else { ... }` as the first statement. Both branches must be non-empty, and the predicate must inspect that handler's catch variable.
3. Assign `std::current_exception()` to `std::exception_ptr` state declared outside the handler when cleanup must finish before propagation. Use separate state for separate cleanup stages when failure priority matters. The owner must later call `ao::async::rethrowException()` or pass the retained exception to a fatal terminal boundary.

The check proves only that the handler immediately transfers ownership of the active exception.
It does not perform cross-statement dataflow to prove the later rethrow or fatal disposition; that remains an invariant of the owning workflow and review.
The second form permits terminal event publication, in-flight retirement, or owner reset before propagation; it does not permit cancellation to be swallowed or normal work to continue.

A `catch (...)` has no typed variable to classify, so it begins with either `ao::async::rethrowIfOperationCancelled()` or the deferred-exception form.
A sibling catch cannot receive an exception rethrown by another handler; classify or defer locally when cleanup cannot be skipped.

`bugprone-throwing-static-initialization` and `bugprone-exception-escape` are disabled in every source mode.
On MSVC they are dominated by standard-library implementation details such as `std::map` sentinel allocation, while explicit `noexcept` paths report every potentially allocating error or buffer operation.
These diagnostics are not actionable enough to justify local suppressions or data-structure churn.
Review and tests remain responsible for intentional fail-fast boundaries.

## Filesystem path text boundaries

`aobus-portability-explicit-path-conversion` rejects ambient narrow conversion in both directions: narrow-returning `path::string()` or `path::generic_string()`, and construction of a path from narrow text. The construction check deliberately permits an ASCII-only string literal, whose byte interpretation is invariant across the supported native encodings; non-ASCII literals and other narrow sources require an explicit boundary.
Text and durable interchange use `pathToUtf8()`, `pathToGenericUtf8()`, or `pathFromUtf8()`.
POSIX APIs supplying native filename bytes enter through `pathFromNative()`, and calls back to a native filesystem API use `path::native()`.
A narrow-only process API receives explicit UTF-8 under the Windows executable-manifest contract.
Use these facades rather than site-local `NOLINT` approvals.

Ordinary tests remain outside this policy.
Shared, POSIX, and Windows production translation units are covered by their host-specific tidy runs.

## Native replay and header diagnostics

Clang-tidy requires an exact native compilation command.
For a selected header, the portal first prefers a same-component implementation with the same stem, including recognized `Windows`, `Linux`, or `Posix` suffixes.
Otherwise it reads the native Ninja dependency graph and selects the lexicographically first repository translation unit that consumed the header.
The selected consumer must also be present in the primary tidy compilation database, and the header borrows the exact command from that primary database rather than flags from a dependency-only tree.
The portal does not build the full product graph merely to populate dependency records.

With default selection, lookup consults the dedicated tidy Ninja tree and an existing normal debug tree: `debug` on Linux and macOS, `windows-debug` on Windows.
Each tree uses its own `compile_commands.json` output-to-source mapping, including MSVC dependency records that name only an object output.
`-p` and `BUILD_DIR` confine lookup to selected build state.
The audited Visual Studio WinUI companion remains the exception: an explicit Windows tidy tree uses its `-winui` sibling, and Visual Studio-only headers use a small audited companion map because that generator provides no Ninja graph.

The portal copies selected compiler flags into a temporary compilation database and checks the header as the main file.
On Windows it removes the translation unit's `/TP` after replacing the input because the invocation supplies `-x c++-header`.
It also removes the CMake-generated forced PCH to avoid redeclaring the headers that PCH aggregates, while retaining other forced includes.
For WinUI it suppresses only the nonportable-include-path compiler diagnostic because generated C++/WinRT headers preserve schema casing while Windows resolves paths case-insensitively.
Intentional header-only WinUI support has audited implementation companions.

A platform-incompatible header without a safe pair, real Ninja consumer, or audited WinUI companion is deferred in a batch scan.
A compatible header without that evidence fails closed and asks for a normal build or check to refresh dependency data.
An explicitly selected uncovered file fails.
These rules cover main-file-only checks and prevent clang-tidy from falling back to a nearby unrelated command.

### Windows tidy host

Windows uses the checkout-specific `windows-tidy` tree and the pinned official LLVM development archive.
State and the shared verified SDK cache default below `%LOCALAPPDATA%\Aobus`; [Windows development](../windows.md) owns paths and overrides.
CMake verifies the archive SHA-256 and statically links `tool/lint/AobusClangTidy.exe` with the Aobus checks.
The official Windows `clang-tidy.exe` does not export the symbols needed by an out-of-tree DLL.
Never substitute a Visual Studio or `PATH` executable, which would omit every `aobus-*` check.

The temporary Windows database merges two native trees.
`windows-tidy` owns shared code, TUI, and CLI; the Visual Studio `windows-winui` tree owns WinUI and generated C++/WinRT headers.
When WinUI is in scope, the portal incrementally builds the Debug WinUI target already used by `ao.bat check`, asks MSBuild `GetCompileCommands` for exact compiler state, validates every selected translation unit, excludes generated translation units outside the repository, and merges the commands.
`--no-build` skips the incremental build and requires an existing configured and generated tree.
With `-p` or `BUILD_DIR`, the companion appends `-winui`; default names remain `windows-tidy` and `windows-winui`.

Before replay, the portal removes only exact `/Zc:preprocessor`, `/c`, `/ZW:nostdlib`, `/GL`, `/GL-`, and `/experimental:deterministic` driver tokens.
The first remains required in product builds; `/c` and `/ZW:nostdlib` describe MSVC behavior the clang-tidy driver already establishes; both IPO spellings are irrelevant to one-translation-unit analysis of the Release/IPO host tree; and deterministic output is a compiler code-generation toggle rather than analysis input.
Other related spellings, such as `/Zc:preprocessor-`, are retained.

The Windows analysis command defines `_USE_STD_VECTOR_ALGORITHMS=0` only for clang-tidy as a workaround for [microsoft/STL#6294](https://github.com/microsoft/STL/issues/6294), in which Visual Studio 18 sends three-byte element types to a vectorized helper that accepts only one-, two-, four-, or eight-byte types.
This is an MSVC STL issue, not an LLVM 22 incompatibility, and does not affect product builds.
Remove the workaround when the corresponding STL fix enters the required Build Tools baseline.

`AOBUS_LLVM_SDK_CACHE_ROOT` relocates the managed shared cache.
The distinct `AOBUS_LLVM_SDK_ROOT` CMake cache option selects a pre-extracted exact archive, for example offline.
That root must contain the LLVM and Clang CMake packages, static libraries, tools, and resource headers; configuration fails closed if a required file is absent.

## Validation

Run the owning fixture through `./ao test --lint` after checker or fixture changes.
Use focused `./ao tidy --no-build --check <alias> <fixture>` only for diagnosis, not as a replacement for the integration gate.
Changes to portal orchestration also follow the tooling and native-host matrix in [validation and review](../test/validation-and-review.md).
A platform-specific checker boundary requires its native fixture pass; an unmatched AST on another host is not coverage.
