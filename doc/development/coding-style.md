---
id: development.coding-style
---
# Aobus C++ coding style

This guide defines the C++ rules contributors apply while writing Aobus code.
Use `clang-format` for mechanical layout, the [naming conventions](naming-convention.md) for project vocabulary, and [test naming and assertions](test/naming-and-assertion.md) for Catch2 names and tags.
Aobus targets C++26 without modules.

## Formatting and source layout

Use blank lines before and after control blocks (`if`, `for`, `while`, and `switch`) and between distinct statement groups.
Top-level macros such as `TEST_CASE` and `SECTION` must be separated by at least one blank line.
Do not put blank lines immediately after `{` or before `}`.
Keep a comment written specifically for an `if` directly above that `if`, and do not add a leading blank line when an `if` is the first effective line in a scope.

Use `#pragma once` in headers.
Keep ordinary function and method definitions in a `.cpp` file, with these exceptions:

- Function templates, definitions in a dependent template context, `constexpr`, `consteval`, deduced-return definitions, and defaulted or deleted functions may remain visible in a header.
  A fully specialized definition that is no longer dependent follows the ordinary-function rule.
- An ordinary definition may remain in a header when its compound body is empty, including a constructor whose work is entirely in its member initializer list.
- An ordinary non-empty body may contain exactly one direct statement: a return statement, coroutine return statement, expression, or declaration statement.
- A single direct statement does not qualify when it contains a nested lambda, statement expression, or another locally defined executable body.
  Control-flow statements and nested compound statements do not qualify.
- Explicit `inline`, an accessor-shaped name, or an out-of-class definition does not create another exception.
- Two statements remain two statements when written on one line; one expression remains one statement when formatting spans lines.

