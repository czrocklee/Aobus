# Clang-Tidy Rule Coverage Plan

This document describes a phased plan for enforcing Aobus coding-guide rules with
`clang-tidy`. It covers built-in `clang-tidy` checks, the existing Aobus plugin,
new Aobus plugin checks to implement, and validation guidance.

## Goals

- Use built-in `clang-tidy` checks whenever they map cleanly to
  `CONTRIBUTING.md`.
- Keep project-specific policy in the existing `AobusLintPlugin` instead of
  ad-hoc scripts when AST or token context is needed.
- Prefer conservative diagnostics over noisy checks. A rule should become a
  build gate only after the current tree has a clean baseline or documented
  suppressions.
- Preserve the current strict/relaxed split: production targets under `lib/` and
  `app/` should stay strict; tests may disable checks that conflict with Catch2,
  FakeIt, or test-data-heavy patterns.

## Current State

`cmake/ClangTidy.cmake` already wires `clang-tidy` into CMake when
`AOBUS_ENABLE_CLANG_TIDY` is enabled. It also builds and loads
`lint/AobusLintPlugin` through `-load=$<TARGET_FILE:AobusLintPlugin>`.

The current plugin registers these checks:

| Check name | Rule coverage | Status |
| --- | --- | --- |
| `aobus-readability-control-block-spacing` | Rule 2.1.2: blank lines around control blocks, no blank lines at block start/end, spacing between `TEST_CASE`/`SECTION` | Intended to be implemented under `lint/check/`. Treat the current working-tree move/deletion state as a recovery item: there must be no stale deleted old-path files, `AobusLintModule.cpp` must include the live header, `lint/CMakeLists.txt` must compile the live source, and the plugin must build before this plan is used as a gate. After recovery, expand it for blank lines after control blocks. |
| `aobus-readability-optional-naming-and-usage` | Rule 3.2.1.1: `std::optional` variables use `opt` prefix and avoid `.has_value()` existence checks | Implemented. |
| `aobus-modernize-lambda-params` | Rule 3.4.7: omit `()` on lambdas that take no arguments | Implemented. |

The current strict check list enables `aobus-*`, `bugprone-*`, `performance-*`,
`cppcoreguidelines-*`, `readability-*`, and `portability-*`, with selected
exclusions. It does not effectively enable the built-in `modernize-*` family.
Resolve any plugin build break before changing the check set; otherwise new
diagnostics will be hidden behind a failing plugin build.

## Phase 1: Modernize Built-In Check Enablement

### Intent

Enable the `modernize-*` checks that directly enforce Aobus C++20/C++23 style
without forcing broad stylistic churn that conflicts with the coding guide.

### CMake change

Replace the current ineffective `modernize-!-trailing-return` entry with an
explicit modernize allowlist first. Avoid enabling all `modernize-*` at once until
the baseline is understood.

The allowlist entries must be comma-separated inside the same `-checks=` string
that currently starts with `-*`. Do not put them in an unrelated variable that is
not passed to `CXX_CLANG_TIDY`.

Suggested first CMake diff:

```diff
   set(_AO_CLANG_TIDY_STRICT_CHECKS
-    "-checks=-*,aobus-*,bugprone-*,performance-*,cppcoreguidelines-*,-cppcoreguidelines-avoid-magic-numbers,-cppcoreguidelines-avoid-const-or-ref-data-members,modernize-!-trailing-return,readability-*,-readability-redundant-member-init,portability-*,-clang-diagnostic-*,-bugprone-easily-swappable-parameters,-cppcoreguidelines-pro-bounds-pointer-arithmetic,-readability-convert-member-functions-to-static,-readability-static-definition-in-anonymous-namespace,-clang-diagnostic-note"
+    "-checks=-*,aobus-*,bugprone-*,performance-*,cppcoreguidelines-*,-cppcoreguidelines-avoid-magic-numbers,-cppcoreguidelines-avoid-const-or-ref-data-members,modernize-concat-nested-namespaces,modernize-use-constraints,modernize-use-designated-initializers,modernize-use-nullptr,modernize-use-override,modernize-use-ranges,modernize-use-starts-ends-with,modernize-use-std-format,modernize-use-using,modernize-use-noexcept,-modernize-use-trailing-return-type,-modernize-use-nodiscard,-modernize-use-auto,-modernize-pass-by-value,-modernize-use-emplace,-modernize-return-braced-init-list,readability-*,-readability-redundant-member-init,portability-*,-clang-diagnostic-*,-bugprone-easily-swappable-parameters,-cppcoreguidelines-pro-bounds-pointer-arithmetic,-readability-convert-member-functions-to-static,-readability-static-definition-in-anonymous-namespace,-clang-diagnostic-note"
   )
```

