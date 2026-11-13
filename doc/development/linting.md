---
id: development.linting
---
# Linting policy

This guide tells contributors how to handle `clang-tidy`, Ruff, mypy, and Buildifier findings.
Checker internals, fixture design, AST proof, and native replay mechanics are in [checker development](lint/checker-development.md); naming-specific proof is in [naming checks](lint/naming-checks.md).

Use project commands as the public entry points:

- `./ao tidy` runs C++ clang-tidy, Python Ruff/mypy, and Bazel Buildifier checks for files in scope; the C++ stage consumes the Bazel lint report actions.
- `./ao hygiene` is the check-only commit gate: format check, source audits, then tidy.
- `./ao test --lint` tests the Aobus clang-tidy plugin fixtures; it is not the Python lint command.

Use the corresponding `ao.bat` commands on native Windows.
Do not call clang-tidy, Ruff, mypy, or Buildifier directly during normal repository work.
The portal owns scope discovery, strict and relaxed modes, plugin loading, include paths, and diagnostic de-duplication.
Keep lint work scoped to the task; do not turn another change into a drive-by cleanup campaign.

## Scope behavior

With no explicit scope, the portal compares the topic to its merge base with local `main`, then includes working-tree, staged, and untracked sources.
On `main`, or when local `main` is unavailable, it uses `HEAD~1`.
An explicit `--commit <rev>` compares the endpoint against that revision.

Hygiene resolves source scope once for formatting, applicable naming and test audits, and tidy.
Empty subsets are skipped, a formatting failure stops later stages, and repository-wide guardrails remain part of `check`.

- `STRICT` applies to production C++ under `lib/`, `app/`, `include/`, and `tool/`.
- `RELAXED` applies to C++ under `test/` with the same baseline but disables test-noisy checks: unchecked optional access, discarded return values, designated initializers for positional expected-data tables, cognitive complexity, identifier length, magic numbers, C arrays, C varargs, and test-only casts.
- Non-fixture C++ files under `test/lint/` are ignored by normal tidy runs.
- C++ fixtures under `test/lint/fixture/` are skipped in batch scans and checked only when explicitly named. `./ao test --lint` owns their diagnostics.
- Python files in scope are checked against `pyproject.toml` by Ruff and mypy through `./ao tidy`.

### Frontend adjustments

The [shared frontend policy](../../script/ao/core/tidypolicy.py) follows the
compilation context. GTK contexts disable `cppcoreguidelines-owning-memory` and
nonliteral-format warnings: widget trees own their children and use printf-style
APIs. WinUI contexts scope warnings to the selected source/header rather than the
implementation headers gathered by the forced include; changed headers receive
their own header-main analysis. On Windows, generated C++/WinRT include spelling
is accepted without nonportable-include-path warnings.

### Bazel-backed reports

The C++ stage's Bazel lint report route has bounded native Debug evidence on Linux, macOS and Windows. The Linux workflow contains a representative lint step; current-head CI status remains a separate gate.
It accepts the file/`--folder`/`--commit`/`--all` selectors plus repeatable `--target`; `--target` alone covers every actual translation unit and paired header main in those owners, not unpaired declared headers.
Without `--target`, scope maps through actual Bazel `srcs` owners (including filegroups and aliases), not a dependency closure; with `--target`, those owners restrict the request and the selected scope gates the reports.
A header without a same-component companion, or an include fragment, is analyzed in the native context of a translation unit whose report recorded including it. Fragment warnings are scoped to its normalized file identity; consumer compiler errors and unlocated diagnostics remain, and scoping cannot hide an incomplete attempt.
Deferral is limited to platform-incompatible files; a compatible header or fragment that no analyzed translation unit includes, or a compatible selected path with no owner, fails rather than producing a partial pass.
`--all` selects the project C++ scope with the same rules.