The `aobus-readability-header-function-definition` check enforces this contract.
Implementation and suppression details are in [checker development](lint/checker-development.md#header-function-definitions).

Do not use an umbrella header to make unrelated fixtures, adapters, formatting support, or subsystem implementations transitively available.
Include the narrow owning header at each use site.
Keep optional adapters such as formatters and stream integration out of ubiquitous value-type headers when callers can opt in through a focused adapter header.

Group includes, separated by blank lines, in this order:

1. the paired header;
2. ordinary local headers (`"..."`) followed by project public headers (`<ao/...>`, `<runtime/...>`, `<cli/...>`, `<app/...>`, `<fixture/...>`, and `<test/...>`);
3. third-party and platform headers; and
4. standard-library headers.

Keep `.cpp` member definitions in the same order as the header.
Order header access sections `public`, `protected`, then `private`.

## Namespaces and external APIs

Use nested namespace syntax: `namespace ao::core { ... }`.
Prefer an anonymous namespace to `static` for internal linkage.
Avoid redundant qualification inside a namespace: inside `ao`, use `Foo`; inside `ao::library`, use `library::Track` rather than `ao::library::Track`.

Prefix external C functions and types with `::`, for example `::mdb_cursor_open()`, `::pw_core_sync()`, and `::snd_pcm_format_t`.
Preserve an external API's spelling when declaring or naming its types, such as LMDB `MDB_*` forward declarations.
For C functions and types also provided by the C++ standard library, use the `std::` form, such as `std::memcpy()`, `std::abs()`, and `std::size_t`.

## Types, casts, and output

- Use fixed-width `std::` integer types such as `std::int32_t` and `std::uint64_t`.
  Use plain `int` or `unsigned` only when matching an external API.
- Prefer `std::string` to owning `char*`.
- Avoid raw C arrays; use `std::array` or `std::to_array` for fixed-size buffers and API parameters.
- Prefer `using` to `typedef`.
- Never use C-style casts.
  Use `static_cast`, `reinterpret_cast`, or `const_cast` as appropriate, and prefer `static_cast` for numeric conversions in log statements and format strings.
- Prefer `'\n'` to `std::endl`; the latter forces a flush.
- Use the project logging facility, such as `PLAYBACK_LOG_INFO`, instead of `std::cout` or `std::cerr` for runtime diagnostics.

## Language features

Use modern language and library features where they make the contract clearer:

- Use concepts, for example `template<typename T> requires std::integral<T>`.
- Prefer `std::format` to `printf` and `sprintf`.
- Use `std::span` for non-owning buffer and container views.
- Use `[[no_unique_address]]` for empty-member optimization.
- Use `starts_with()` and `ends_with()` for prefix and suffix checks.
- Use designated initializers for structs.
- Use `std::variant` for type-safe unions.
- Use `std::string_view` for non-owning string parameters.
- Use `if constexpr` to remove compile-time branches.
- Use structured bindings when they improve clarity.
- Use init-statements in `if` and `switch` when they keep a temporary's scope local.

Use ranges when they make intent clearer, not merely shorter.
Prefer direct algorithms for boilerplate removal: projections with `find` or `contains`, `std::erase`, `std::erase_if`, `append_range`, and `insert_range`.
Simple traversal views such as `reverse`, `drop(1)`, `iota`, and `enumerate` are appropriate when the loop body stays clear.
Prefer explicit loops for business logic, C API boundaries, side effects, allocation-heavy formatting, locks, I/O, debugger-worthy branching, and long pipelines such as `filter | transform | to` or temporary-container `join`.
Keep algorithm families consistent: when using `std::ranges::sort`, prefer `std::ranges::unique` over an iterator-based counterpart.
When in doubt, choose the boring loop.

### Optional, result, and discarded values

Use `std::optional<T>` for legitimate absence, such as a lookup miss or an optional field, not for failure.
Optional variables, fields, and parameters use the `opt` prefix from the [naming conventions](naming-convention.md#pointer-optional-result-and-time-names).
Use concise boolean conversion for a named optional (`if (optValue)` or `if (!optValue)`).
A temporary expression may use `.has_value()` when that clarifies the test, and `.has_value()` is also the correct way to materialize a `bool`.
Do not use `static_cast<bool>(optional)`.

Use `ao::Result<T>`, the project alias for `std::expected<T, ao::Error>`, for recoverable failure; use `ao::Result<>` for `void`.
Do not introduce `bool` plus `lastError()`, `std::optional`, or `std::error_code`/`std::error_category` as a new failure channel.

Do not use casts to silence unused-value warnings:

- Make a never-used parameter anonymous: `Type /*name*/`.
- Apply `[[maybe_unused]]` to conditionally used parameters, locals, and structured bindings.
- Discard a return value deliberately with `std::ignore = expression;`.
  Do not use a void cast, create a local solely to discard a value, or apply `std::ignore` to a plain variable read where one of the first two forms applies.

### Functions and initialization

Do not repeat `virtual` on an override; write `override`.
Mark a function `noexcept` when it cannot throw.
Use traditional return syntax for every non-lambda function; do not use a trailing return type.
Omit the empty parameter list in a no-argument lambda: `[] { ... }`.
Use brace initialization in member initializer lists.

For local initialization:

- For non-primitive types, prefer `auto value = T{args};` or `auto value = T{};`.
- When a container's braces are ambiguous with `std::initializer_list`, use `auto value = T(args);` unless list initialization is intended.
- For primitive types, use `T value = initial;` or an unambiguous `auto`; do not use braces.
- For string constants, import the standard literal namespace and prefer `"text"s` or `"text"sv` to explicit construction.
- Treat enums and `std::byte` as non-primitive types and use `auto value = T{...};`.
- Use an explicit type when a C API requires a pointer to that exact C type.
- Initialize a null pointer as `T* pointer = nullptr;`.

## Ownership and class design

Prefer `final` on concrete classes not designed for inheritance, especially concrete data values and POD-like structs.
Keep a type used by one `.cpp` in that file's anonymous namespace.
When a type must appear in a header, prefer a nested type with the narrowest possible visibility.

Use RAII and prefer `std::unique_ptr` for owned resources.
In a `.cpp`, use `ao::utility::makeUniquePtr<::c_func>(ptr)` for local C-resource ownership where appropriate.
In a header, provide an explicit deleter type.
Use a reference for a required non-owning collaborator.
Use a raw pointer only for genuine optional absence, late binding, or a revocable callback target, and document its owner and invalidation boundary.

Use `[[nodiscard]]` on an RAII owner type when discarding a temporary would immediately release a resource or undo a scoped effect; do not apply it to functions.
The enforced owner vocabulary includes names such as `*Session`, `*Scope`, `*Transaction`, and `*Future` when the type structurally owns cleanup and cannot be copied.
Raw pointers and references do not prove ownership, and abstract lifecycle interfaces are not annotated merely because of their name.
A structurally RAII type with another clear domain name may opt in.
`ao::Result` is the explicit non-RAII exception because discarding it loses a recoverable failure channel.

### Composition and published lifetimes

Prefer direct values for mandatory owner-local graph members and references for mandatory borrows.
Use `std::optional<T>` when presence represents a real phase whose reset completely ends that phase.
A recoverable factory should return a move-only value when an outer `unique_ptr` would serve only ownership transfer.
Retain `unique_ptr` for PImpl, polymorphism, native ownership, stable facade identity, nonmovable storage, or another documented lifetime need.

Complete final placement before publishing references, callbacks, subscriptions, provider registrations, observers, or restored state that target an object.
A value factory may make one post-factory move into final storage; later wrapper movement does not permit moving a published facade identity.
Move-only composition roots and lifecycle facades provide `noexcept` move construction, delete move assignment unless replacement has a proved contract, and make destruction of a moved-from value inert.

Use `shared_ptr` only when an identified independent participant may legitimately retain the same state after the originating facade releases it.
Name independently retained asynchronous storage `State`, keep its shared surface narrow, and document every borrowed lifetime ceiling.
Do not use `shared_ptr<Impl>` merely to make callback-stack destruction safe or to pin a facade around method calls.
Forbid synchronous owner destruction with a contract and share only a narrow control block when callbacks or tokens have a genuinely independent lifetime.

An admission or generation token is not owner lifetime.
Retire it before cancellation, drain or join admitted work through a separate protocol, and keep the raw owner alive until settlement completes.

### PImpl

Use PImpl for complex implementation details:

```cpp
struct Impl;
std::unique_ptr<Impl> _implPtr;
```

Define the implementation as `struct ClassName::Impl final { ... };` in the `.cpp` file.
The ownership and published-lifetime rules above still apply.

## Const correctness

Use `const` wherever possible: for locals, member functions, pointers to constant data, and input references.
Pass mandatory services by reference rather than smart pointer to express non-nullability and lifetime requirements.

## Threading and callbacks

- Name every background thread with `ao::setCurrentThreadName()`.
- Use `std::jthread` and `std::stop_token` for cooperative cancellation; do not roll a manual stop flag.
- Protect shared state with `std::mutex` and `std::scoped_lock`; use `std::unique_lock` only when conditional unlocking is needed.
- Use `std::atomic` for simple shared flags and counters; do not use `volatile` for synchronization.
- Never invoke a user or external callback while holding a state mutex.
  Copy publication state, unlock, and then call outward.
- Document callback executor affinity and marshal every off-executor call before touching confined state.
- Treat a stop request as neither a join nor a lifetime guarantee.
  Teardown must quiesce work before destroying its owners.
- An owner-bound worker may capture a borrowed owner only when teardown stops and joins the worker before releasing that owner.
  A callback must defer synchronous owner destruction unless the API documents a stronger reentrant lifetime model.
- Concurrent shutdown callers share one completion boundary.
  A callback-thread initiator may avoid self-wait only when independently retained state completes quiescence after the callback returns and a later external caller can still wait for completion.

## Error handling

Choose an outcome channel by contract:

| Situation | Channel |
|---|---|
| Recoverable failure the caller handles | `ao::Result<T>` |
| Legitimate absence | `std::optional<T>` |
| Caller violation known before a call | `AO_EXPECTS` |
| Normal-return guarantee cannot be met | `AO_ENSURES` |
| Private lifecycle, state-machine, construction, or validated fact | `AO_INVARIANT`, or `AO_RT_INVARIANT` on a realtime path |
| Mandatory infrastructure can no longer report recovery truthfully | `AO_FATAL` |
| Whitelisted transport or foreign mechanism | Exception contained by its named boundary |

`ao::Error` contains a `Code` for programmatic dispatch and a contextual `message`.
Use `ao::makeError(code, message)` for concise error construction.
Return `{}` for a successful `ao::Result<>`; use `std::unexpected(ao::Error{...})` for explicit failure.

Do not use raw `gsl_Expects`, `gsl_Ensures`, `gsl_Assert`, or the C `assert` macro in production; `static_assert` remains valid.
Conditions and diagnostic arguments of AO contract macros must not perform a mutation or query required for correctness.
Perform the effect first and then check its result.
Do not use exceptions as the ordinary public contract for recoverable core, runtime, or frontend failure, or for project contract faults.
Catch third-party exceptions at the narrow adapter boundary and translate them to the declared channel while preserving code and context.
A project-private exception carrier names its exact catch owner and never escapes the public subsystem boundary.
No current site translates `std::bad_alloc` to `ResourceExhausted`.

Do not report success with an empty string, use `bool` plus `lastError()`, use `optional` for failure, or catch low-level exceptions only to stringify them.
The shared channel contract is in [outcome channels](../system/failure/outcome-channel.md); exact fatal, error, and exception surfaces are in the [fatal](../system/failure/fatal.md), [error](../system/failure/error.md), and [exception-carrier](../system/failure/exception-carriers.md) references.

## Platform-specific code

Put platform implementations in files selected by CMake, using either a platform suffix or a platform file family, for example `SignalExitWatcherPosix.cpp`, `SignalExitWatcherWindows.cpp`, `backend/WasapiProvider.cpp`, and `backend/PipeWireProvider.cpp`.
A small localized branch containing only a conditional include and a call or two may use preprocessor conditionals; split a larger branch into platform files.
Prefer platform-selected source files to generated availability macros in shared source.
Raw compiler and OS macros such as `_WIN32` and `__linux__` belong only in platform-suffixed files or those small localized branches.

## Objective-C++ boundary

Apply the C++ rules to C++ declarations and expressions in Objective-C++ files.
At the Cocoa boundary, preserve framework selectors, parameter types such as `NSInteger`, `CGFloat`, and `BOOL`, and native object initialization with `alloc`/`init` or framework factories.
Use `nil` for Objective-C object absence and `nullptr` for ordinary C++ pointers.
Do not change a protocol or superclass signature to satisfy a C++ spelling preference.

Build the AppKit boundary with ARC.
Objective-C object pointers have ARC ownership qualifiers and are not the ordinary C++ observer pointers described above.
Keep `__weak` callback targets where a strong capture would create a cycle, and load a weak target once into a strong local before multiple accesses.
A const pointer binding does not make its Cocoa object immutable.
See the [ARC specification](https://clang.llvm.org/docs/AutomaticReferenceCounting.html).

ARC does not replace explicit timer or subscription invalidation, or join-before-destruction.
Keep view access on the main thread and contain C++ exceptions at native event callbacks.
Mark unused Objective-C++ delegate parameters on their declarations, for example `- (void)runProbe:(id) [[maybe_unused]] sender`, without changing the selector.
Do not use `== YES` as a general boolean test because `BOOL` may contain another nonzero value.
Use a truth test or `!= NO`, and normalize explicitly when crossing into C++.
Native diagnostic workflows are in [Objective-C diagnostics](linting.md#objective-c-diagnostics).