Recommended initial strict checks to add:

```cmake
modernize-concat-nested-namespaces,
modernize-use-constraints,
modernize-use-designated-initializers,
modernize-use-nullptr,
modernize-use-override,
modernize-use-ranges,
modernize-use-starts-ends-with,
modernize-use-std-format,
modernize-use-using,
modernize-use-noexcept
```

Checks to keep disabled or defer:

```cmake
-modernize-use-trailing-return-type,
-modernize-use-nodiscard,
-modernize-use-auto,
-modernize-pass-by-value,
-modernize-use-emplace,
-modernize-return-braced-init-list
```

Rationale:

- `modernize-use-trailing-return-type` conflicts with Rule 3.4.6.
- `modernize-use-nodiscard` conflicts with Rule 3.2.7.
- `modernize-use-auto` overlaps with Rule 3.4.5 but cannot express the Aobus
  primitive/non-primitive split by itself.
- `modernize-pass-by-value`, `modernize-use-emplace`, and
  `modernize-return-braced-init-list` are useful, but they are more likely to
  trigger non-policy churn and should be evaluated after the first baseline.

### Relaxed test behavior

Tests should inherit the same modernize allowlist, then selectively disable rules
only when a concrete test-framework conflict exists. Initial likely relaxations:

```cmake
-modernize-use-designated-initializers
```

Only add this if test fixtures produce too much churn. Otherwise keep tests close
to production style.

### Validation

1. Configure and build with `./build.sh debug --tidy`.
2. If many warnings appear, capture a baseline under `/tmp` and fix by check
   family, not by file.
3. Do not enable `AOBUS_CLANG_TIDY_FIX` globally until the diagnostic set is
   reviewed. Some modernize fix-its are safe, but others can produce broad churn.

## Phase 2: Built-In Check Mapping

These rules can be covered primarily by built-in `clang-tidy` checks.

| Coding-guide rule | Built-in checks | Plan |
| --- | --- | --- |
| 2.2.1-2.2.6 Naming conventions | `readability-identifier-naming`, existing `readability-identifier-length` | Add explicit naming configuration. Start with types, functions, constants, and private members. Struct-vs-class member naming may need plugin support. |
| 2.3.1 Use `#pragma once` | No clean built-in hard gate; `llvm-header-guard` enforces header guards rather than the project preference for `#pragma once`; `portability-avoid-pragma-once` is the opposite policy | Implement as a small Aobus source check or script-backed verifier. Do not rely on `misc-include-cleaner` for this rule because it does not enforce the pragma requirement. |
| 2.4.1 Include grouping | `llvm-include-order`, `misc-include-cleaner` as supporting signal | Evaluate `llvm-include-order` with project include categories. If it cannot express paired/project, third-party, then standard-library grouping without churn, keep grouping in a custom source check. |
| 2.6.1 Nested namespace syntax | `modernize-concat-nested-namespaces` | Enable in Phase 1. |
| 2.6.2 Anonymous namespace over `static` | `misc-use-anonymous-namespace` or `misc-use-internal-linkage` | Prefer `misc-use-anonymous-namespace` first; audit noise before gating. |
| 2.7.1 Prefer `std::` integer types and avoid plain `int`/`unsigned` | No sufficient built-in check | Use a custom Aobus check for project-owned declarations; keep external API signatures and callbacks exempt. |
| 2.7.3 Avoid raw C arrays | `cppcoreguidelines-avoid-c-arrays`, `modernize-avoid-c-arrays` | Production already gets `cppcoreguidelines-*`; keep test relaxation for framework-heavy code. |
| 2.7.4 Prefer `using` to `typedef` | `modernize-use-using` | Enable in Phase 1. |
| 2.8.1 No C-style casts | `cppcoreguidelines-pro-type-cstyle-cast` | Already covered by strict configuration. |
| 2.9.1 Prefer `\n` to `std::endl` | `performance-avoid-endl` | Already covered by strict configuration. |
| 3.1.1 Use concepts | `modernize-use-constraints` | Enable in Phase 1. |
| 3.1.2 Prefer `std::format` to `printf`/`sprintf` | `modernize-use-std-format`, `cppcoreguidelines-pro-type-vararg` | Enable `modernize-use-std-format`; keep vararg check as supporting signal. |
| 3.1.4 Use ranges | `modernize-use-ranges` | Enable in Phase 1. Audit generated include needs such as `<ranges>`. |
| 3.1.6 Use `starts_with()`/`ends_with()` | `modernize-use-starts-ends-with` | Enable in Phase 1. |
| 3.1.7 Designated initializers | `modernize-use-designated-initializers` | Enable in Phase 1; consider relaxed mode only if tests become noisy. |
| 3.2.7 Catch ignored return values | `bugprone-unused-return-value` | Already covered in strict mode; intentionally disabled in relaxed tests. |
| 3.4.2 Use `override`, do not repeat `virtual` | `modernize-use-override`, `cppcoreguidelines-explicit-virtual-functions` | Enable `modernize-use-override` explicitly. Review any conflict with interface declarations. |
| 3.4.4 Mark non-throwing functions `noexcept` | `modernize-use-noexcept`, `cppcoreguidelines-noexcept-*`, `performance-noexcept-*` | Enable `modernize-use-noexcept`; do not blindly apply fix-its to functions that may throw through callbacks or framework code. |
| 4.3.1 Local const correctness | `misc-const-correctness`, `readability-make-member-function-const`, `readability-non-const-parameter` | Add `misc-const-correctness` with conservative options; keep current readability checks. |