Reports are cached from actual native compile contexts and gated from the current BEP: 0 clean, 1 findings, 2 incomplete.
Cached warnings still fail.
`--check` and `--header-filter` are analysis-keyed; scope is not.
`--tidy-arg` accepts only the documented diagnostic-only allowlist, and other raw options are rejected before preparation.
The existing ownership/suppression policy, including stale `NOLINT` detection, applies; the route never exports or applies fixes.
Windows Clang replay excludes MSVC `/WX` warning promotion alongside audited code-generation flags: Clang-only compatibility warnings must not become MSVC hard errors. Unix replay appends `-Wno-error` for the same reason. Native product compilation keeps its warning promotion; the linter's configured checks and genuine compiler-error failures are unchanged.
The manual fixture entry is `./ao test lint`; native fixtures pass on Linux, macOS Debug and Windows (with explicit Objective-C++ deferrals) and remain outside the default aggregate.
See the [Bazel lint workflow and report contracts](bazel.md#lint-reports).

## Objective-C diagnostics

Objective-C++ (`.mm`) production files remain `STRICT`.
The shared formatter supports Objective-C syntax, and native tidy uses the exact Objective-C++ command, including ARC and the macOS SDK.
C++ checks still apply to C++ declarations; framework signatures and ARC ownership follow the [Cocoa boundary](coding-style.md#objective-c-boundary).

The curated baseline adds `objc-avoid-nserror-init`, `objc-dealloc-in-category`, `objc-forbidden-subclassing`, `objc-missing-hash`, `objc-nsinvocation-argument-lifetime`, `objc-property-declaration`, `objc-super-self`, and `google-objc-avoid-throwing-exception`.
These checks complement the C++ rules rather than imposing a foreign naming style.
Checker regressions must distinguish Objective-C methods and ivars from C++ methods and record fields; a clean unmatched AST is not coverage.

`./ao analyze` includes stable `clang-analyzer-osx.*` checks for Cocoa and Core Foundation path analysis.
It is report-only unless `--fail-on-diagnostics` is selected; tool failures always fail.
It runs a distinct ANALYZER profile over the same Bazel compile actions — a
minimal `clang-analyzer-*` check group set, with the alpha, check, and test
group leak policies owned by the analyzer configuration — and never reuses the
STRICT/RELAXED lint report actions.
Select an explicit native source scope so analysis uses the matching compile action and SDK.
An Objective-C++ unit without an exact command fails analysis.
Batch scopes report incompatible platform files as not analyzed; explicit selection fails.
The `.mm` suffix selects a language, not a platform owner, so add a new native target's source tree to platform coverage with the target itself.

For the native media adapter, select the tree containing its compile action:

```bash
./ao analyze app/macos-appkit/MediaPlayerAdapter.mm --path /path/to/native/build --fail-on-diagnostics
```

The [LLVM check catalog](https://releases.llvm.org/22.1.0/tools/clang/tools/extra/docs/clang-tidy/checks/list.html) and [analyzer catalog](https://clang.llvm.org/docs/analyzer/checkers.html) define upstream checks.
Runtime evidence and Main Thread Checker remain separate; see [macOS development](macos.md).

## Platform coverage

Clang-format is source-based and may run on any native host.
Linux obtains it from Nix, macOS from the portal-selected `llvm@22`, and Windows from the LLVM SDK pinned in [`script/ao/windows-llvm-sdk.json`](../../script/ao/windows-llvm-sdk.json), which `ao.bat deps provision` installs and validates.
Clang-tidy must replay a real native compile command with its flags, defines, generated headers, and SDK:

- Linux owns PipeWire, ALSA, POSIX, and GTK translation units.
- macOS owns Darwin translation units and independently covers POSIX and shared units.
- Windows owns WASAPI and other Windows-only units.
- Shared units are intentionally checked on every host that builds them.

Changed-file, folder, and `--all` scopes may defer only files incompatible with the current host, and the portal reports every deferral.
An explicitly selected uncovered file always fails.
If a compatible project file lacks an exact translation-unit command or a proved header consumer, the whole requested scope fails before a partial tidy pass.
Run the normal `./ao build` or `./ao check` workflow (`ao.bat` on Windows) to refresh the configured compile actions, then retry.
Header consumer selection, WinUI command merging, and Windows flag replay are specified in [native replay and header diagnostics](lint/checker-development.md#native-replay-and-header-diagnostics).

The managed macOS environment pins Ruff and mypy but accepts the contracted Homebrew Python major/minor rather than an exact patch.
It checks changed Python during hygiene without claiming the full tooling contract.
The `tooling` suite on Linux and Windows owns the version and behavior gate.

## Policy-specific diagnostics

The coding and product contracts remain authoritative when a custom check reports them:

| Diagnostic area | Contributor action | Technical reference |
| --- | --- | --- |
| Semantic naming | Apply the [naming conventions](naming-convention.md); rename the complete declaration and consumer set. | [Naming checks](lint/naming-checks.md) |
| Header definitions | Move a disallowed definition to its owning `.cpp`; do not infer an exception from size, accessor intent, or `inline`. | [Header function definitions](lint/checker-development.md#header-function-definitions) |
| AO fatal and exception boundaries | Use the owned AO contract or exception-carrier surface; do not suppress the raw-fatal and raw-throw checks. | [Fatal-contract source guardrails](lint/checker-development.md#fatal-contract-source-guardrails) |
| Broad coroutine catches | Transfer cancellation first through one of the approved forms; later disposition remains the workflow owner's invariant. | [Cancellation handling](lint/checker-development.md#cancellation-handling-in-coroutine-catches) |
| Filesystem path text | Use `pathToUtf8()`, `pathToGenericUtf8()`, `pathFromUtf8()`, `pathFromNative()`, or `path::native()` at the appropriate boundary. | [Filesystem path text boundaries](lint/checker-development.md#filesystem-path-text-boundaries) |

The raw contract lexical guard remains an early failure for common spellings; AST checks own symbol resolution and narrow implementation-marker exceptions.
New guardrail policy is owned by the shared Python inventory and rules in [`guardrails.py`](../../script/ao/core/guardrails.py), which the completion gate consumes directly.
The completion `./ao check` runs the shared audits; ordinary incremental product builds do not rerun repository-wide scans.

## Triage

First classify the warning as a real code issue, a project-style issue, a tool false positive, or an unavoidable external API shape.
Treat correctness, lifetime, ownership, optional access, and special-member diagnostics as real unless local code proves otherwise.

- Fix readability findings when the result is clearer to a future reader. Prefer named constants, early returns, clear expressions, or a small local helper over mechanical churn.
- Add the direct header that provides a used symbol; do not rely on transitive includes.
- For RAII guards, explicitly delete copy and move or define the required operations.
- For naming, use the [naming conventions](naming-convention.md); for language and style, use the [C++ coding style](coding-style.md). Do not rename public API, framework-required names, or boundary vocabulary merely to satisfy a generic rule.
- If a tool is consistently wrong for a reusable project pattern, narrow its configuration or refine the custom check. Do not scatter identical suppressions.

An LLVM upgrade may add checks through a wildcard family, move checks into an enabled family, or broaden existing behavior.
Treat the resulting diagnostic classes as a policy change: review release notes, explicitly disable rules that conflict with Aobus architecture, tune options that restore intended scope, and fix findings that match policy.
Do not turn a toolchain-wide mismatch into repeated local suppressions.

## Suppressions

Use `NOLINT` only for an external API shape, a clear false positive, or a test-only pattern where a fix would be worse than the warning.

- Prefer `NOLINTNEXTLINE(check-name)` or inline `NOLINT(check-name)` at the exact expression.
- Always name the check; avoid bare `NOLINT`.
- Add a short English reason when the boundary is not obvious.
- Use `NOLINTBEGIN/END` only for a compact contiguous region that cannot be clarified locally.

Acceptable cases include GTKmm ownership handoff such as `Glib::make_refptr_for_instance(new T)`, GLib or GTK macros, C varargs or arrays at an API boundary, unavoidable test `reinterpret_cast`, framework-required method names, and genuine template or framework false positives.

`./ao tidy` rejects a named `NOLINT` when that check is disabled for the file's `STRICT` or `RELAXED` mode.
Because it cannot suppress a diagnostic, such a directive is stale.
Remove local suppressions in the same change that disables a check or removes it from a mode.

## Repository-wide suppression governance

Classify each suppression in this order:

1. **Stale:** the configured mode cannot emit the diagnostic or current code no longer triggers it. Delete the directive without changing code.
2. **Code issue:** a local behavior-preserving edit states the contract more clearly. Fix the code and remove the directive.
3. **Aobus checker mismatch:** an `aobus-*` rule misunderstands a reusable pattern. Refine it and add an integration fixture proving both sides of the boundary.
4. **Upstream policy mismatch:** an upstream check is systematically wrong for a project pattern. Prefer the narrowest supported option; disable the check only when its whole diagnostic class conflicts with Aobus design, with a configuration test and rationale.
5. **Necessary local boundary:** an external ABI, platform API, framework macro, implementation-dependent standard-library type, or deliberate contract test requires the construct. Keep the smallest named suppression and explain a non-obvious boundary.

Cognitive-complexity analysis ignores macro expansions because callers do not own macro control flow; ordinary function bodies remain checked.
Derived-method-shadowing is disabled because LLVM `RecursiveASTVisitor` and standard range-view CRTP customization points intentionally refine a non-virtual fallback.
Neither policy should become repeated local suppressions.

Do not disable checks directory-wide or file-wide.
Do not add umbrella includes merely to satisfy include-cleaner, add global constants for one-use literals, hide a one-off C API warning behind an abstraction with no design value, or split clear local logic into many single-use functions only to reduce a metric.
Keep include cleanup separate from behavioral lint cleanup unless the task explicitly combines them.

## NOLINT cleanup playbook

Use the smallest behavior-preserving edit and rerun tidy on touched files before widening scope:

1. Delete stale suppressions first; if the line no longer warns, keep only the deletion.
2. Keep include-cleaner work separate when excluded from the task.
3. Replace a suppression with clearer local code when that preserves behavior.
4. Keep a targeted suppression when cleaner-looking code would reduce readability or obscure an external contract.

Useful patterns:

- Give protocol, binary-layout, or UI-policy literals a named `constexpr` only when the name carries domain meaning. For layout assertions, prefer a byte-count constant on the layout type.
- Mark an unused overload parameter as `Type& /*value*/` rather than suppressing `readability-named-parameter`.
- Prefer `std::from_chars` for strict whole-string unsigned parsing; it avoids C output-parameter suppressions and preserves no-leading-space behavior.
- At test C API pointer boundaries, use an existing helper such as `utility::layout::asLegacyPtr<T>(ptr)` when it directly expresses the boundary; otherwise retain a narrow suppression.
- For C structs in framework tests, prefer `std::array`, `std::to_array`, `std::span`, or a small designated-initializer helper when clearer than raw arrays or macro initializers.
- STL iterator aliases (`value_type`, `difference_type`, `reference`, `pointer`, and `iterator_category`) belong in lint configuration rather than repeated local suppressions.
- GTKmm and glibmm ownership boundaries are usually acceptable suppression sites. Do not hide them behind a helper unless local design already supports it.
- When a format literal is clearer than a named constant, keep a narrow suppression or revisit the rule.

## Include-Cleaner triage

Add the direct provider include where the symbol is used.
A public header includes providers for its public symbols; a `.cpp` includes providers for symbols used only there rather than relying on its paired header.
Use the standard header that owns a standard-library symbol.

For GTKmm, GLib, PipeWire, LLVM, and other Nix-provided libraries, inspect package headers and build configuration.
From the repository root, `nix-shell --run "pkg-config --cflags <lib>"` is useful for pkg-config packages.
On macOS, inspect the active vcpkg installation from the dependency report.
For Clang and LLVM internals, inspect the native compile actions through the lint report evidence or direct `bazel aquery` on the selected owners ([direct Bazel](bazel.md#direct-bazel-without-the-portal)).
`llvm-config --cxxflags` is also useful on Linux.

Suppress `misc-include-cleaner` only when it genuinely cannot model a provider, such as a required umbrella header or C macro.
When a required include exists solely for specialization or registration side effects, keep it and add the narrowest project-level `IgnoreHeaders` entry rather than repeated local suppressions.

## Python hygiene

Fix Ruff and mypy findings with the same bias as C++ lint: prefer a local code or typing improvement, keep public shape stable unless the task changes it, and avoid broad ignores.
Linux uses the project shell.
Windows uses the checkout-specific managed environment and never ambient `PATH` tools.
Both supported tooling environments must match exact versions in `script/ao/toolchain.json`; Nix checks this during evaluation, while Windows bootstrap and tooling tests probe the managed environment.

- Use `./ao format` for Python formatting.
- Use targeted `# noqa: RULE` or `# type: ignore[code]` only when the tool cannot express the real contract; explain a non-obvious reason.
- Do not silence mypy by widening to `Any` unless the boundary is genuinely dynamic.

## Bazel/Starlark hygiene

`./ao format` formats `.bzl`, `.bazel`, and exactly named `BUILD`/`WORKSPACE`
files with the [governed Buildifier](dependency-governance.md#repository-tooling).
`--check` never rewrites them. `./ao tidy` and `./ao hygiene`
check both formatting and lint; required product CI uses the hygiene gate. The existing changed-file, explicit-file, folder and `--all`
selectors apply, including root module/repository files and `asset/BUILD.bazel`.
`.bazelrc`, `.bazelversion`, lockfiles and templates are not Starlark inputs.
Bazel-only scopes do not prepare a C++ compiler, compile database or lint plugin.
Bazel graph files under `test/lint/` remain ordinary Buildifier inputs; the
native checker's negative-fixture exclusion does not exempt those declarations.

Checks use configuration-isolated JSON diagnostics. Warnings fail even when
Buildifier returns zero. Neither personal
`BUILDIFIER_CONFIG` nor a local config can disable the gate. Formatting enables
no automatic lint fixes; correct lint findings with explicit source edits.

The pinned upstream default warning set is retained except `unnamed-macro`.
Aobus repository graph factories deliberately own fixed imported labels rather
than per-instance names; requiring an unused `name` argument would obscure that
contract. This is a single documented policy exception, not scattered local
suppressions. Other warning categories remain enabled and returned diagnostics
are never filtered by category. C++ and Python lint policies are unchanged.

## Diagnostic-only runs

`./ao tidy` and `./ao analyze` are diagnostic-only: the portal never exports or applies replacements, even when an upstream or custom diagnostic carries hint ranges.
A shared validator in `script/ao/core/tidyconfig.py` enforces the same boundary for these commands; the report routes additionally restrict raw arguments to their documented allowlist.
The retired `--no-build` option is not accepted: actual reports require real compile actions.
Raw clang-tidy arguments that request fixes are rejected: `-fix`, `-fix-errors`, `-fix-notes`, and `-export-fixes` in their single-dash, double-dash, and `=`-joined forms.
A standalone `@responsefile` argument is rejected as well because a response file can hide those flags; the portal fails closed rather than expanding it.
The owning argument parser rejects these options before source-scope resolution, workspace setup, or native compiler-environment selection.
The launcher may still prepare its initial Nix shell or Python interpreter before parsing; rejection does not promise a bootstrap-free invocation.
Direct programmatic command calls also validate the boundary.
Compiler database arguments and `--extra-arg=@responsefile` compiler responses remain supported because they select compile state rather than clang-tidy options.

Agents make explicit, reviewable edits under the `use-clang-tidy` skill.
Prefer hand edits for ownership, public API, behavior, naming, readability tradeoffs, and framework boundaries.

## Verification

After C++ lint edits, rerun the narrowest `./ao tidy` scope covering modified C++ files.
After Python hygiene edits, rerun `./ao tidy` for modified Python and `./ao test --tooling` when tooling behavior changed.
Run focused build or test validation when a lint fix changes behavior, ownership, public API, or test semantics.

Platform-specific C++ requires its native tidy pass.
Cross-platform C++ requires affected Linux, macOS, and Windows passes because each host validates only translation units generated by its build.
Checker changes additionally follow [checker development validation](lint/checker-development.md#validation).
Use [validation and review](test/validation-and-review.md) for completion scope and result reuse.
