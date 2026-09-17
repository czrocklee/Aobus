---
id: development.lint.naming-checks
---
# Naming checks

This reference serves maintainers of naming audits and contributors diagnosing a naming diagnostic.
The [naming conventions](../naming-convention.md) remain the authority for semantic vocabulary and role choice.
This page records only what repository automation proves and where review must take over.

## Enforcement

Naming rules have four enforcement levels.

**1. `./ao name-audit`** enforces mechanical file and placement rules:

- Banned catch-all file name suffixes: `Utils`, `Util`, `Utility`, `Types`.
- Singular `*Helper` file names are banned; plural support collections belong
  only in tests, tools, or implementation-detail areas.
- Layer placement for role suffixes: `ViewModel`, `Service`, `Component`, `Dialog`, `Widget`, `Panel`, `Controller`, `Coordinator`, `Host`, `Bridge`.
- `Fake*`, `Mock*`, `Spy*`, and `Stub*` types must live under `test/`.
- `*TestAccess` definitions are banned; use public behavior, constructor
  injection, or a production collaboration seam.

**2. Project clang-tidy checks** enforce type-aware identifier rules:

- `IdentifierNamingExtensionsCheck`: class members use `_camelCase`; passive
  struct fields use `camelCase`.
- `PointerNamingConventionCheck`: pointer-like variables use the `Ptr`
  rules in the [naming guide](../naming-convention.md#pointer-optional-result-and-time-names).
- `OptionalNamingAndUsageCheck`: optional values use the `opt` rules in the
  [naming guide](../naming-convention.md#pointer-optional-result-and-time-names).
- `ChronoNamingConventionCheck`: chrono values use unit-free time nouns.
- `ResultNamingConventionCheck`: `ao::Result` variables, parameters, and fields
  use `res` or a descriptive `*Res` name.
- `AsyncFunctionNamingCheck`: named `ao::async::Task`-returning functions end
  in `Async`.
- `BoolFunctionNamingCheck`: named source-fixed direct-`bool` functions use
  predicate, `try*` action, or the exact bool conversion vocabulary in the
  [naming guide](../naming-convention.md#function-names).

For the exact command aliases, use the registrations in [`AobusLintModule.cpp`](../../../tool/lint/AobusLintModule.cpp), not a second hand-maintained mapping here.

**3. Built-in `readability-identifier-naming`** enforces the ordinary cases:

- `PascalCase` for types, enums, scoped enum values, aliases, and concepts;
- `kCamelCase` for constexpr variables and namespace, static, or class constants;
- `camelBack` for functions, methods, parameters, and locals; and
- an underscore prefix for private and protected data members.

The built-in casing check permits GTK binding spellings such as `property_*`,
`signal_*`, `vfunc_*`, and `on_*`.
That spelling allowance does not create a semantic bool or Task exception without the framework proof below.

**4. Review** owns semantic role choice and vocabulary.
Do not turn semantic inference into a regex with exception churn.

## Semantic function and value naming

The Task and Result checks use declaration and canonical type identity through aliases, deduced types, and supported templates; they never classify a type by a source-text substring.
The bool check requires a source-fixed direct-bool contract rather than an incidental instantiation result.
All three checks are diagnostic-only because a correct rename must update the complete declaration and consumer set and may require semantic judgment.
Keep the repository at zero findings after a migration; do not add a legacy baseline or per-file exception list.

### Bool function proof

The checker applies to project-owned named functions whose direct result is fixed as `bool` by source.
It does not unwrap `Task<bool>` or `Result<bool>`, govern lambda call operators, impose a reverse type rule on a predicate-shaped function returning another type, or treat a `bool&` as a direct value.

The checker recognizes aliases, trailing bool returns, explicit bool function or class templates, and non-template deduced bool.
A deduced `auto` template is checked only when every source value return is proved bool by either a bool-typed source expression or a qualified source function lookup whose complete candidate set promises bool.
The bounded proof:

- excludes nested lambda and local-class bodies;
- examines both `if constexpr` branches;
- does not infer dependent operators, arbitrary callable results, or unresolved `decltype(auto)` returns; and
- does not infer a bool obligation for generic `T`, dependent payload aliases, or `auto` copies and forwarders merely because one or every observed instantiation returns bool.

Unknown dependent return shapes remain subject to semantic review, not automatic proof.
Source templates and redeclarations receive one diagnostic.
An independently written explicit bool specialization retains its own contract and diagnostic.

Predicate and `try` prefixes accept an initial capital, such as `IsReady` and `HasItems`, with the same word boundary; this does not accept arbitrary case-insensitive spelling such as `Isready`.
Built-in casing still applies to project-owned names.
Only exact `asBool` and `readBoolOr` are value-conversion exceptions, not broad `as*` or `read*` families.

Framework preservation requires identity, not resemblance.
The checker preserves:

- a name required by an actual foreign virtual override;
- exact C++/WinRT `IIterator` or `IVectorView` bool members proved from the `winrt::implements` interface list; and
- exact Clang `RecursiveASTVisitor` customization members whose name and signature exist on the visitor base: `Visit*`, `Traverse*`, `WalkUpFrom*`, `dataTraverseStmtPre`, and `dataTraverseStmtPost`.

A prefix, capitalization, generated-header ancestor, framework-derived owner, or project-owned override chain does not prove an exception.
Rename a project-owned chain in full.
When a non-virtual generated API such as a C++/WinRT IDL property cannot be proved reliably from the AST, retain its generated spelling with a narrow `NOLINTNEXTLINE` for the applicable naming check and an adjacent explanation of the actual generated boundary.

### Async function proof

The checker follows a declared `ao::async::Task<T>` return through aliases, deduced `auto`, and template specializations.
It covers coroutine bodies, ordinary forwarders, and infrastructure operations.
A resolved template instantiation maps to one diagnostic on its source template declaration.

When one shared deduced template produces both Task and non-Task specializations, the diagnostic remains intentional.
Review must choose a truthful shared name or split the contracts rather than exempting all deduced templates.
The rule does not apply merely because a function takes a task, nor to lambda call operators or launchers returning `void`, `TaskHandle`, or `Future`.
Names fixed by the proved framework exception classes in the bool section remain unchanged.

### Result value proof

Explicit dependent `ao::Result<T>` declarations, including references, are checked at template definition time.
Deduced Result locals in templates and generic lambdas require source evidence: a Result-typed initializer, or a resolved function named in the source lookup whose source return type promises Result.
Instantiations map to one source diagnostic.

An unconstrained `T`, its `auto` copies, and calls through arbitrary callable parameters do not acquire a Result naming requirement merely because a caller instantiates them with Result.
Other unresolved dependent initializer shapes remain outside bounded proof; they are not blanket template exemptions.

### Pointer, optional, chrono, and record proof

`PointerNamingConventionCheck` requires `Ptr` on recognized owning or counted pointer-like types and reserves that suffix from raw pointers. It also rejects the `pX`/`_pX` Hungarian prefix on both categories. References to a recognized managed pointer retain the managed-pointer rule; references to raw pointers retain the raw-pointer rule. Spans, iterators, callbacks, arbitrary wrappers, and Objective-C object pointers remain outside this C++ ownership spelling.

`OptionalNamingAndUsageCheck` applies `opt` to optional values and checks the named-value boolean-use forms owned by the coding style.
It does not reinterpret nullable pointers or expected values as optionals.
`ChronoNamingConventionCheck` distinguishes chrono durations and time points from numeric quantities whose names carry a unit.
`IdentifierNamingExtensionsCheck` uses record semantics to distinguish underscored class members from passive struct fields rather than applying one textual field pattern to both.

## Suppression boundary

Naming checks are diagnostic-only.
Use an explained, check-specific source-local suppression only for a real external or generated boundary that the checker cannot prove.
A broad prefix, owner directory, file name, capitalization, or observed template instantiation is not evidence.
Do not add project allowlists for migrated code or suppress a semantic disagreement that review should resolve.
The general [suppression policy](../linting.md#suppressions) still applies.

## Maintenance evidence

The implementation classes live in `tool/lint/check/` and are registered in `tool/lint/AobusLintModule.cpp` and `tool/lint/CMakeLists.txt`.
Their fixtures live under `test/integration/lint/fixture/<check-alias>/`.
Use the [checker development workflow](checker-development.md#custom-checker-development) for fixture markers, AST investigation, and validation.