Supplemental style candidate:

| Style area | Built-in checks | Plan |
| --- | --- | --- |
| Braces around control statements | `readability-braces-around-statements` | This is not currently an explicit `CONTRIBUTING.md` rule. Enable only if the guide is updated to require braces; otherwise leave it out to avoid introducing a hidden rule. |

## Phase 3: Aobus Plugin Checks To Add

Use one check class per policy area. Register each check in `AobusLintModule.cpp`
with the `aobus-` prefix so it is included by the existing `aobus-*` check glob.

### `aobus-readability-identifier-naming-extensions`

Rule coverage:

- Rule 2.2.4: class data members use `_camelCase`.
- Rule 2.2.4: POD/record/`Impl`/helper struct members use `camelCase`.

Why not only built-in `readability-identifier-naming`:

- Built-in naming configuration can distinguish many declaration categories, but
  it cannot reliably encode the project distinction between class members and
  struct/POD/helper-record members without either false positives or broad
  exemptions.

Implementation guidelines:

- Match `fieldDecl()` and inspect the parent `CXXRecordDecl`.
- Use `CXXRecordDecl::isClass()` and `CXXRecordDecl::isStruct()` to distinguish
  the spelling category; both appear as `CXXRecordDecl` in AST matchers.
- Treat `class` records as requiring `_camelCase` for non-static data members.
- Treat `struct` records as requiring `camelCase` unless the struct is acting as
  a concrete class with private state; start conservative and omit uncertain
  records.
- Ignore system headers, macros, generated files, and third-party headers.
- Do not auto-fix initially; renaming fields has cross-file impact.

### `aobus-readability-member-order`

Rule coverage:

- Rule 2.5.2: access sections ordered `public`, `protected`, `private`.
- Rule 2.5.3: members ordered as nested types/using declarations, non-static
  member functions, static functions, non-static data members, static data
  members, then friends.

Implementation guidelines:

- Match `cxxRecordDecl(isDefinition())` in header files only.
- Iterate declarations in source order and assign each declaration a category.
- Report the first out-of-order declaration with a message naming the expected
  preceding category.
- Exempt Qt/GTK or macro-generated record shapes if needed, but prefer local
  `NOLINT` only for true framework constraints.
- Do not auto-fix; reordering can interact with comments and access grouping.

### `aobus-readability-c-api-global-qualification`

Rule coverage:

- Rule 2.6.3: external C library functions and types use `::` qualification.

Implementation guidelines:

- Start with known C APIs used by Aobus: LMDB `mdb_*`, FFmpeg `av_*` and
  `swr_*`, ALSA `snd_*`, PipeWire `pw_*`, and SPA `spa_*`.
- Match `declRefExpr()` and type locations for declarations whose spelling or
  canonical declaration belongs to those APIs.
- Use source text to confirm the reference was written without leading `::`.
- Emit fix-its for simple function calls and type names when the token range is
  unambiguous.
- Avoid diagnosing macro expansions and qualified member-like uses.

### `aobus-readability-std-c-library-qualification`

Rule coverage:

- Rule 2.6.4: C standard library functions/types available in C++ use `std::`.

Implementation guidelines:

- Start with a narrow allowlist: `memcpy`, `memmove`, `memcmp`, `strlen`, `abs`,
  `size_t`, integer typedefs when applicable.
- Prefer AST declaration matching over spelling-only matching to avoid catching
  project functions with the same name.
- Emit fix-its only when replacing an unqualified token with `std::token` is
  source-safe.
- Do not diagnose C library identifiers intentionally required by external C API
  signatures.

### `aobus-readability-header-style`

Rule coverage:

- Rule 2.3.1: headers use `#pragma once`.

Implementation guidelines:

- Prefer a simple source-level check over AST matching.
- Run only on project header files.
- Ignore generated headers and third-party headers.
- Diagnose headers that do not contain `#pragma once` before the first
  non-comment declaration or include.
- Do not replace existing include guards automatically in the first version;
  removal can affect comments and generated-file conventions.

### `aobus-readability-include-groups`

Rule coverage:

- Rule 2.4.1: includes are grouped as paired/project headers, third-party
  headers, then standard-library headers, with blank lines between groups.

Implementation guidelines:

- First evaluate whether `llvm-include-order` can express the desired grouping
  with project-specific categories.
- If it cannot, implement a token/source check over the leading include block.
- Classify includes by spelling: quoted project includes first, known third-party
  angle includes next, standard-library angle includes last.
- Preserve the paired-header exception for `.cpp` files.
- Do not auto-fix until the classifier is proven against the current tree.

### `aobus-readability-standard-integer-types`

Rule coverage:

- Rule 2.7.1: use `std::int32_t`, `std::uint64_t`, and related standard integer
  types; avoid plain `int` and `unsigned` unless matching an external API.

Implementation guidelines:

- Match project-owned function parameters, return types, local variables, fields,
  and aliases with builtin integer types.
- Exempt `bool`, character types, `std::size_t`, `std::ptrdiff_t`, and explicit
  external API callback/signature locations.
- Treat toolkit and C-library callback signatures as opt-out contexts by default.
- Do not auto-fix initially; choosing width and signedness is semantic.

### `aobus-modernize-member-initializer-braces`

Rule coverage:

- Rule 3.4.3: member initializer lists use braces.

Implementation guidelines:

- Match `cxxCtorInitializer()` entries written with parentheses.
- Emit a fix-it only when replacing `member(expr)` with `member{expr}` is a
  single-token delimiter change.
- Avoid initializer-list ambiguity in the first version by skipping initializers
  with multiple arguments or known initializer-list-taking types.
- This is low risk compared with local-variable initialization and can become a
  gate earlier.

### `aobus-modernize-local-initialization-style`

Rule coverage:

- Rule 3.4.5: non-primitive objects prefer `auto x = T{args}` or
  `auto x = T{}`.
- Rule 3.4.5: primitives use assignment-style initialization and do not use
  braces.
- Rule 3.4.5: C API pointer parameters may use explicit types.

Implementation guidelines:

- Match `varDecl()` for local variables only.
- Keep this check diagnostic-only for at least one release cycle before making it
  a build gate.
- Classify primitives conservatively: arithmetic types, enum types, pointer
  types, `std::byte`, character-like types such as `char8_t`, fixed-width
  typedefs, and known C/toolkit API scalar typedefs should not be forced to
  `auto x = T{}`.
- Preserve explicit pointer declarations such as `T* ptr = nullptr`.
- Do not suggest `const` for variables that are moved later; leave that to the
  const-correctness pass.
- Avoid changing vector size constructors such as `std::vector<T> values(count)`;
  braces would change semantics.
- Avoid diagnosing parenthesized construction where braces could select a
  different `std::initializer_list` overload.
- Provide fix-its only for simple local object construction where the semantic
  constructor form is unchanged.

### `aobus-modernize-forbid-trailing-return`

Rule coverage:

- Rule 3.4.6: non-lambda functions use traditional return type syntax.

Implementation guidelines:

- Match `functionDecl()` with a trailing return type location.
- Exclude lambdas and any required external API macro patterns.
- Report the function name and return type.
- Do not auto-fix initially; preserving comments, attributes, and requires
  clauses is source-sensitive.

### `aobus-modernize-forbid-nodiscard`

Rule coverage:

- Rule 3.2.7: do not use `[[nodiscard]]`; rely on `clang-tidy` ignored-return
  diagnostics instead.

Implementation guidelines:

- Match declarations with `WarnUnusedResultAttr` or C++ `[[nodiscard]]` spelling.
- Report only project files.
- Provide a removal fix-it only when the exact attribute token range is available
  and not macro-generated.

### `aobus-readability-unused-suppression-style`

Rule coverage:

- Rule 3.2.8: do not use casts to suppress unused warnings.
- Rule 3.2.8.1: never-used parameters use anonymous parameter syntax.
- Rule 3.2.8.2: conditionally-used variables use `[[maybe_unused]]`.

Implementation guidelines:

- Match casts to `void`, including `static_cast<void>(name)` and C-style
  `(void)name`.
- Report the declaration location of `name` when possible and suggest either an
  anonymous parameter or `[[maybe_unused]]` based on whether it is a parameter or
  local variable.
- Do not auto-fix by default; choosing between anonymous parameters and
  `[[maybe_unused]]` may require intent.

### `aobus-readability-project-logging`

Rule coverage:

- Rule 2.9.2: runtime diagnostics use project logging instead of
  `std::cout`/`std::cerr`.

Implementation guidelines:

- Match `declRefExpr()` or overloaded operator expressions involving
  `std::cout`, `std::cerr`, or `std::clog`.
- Exempt CLI user-facing output paths where streams are part of command output,
  not runtime diagnostics.
- Exempt tests unless the stream is clearly diagnostic noise.
- Do not auto-fix; choosing the correct logging category and level is semantic.

### `aobus-modernize-concrete-final`

Rule coverage:

- Rule 4.2.1: concrete classes and POD-like structs not designed for inheritance
  should be `final`.

Implementation guidelines:

- Match class/struct definitions without `final`.
- Skip records that have virtual functions, protected constructors/destructors,
  names starting with `I`, names ending with `Base`, or comments/attributes that
  document extension.
- Start by enforcing `final` on nested `Impl` structs and private/local concrete
  records where false positives are unlikely.
- Provide fix-its only for simple class-head locations.

### `aobus-modernize-result-error-policy`

Rule coverage:

- Rule 3.3.1 and Rule 5.3.1: new recoverable failures use `ao::Result`, not
  `bool` plus `lastError()`.
- Rule 5.3.2: do not return an empty `std::string` to indicate success.
- Rule 5.3.3: do not use `std::optional` to signal errors.
- Rule 5.3.4: low-level code should not catch exceptions only to convert them to
  error strings.

Implementation guidelines:

- Keep this check conservative and warning-only.
- Detect obvious `lastError()` side-channel patterns: classes with a `lastError`
  accessor plus `bool`-returning operations.
- Detect `std::error_code`, `std::error_category`, and new code returning
  `std::optional` from functions whose names imply failure (`open`, `load`,
  `parse`, `read`, `write`, `import`, `export`).
- Do not auto-fix; error-policy migrations affect APIs and callers.

### `aobus-threading-policy`

Rule coverage:

- Rule 4.4.1: background threads are named.
- Rule 4.4.2: prefer `std::jthread` and `std::stop_token` over manual stop flags.
- Rule 4.4.3: prefer `std::scoped_lock`, use `std::unique_lock` only when needed.
- Rule 4.4.4: avoid `volatile` for shared state.

Implementation guidelines:

- Split into sub-diagnostics internally, but keep one check name while the policy
  is evolving.
- Match `std::thread` construction and suggest `std::jthread` unless an external
  API requires `std::thread`.
- Match the combination of `std::atomic<bool>` state, thread ownership, and lack
  of `std::jthread`/`std::stop_token` first; use names such as `stop`, `stopping`,
  `shouldStop`, `quit`, `cancelRequested`, and `abortFlag` only as confidence
  boosters, not as the sole heuristic.
- Match `std::unique_lock` and suppress diagnostics when used with
  `std::condition_variable`, deferred locking, manual unlock, or lock transfer.
- Match `volatile` declarations in project files.
- Do not auto-fix; threading changes require semantic review.

## Phase 4: Checks Better Kept Outside The Plugin

Some rules are not good hard `clang-tidy` gates because they require product,
API, or architecture judgment.

| Rule | Reason |
| --- | --- |
| 2.5.1 `.cpp` definition order | A plugin can only compare definitions visible in the current translation unit against declarations available through included headers. Methods defined in a different `.cpp` are invisible, so coverage is inconsistent. Keep this as a best-effort script-backed verifier or warning-only per-TU check, never a build gate. |
| 3.1.3 `std::span` for non-owning views | Requires understanding caller/callee ownership and API stability. |
| 3.1.5 `[[no_unique_address]]` | Requires layout and ABI judgment. |
| 3.2.1 optional means absence, not failure | Plugin can flag suspicious names, but cannot reliably infer semantics. |
| 3.2.2, 3.2.3, 3.2.4, 3.2.5, 3.2.6 | Usually “prefer when clearer” rules; automated enforcement would be noisy. |
| 4.1.1 inline trivial getters/setters | Can be found mechanically, but whether to move declarations may depend on ABI and include cost. |
| 4.2.2 minimize type exposure | Architecture decision. Plugin can only find candidates. |
| 4.2.3 Pimpl for complex implementation details | Architecture decision; use review/design checks. |
| 3.4.1 full RAII policy | Built-ins and project checks can catch obvious raw ownership patterns, but converting resources to RAII and choosing `ao::utility::makeUniquePtr` often needs ownership/lifetime review. |
| 5.1 and 5.2 full error-handling policy | Plugin can catch anti-patterns, but cannot fully classify recoverable vs invariant failures. |

## Implementation Guidelines For New Plugin Checks

### Naming

- Check names must start with `aobus-` so they are included by the existing
  `aobus-*` check glob.
- Use the closest upstream category prefix:
  - `aobus-readability-*` for formatting, naming, member order, qualification,
    logging style, and source readability rules.
  - `aobus-modernize-*` for C++ feature migration and C++23 style rules.
  - `aobus-threading-*` for thread-specific policy.
- C++ class names should end with `Check`, for example
  `CApiGlobalQualificationCheck`.

### File layout

Each check should use the existing layout:

```text
lint/check/<CheckName>.h
lint/check/<CheckName>.cpp
```

Then add the files to `lint/CMakeLists.txt` and register the check in
`lint/AobusLintModule.cpp`.

### Matcher and source handling

- Prefer AST matchers for semantic rules.
- Use `Lexer`/token scanning only when the rule depends on spelling, blank lines,
  qualification tokens, or exact attribute syntax.
- Always ignore system headers and macro-generated locations unless the rule is
  explicitly intended to diagnose macro use.
- Use `SourceManager` and `CharSourceRange` carefully; only emit fix-its for
  unambiguous token ranges.

### Diagnostic policy

- Start new checks in diagnostic-only mode unless the fix is clearly mechanical.
- One diagnostic should explain the violated `CONTRIBUTING.md` rule and the
  smallest compliant change.
- Prefer suppressing uncertain cases over producing noisy diagnostics.
- Use `NOLINT(<check-name>)` only for genuine framework/API constraints or false
  positives.

### Test strategy

Add check-level tests before making a new rule a build gate. The preferred shape
is a small fixture directory plus a CTest runner that invokes `clang-tidy -load`
against the built plugin.

Recommended layout:

```text
lint/test/
  CMakeLists.txt
  run-clang-tidy-check.py
  check/
    control-block-spacing.cpp
    optional-naming-and-usage.cpp
    lambda-params.cpp
    c-api-global-qualification.cpp
```

The repository currently has scratch-style experiments such as `test_lambda.cpp`;
do not rely on root-level scratch files as the long-term harness. Move durable
fixtures under `lint/test/check/`.

Recommended CMake integration:

- Build `AobusLintPlugin` first.
- Add one `add_test(NAME aobus_lint_<check> ...)` per fixture.
- Invoke `clang-tidy` with `-load=$<TARGET_FILE:AobusLintPlugin>` and
  `-checks=-*,<check-name>`.
- Use a small Python runner to compare actual diagnostics with expectations in
  fixture comments. Prefer LLVM-style `// CHECK-MESSAGES:` and
  `// CHECK-FIXES:` markers if the runner implements them; otherwise start with
  exact diagnostic substring matching and evolve toward LLVM-compatible markers.
- Keep tests runnable through `ctest` from the normal debug build tree.

Each check should have:

- one passing example,
- one failing example,
- one macro/system-header exemption example if relevant,
- one example for each fix-it category when fix-its are supported.

If adding a dedicated clang-tidy test harness is too much for the first patch,
use targeted repository fixtures and verify with:

```bash
./build.sh debug --tidy
```

After the harness exists, add a narrower command such as:

```bash
nix-shell --run "ctest --test-dir /tmp/build/debug -R aobus_lint"
```

## Rollout Plan

### Milestone 1: Fix check configuration

- Recover the plugin baseline first: reconcile any deleted old-path
  `ControlBlockSpacingCheck.*` files with the intended `lint/check/` layout and
  confirm `AobusLintPlugin` compiles.
- Replace the current modernize placeholder with the Phase 1 allowlist.
- Add `misc-const-correctness` and `misc-use-anonymous-namespace` only after a
  local baseline confirms acceptable noise.
- Run `./build.sh debug --tidy` and save the initial warning set.

Done when the project has a documented warning baseline for the newly enabled
checks.

### Milestone 2: Naming and source-style coverage

- Configure `readability-identifier-naming` for obvious categories.
- Add `aobus-readability-identifier-naming-extensions` only for the class-vs-struct
  member gap.
- Extend `aobus-readability-control-block-spacing` for blank lines after control
  blocks after the plugin recovery task is complete.

Done when naming and spacing rules are either enforced or explicitly documented
as manual-review-only.

### Milestone 3: Qualification and initialization checks

- Implement `aobus-readability-c-api-global-qualification`.
- Implement `aobus-readability-std-c-library-qualification`.
- Implement `aobus-modernize-member-initializer-braces` with conservative
  fix-its.
- Implement `aobus-modernize-local-initialization-style` as diagnostic-only.

Done when the common FFmpeg, ALSA, PipeWire, SPA, LMDB, and standard C-library
qualification cases are covered without false positives in current code.

### Milestone 4: Class design and forbidden constructs

- Implement `aobus-modernize-forbid-trailing-return`.
- Implement `aobus-modernize-forbid-nodiscard`.
- Implement `aobus-modernize-concrete-final` for low-risk records first.
- Implement `aobus-readability-unused-suppression-style`.

Done when forbidden constructs fail tidy and concrete `Impl`/local classes get
reliable `final` diagnostics.

### Milestone 5: Semantic candidate checks

- Add warning-only `aobus-modernize-result-error-policy`.
- Add warning-only `aobus-threading-policy`.
- Keep these out of auto-fix mode permanently.

Done when these checks produce actionable review candidates without blocking
normal development on ambiguous architectural cases.

## Verification Commands

Use the repository entrypoint for final validation:

```bash
./build.sh debug --tidy
```

For a full-project rebaseline after changing check sets or plugin behavior:

```bash
./build.sh debug --clean --tidy
```

When validating a narrow rule implementation, touch representative source files
before running tidy so the build system re-analyzes them:

```bash
touch lib/audio/backend/PipeWireBackend.cpp
./build.sh debug --tidy
```

Keep long logs under `/tmp`, for example:

```bash
./build.sh debug --tidy > /tmp/aobus-clang-tidy-modernize.log 2>&1
```

## Rule Coverage Summary

| Coverage level | Rules |
| --- | --- |
| Already covered by current plugin | 2.1.2 partial, 3.2.1.1, 3.4.7 |
| Already covered or mostly covered by current built-ins | 2.7.3, 2.8.1, 2.9.1, 3.2.7, 3.4.1 partial, 3.4.2 partial, 3.4.4 partial, 4.3.1 partial |
| Cover by enabling/configuring built-ins | 2.2 partial, 2.4.1 candidate via `llvm-include-order`, 2.6.1, 2.6.2, 2.7.4, 3.1.1, 3.1.2, 3.1.4, 3.1.6, 3.1.7, 3.4.2, 3.4.4, 4.3.1 |
| Cover with new Aobus plugin checks | 2.2.4 gap, 2.3.1 if no script verifier is preferred, 2.4.1 if `llvm-include-order` is insufficient, 2.5.2, 2.5.3, 2.6.3, 2.6.4, 2.7.1, 2.9.2, 3.2.7 forbid `[[nodiscard]]`, 3.2.8, 3.3.1 anti-patterns, 3.4.3, 3.4.5 diagnostic-only, 3.4.6, 4.2.1, 4.4 candidate rules, 5.3 anti-patterns |
| Keep primarily in manual review/design review | 2.5.1, 3.1.3, 3.1.5, 3.2 semantic preference rules, 3.4.1 full RAII policy, 4.1.1, 4.2.2, 4.2.3, full 5.1/5.2 policy |
