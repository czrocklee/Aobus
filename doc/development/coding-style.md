---
id: development.coding-style
type: development
status: current
domain: development
summary: Defines the C++ language and source-style policy for Aobus.
---
# Aobus C++ coding style

This guide defines Aobus's C++ coding conventions.
Rules are numbered for easy reference in reviews and tooling.
Detailed naming policy lives in `doc/development/naming-convention.md`.

- 1\. C++ Standard
  - 1.1. Target `C++26` without modules
- 2\. Code Style
  - 2.1. Indentation & Formatting
    - 2.1.1. Use `clang-format`
    - 2.1.2. Use blank lines before and after control blocks (`if`, `for`, `while`, `switch`), and between distinct statement groups.
      - Top-level macros like `TEST_CASE` or `SECTION` MUST be separated by at least one blank line.
      - Do not put blank lines at the start or end of a block (immediately after `{` or before `}`).
      - Exception: if the previous line is a comment written specifically for an `if`, keep the comment directly above the `if`
      - Exception: if an `if` is the first effective line in a scope, do not add a leading blank line
  - 2.2. Naming Conventions
    - 2.2.1. Detailed naming policy lives in
      `doc/development/naming-convention.md`. Use that document for type/interface
      naming, semantic vocabulary, pointer/optional naming, file names, and
      helper/support allocation.
    - 2.2.2. Common identifier shapes: types and classes use `PascalCase`;
      functions and variables use `camelCase`; non-static class data members use
      `_camelCase`; struct data members use plain `camelCase`; constants use
      `kCamelCase`.
    - 2.2.3. Test case names and Catch2 tags are covered in
      `doc/development/test/naming-and-assertion.md`.
  - 2.3. Headers
    - 2.3.1. Use `#pragma once`
  - 2.4. Includes
    - 2.4.1. Group includes with blank lines in this order:
      - the paired header for this file
      - ordinary local headers (`"..."`) and project public headers (`<ao/...>`, `<runtime/...>`, `<cli/...>`, `<app/...>`, `<fixture/...>`, `<test/...>`)
      - third-party and platform headers
      - standard library headers
    - 2.4.2. Within the ordinary local/project group, keep quoted local headers before angle-bracket project headers.
  - 2.5. Member Order
    - 2.5.1. Keep `.cpp` member definitions in the same order as the header
    - 2.5.2. Order header access sections as `public` → `protected` → `private`
  - 2.6. Namespaces
    - 2.6.1. Use nested namespace syntax: `namespace ao::core { ... }`
    - 2.6.2. Prefer anonymous namespaces to `static` for internal linkage
    - 2.6.3. Prefix external C library functions and types with `::`: `::mdb_cursor_open()`, `::pw_core_sync()`, `::snd_pcm_format_t`
      - 2.6.3.1. Keep external C vocabulary spelling when declaring or naming external API types, such as LMDB `MDB_*` forward declarations.
    - 2.6.4. For C functions and types also available in the C++ standard library (e.g., from `<cstring>`, `<cmath>`, `<cstddef>`), use the `std::` prefix: `std::memcpy()`, `std::abs()`, `std::size_t`
    - 2.6.5. Avoid redundant namespace qualification when the usage is already within that namespace (or a sub-namespace).
      - Inside `namespace ao`, use `Foo` instead of `ao::Foo`.
      - Inside `namespace ao::library`, use `library::Track` instead of `ao::library::Track` (prefer the most concise relative path).
  - 2.7. Types
    - 2.7.1. Use `std::` integer types such as `std::int32_t` and `std::uint64_t`; avoid plain `int` and `unsigned` unless matching an external API
    - 2.7.2. Prefer `std::string` to owning `char*`
    - 2.7.3. Avoid raw C arrays; use `std::array` or `std::to_array` for fixed-size buffers and API parameters
    - 2.7.4. Prefer `using` to `typedef`
  - 2.8. Casts
    - 2.8.1. Never use C-style casts (`(int)x`); use `static_cast`, `reinterpret_cast`, or `const_cast` as appropriate
    - 2.8.2. Prefer `static_cast` for numeric widening/narrowing conversions in log statements and format strings
  - 2.9. Output
    - 2.9.1. Prefer `'\n'` to `std::endl` — `std::endl` forces a flush, which is rarely desired
    - 2.9.2. Use the project logging facility (`PLAYBACK_LOG_INFO`, etc.) rather than `std::cout`/`std::cerr` for runtime diagnostics
- 3\. Modern C++ Features
  - 3.1. C++20 Features
    - 3.1.1. Use concepts: `template<typename T> requires std::integral<T>`
    - 3.1.2. Prefer `std::format` to `printf` and `sprintf`
    - 3.1.3. Use `std::span` for non-owning buffer and container views
    - 3.1.4. Use `std::ranges`
      - Use ranges when they make intent clearer, not merely shorter.
      - Prefer direct algorithms for boilerplate removal: projections with `find`/`contains`, `std::erase`, `std::erase_if`, `append_range`, and `insert_range`.
      - Simple traversal views (`reverse`, `drop(1)`, `iota`, `enumerate`) are fine when the loop body stays clear.
      - Be cautious with long pipelines (`filter | transform | to`, folds, temporary-container `join`). Prefer explicit loops for business logic, C API boundaries, side effects, allocation-heavy formatting, locks, I/O, or debugger-worthy branching.
      - Keep algorithm families consistent: if using `std::ranges::sort`, prefer `std::ranges::unique` over iterator-based versions.
      - Rule of thumb: ranges should make the intent more prominent than the technique. When in doubt, choose the boring loop.
    - 3.1.5. Use `[[no_unique_address]]` for empty-member optimization
    - 3.1.6. Use `starts_with()` and `ends_with()` for prefix and suffix checks
    - 3.1.7. Use designated initializers for structs
    - 3.1.8. Use `std::jthread` and `std::stop_token` for background threads that need cooperative cancellation
  - 3.2. C++17 Features and Attributes
    - 3.2.1. Use `std::optional` for nullable return values where absence is not an error (e.g., lookups, optional fields); do not use it to report failures — use `std::expected` instead (see 3.3.1)
      - 3.2.1.1. Optional variables, fields, and parameters use the `opt`
        prefix defined in `doc/development/naming-convention.md`. Existence checks on
        named optional variables and fields MUST use concise boolean conversion,
        such as `if (optVar)` or `if (!optVar)`. Temporary optional expressions
        may use `.has_value()` when that makes the absence check clearer, such
        as `reader.get(id).has_value()`. Do not use
        `static_cast<bool>(optional)`; use the optional directly in boolean
        contexts and `.has_value()` when materializing a `bool`.
    - 3.2.2. Use `std::variant` for type-safe unions
    - 3.2.3. Use `std::string_view` for non-owning string parameters
    - 3.2.4. Use `if constexpr` to remove compile-time branches
    - 3.2.5. Use structured bindings when they improve clarity: `for (auto& [key, value] : map)`
    - 3.2.6. Use init-statements in `if` and `switch` when they keep temporary scope local: `if (auto var = get(); condition)`
    - 3.2.7. Use `[[nodiscard]]` on RAII owner types when discarding a temporary would immediately release a resource or undo a scoped effect; do not use it on functions.
      - `aobus-modernize-nodiscard-usage` requires the attribute when both the domain name signals a lifecycle or resource owner, such as `*Session`, `*Scope`, `*Transaction`, or `*Future`, and the type structurally owns cleanup through a user-provided destructor, an owning RAII member, or an RAII base, with copy construction disabled.
      - Raw pointers and references are observers and do not establish RAII ownership. Abstract lifecycle interfaces are not annotated merely for their name; annotate the concrete owning implementation.
      - Structurally RAII types with another clear domain name may opt in. `ao::Result` is the explicit non-RAII exception because discarding it loses the recoverable failure channel.
    - 3.2.8. Do not use casts to suppress unused warnings.
      - 3.2.8.1. Never-used function parameters: use `Type /*name*/` (anonymous parameter).
      - 3.2.8.2. Conditionally-used parameters, local variables, and structured bindings: use `[[maybe_unused]]`.
      - 3.2.8.3. Deliberately discarded return values: use `std::ignore = expr;` (first-class for this purpose in C++26). Do not use void casts or introduce locals just to discard a value, and do not use `std::ignore` on plain variable reads where 3.2.8.1/3.2.8.2 apply.
  - 3.3. Modern C++ Features (C++23/26)
    - 3.3.1. Use `std::expected<T, E>` for operations that can fail recoverably
      - Use the project alias `ao::Result<T>` (defaults to `ao::Result<>` for `void`), defined in `include/ao/Error.h`
      - The error type is `ao::Error`, a struct with an error `Code` enum and a `message` string
      - Do not use `bool` return + side-channel `lastError()` for new code
      - Do not use `std::optional` to represent failure; reserve it for legitimate absence
      - Do not use `std::error_code` / `std::error_category`; the project has no cross-library error interop needs
  - 3.4. General Language Practices
    - 3.4.1. Use RAII. Prefer `std::unique_ptr` for owned resources and add a custom deleter when needed
      - In `.cpp` files, prefer `ao::utility::makeUniquePtr<::c_func>(ptr)` for local RAII without extra boilerplate
      - In headers, wrap C resources with an explicit deleter type, for example `struct PwLoopDeleter { void operator()(::pw_thread_loop* p) const noexcept { ::pw_thread_loop_destroy(p); } };`
    - 3.4.2. Do not repeat `virtual` on overridden functions; use `override`
    - 3.4.3. Prefer brace initialization in member initializer lists
      - Prefer `T() : _mem{a}` over `T() : _mem(a)`
    - 3.4.4. Mark functions `noexcept` when they cannot throw
    - 3.4.5. Initialization style
      - **Non-primitive types**: prefer `auto x = T{args};` or `auto x = T{};`.
        - The `auto` + braces style avoids narrowing and most-vexing-parse.
        - **Exception for containers**: For containers where braced initialization is ambiguous with `std::initializer_list` (e.g., `std::vector`, `std::string`), use `auto x = T(args);` unless you explicitly intend to perform list initialization.
      - **Primitive types** (`int`, `std::size_t`, `float`, etc.): use `T x = val;` (e.g., `std::size_t offset = 0;`, `auto count = 0;`). Do not use brace initialization for primitives.
      - **Strings and String Views**: Prefer standard literals over explicit construction for constants.
        - Use `using namespace std::string_literals;` and `using namespace std::string_view_literals;` (often in an anonymous namespace).
        - Prefer `auto str = "text"s;` over `auto str = std::string("text");`.
        - Prefer `auto view = "text"sv;` over `auto view = std::string_view("text");`.
      - **Enums and std::byte**: Treat as non-primitive types and use `auto x = T{...};`.
      - Interfacing with C APIs: use explicit types when an API requires a pointer to a specific C type (e.g., `unsigned int*`).
      - Exception: for null pointer initialization, use `T* ptr = nullptr;`.
    - 3.4.6. Return types: Use traditional return type syntax (`RetType FuncName(...)`) for all non-lambda functions. Avoid trailing return types.
    - 3.4.7. Lambdas: Omit the empty parameter list `()` in lambdas that take no arguments (e.g., `[] { ... }` instead of `[]() { ... }`).
- 4\. Best Practices
  - 4.1. Getters and Accessors
    - 4.1.1. Keep trivial one-line getters and setters inline in headers
  - 4.2. Class Design
    - 4.2.1. Prefer `final` on concrete classes that are not designed for inheritance
      - This applies especially to POD structs such as `TrackHeader` and `ListHeader`
      - This applies especially to concrete data types such as `Metadata` and `TrackView`
    - 4.2.2. Minimize type exposure
      - If a type is used only in one `.cpp`, keep it in that file's anonymous namespace
      - If a type must appear in a header, prefer a nested type with the narrowest possible visibility, ideally `private`
  - 4.2.3. Use the Pimpl idiom for complex implementation details
      - Forward-declare an `Impl` struct in the header: `struct Impl;`
      - Define it as `struct ClassName::Impl final { ... };` in the `.cpp` file
      - Hold via `std::unique_ptr<Impl> _impl;`
      - Do not use `shared_ptr<Impl>` to make callback-stack destruction safe or
        to pin a facade around individual method calls. Forbid synchronous owner
        destruction with a contract and share only a narrow control block when
        callbacks or tokens have a genuinely independent lifetime.
  - 4.3. Const Correctness
    - 4.3.1. Use `const` wherever possible
      - locals: `auto const result = compute();`
      - member functions: `std::size_t size() const;`
      - pointers to constant data: `char const* name;`
      - input parameters: `void addTrack(Track const& track);`
      - mandatory services: pass by reference, not smart pointer, to express non-nullability and lifetime requirements
  - 4.4. Threading
    - 4.4.1. Name all background threads using `ao::setCurrentThreadName()` for debuggability
    - 4.4.2. Use `std::jthread` with `std::stop_token` for cooperative cancellation — do not roll manual stop flags
    - 4.4.3. Access shared state through `std::mutex` + `std::scoped_lock`; prefer `std::unique_lock` only when needed for conditional unlocking
    - 4.4.4. Use `std::atomic` for simple flags and counters shared between threads; avoid `volatile`
    - 4.4.5. Do not invoke user or external callbacks while holding a state mutex; copy publication state, unlock, then call outward
    - 4.4.6. Document callback executor affinity and marshal every off-executor call before accessing confined state
    - 4.4.7. Treat cooperative cancellation as a stop request, not a join or lifetime guarantee; teardown must quiesce work before destroying its owners
    - 4.4.8. Owner-bound workers capture a borrowed owner only when teardown
      stops and joins them before releasing that owner. A callback must defer
      synchronous owner destruction unless the API explicitly documents a
      stronger reentrant lifetime model.
- 5\. Error Handling
  - 5.1. Three-Layer Policy
    - 5.1.1. **`ao::Result<T>`** (alias for `std::expected<T, ao::Error>`) — Recoverable fallible operations
      - Use when the operation can legitimately fail and the caller is expected to handle it
      - Examples: `ao::Result<> open(path)`, `ao::Result<PcmBlock> readNextBlock()`
      - The error value travels with the return — no separate `lastError()` query needed
    - 5.1.2. **Exceptions** — Invariant violations, programmer errors, third-party callback mechanisms, and rare fatal startup defects
      - Use `std::logic_error` or `ao::throwException<ao::Exception>()` according to the local pattern
      - Do not use exceptions as the ordinary public contract for recoverable core/runtime/frontend failures
      - Catch third-party exceptions at adapter boundaries when the failure should become `ao::Result<T>`
    - 5.1.3. **`std::optional<T>`** — Legitimate absence
      - Use when "not found" is a normal outcome, not an error
      - Examples: database lookups, optional UI state, finding a sink by name
    - 5.1.4. See `doc/spec/failure/outcome-channel.md` for shared channel rules and `doc/reference/failure/error.md` for the exact error surface
  - 5.2. Error Type
    - 5.2.1. Use `ao::Result<T>` (alias for `std::expected<T, ao::Error>`) as the return type; use `ao::Result<>` when `T` is `void`
      - `ao::Error` has a `Code` enum for programmatic dispatch and a `message` string for human context
      - Use `ao::makeError(code, message)` to construct error results concisely
    - 5.2.2. Return `{}` for success when the return type is `ao::Result<>` (void expected)
      - Prefer `return {};` over `return ao::Result<>();` for consistency
      - Use `return std::unexpected(ao::Error{...})` for explicit error construction
  - 5.3. Anti-Patterns
    - 5.3.1. Do not use `bool` return + `lastError()` getter for error reporting in new code
    - 5.3.2. Do not return an empty `std::string` to indicate success
    - 5.3.3. Do not use `std::optional` to signal an error — use `std::expected` and let `std::nullopt` mean "absent, not broken"
    - 5.3.4. Do not catch exceptions in low-level implementation code only to stringify them; catch at meaningful adapter boundaries and preserve error code/context when converting to `ao::Result<T>`
- 6\. Platform-Specific Code
  - 6.1. File-Level Separation
    - 6.1.1. Put platform implementations in separate files selected by CMake (`if(WIN32)`/`elseif(LINUX)`), using a platform suffix or a platform file family: `SignalExitWatcherPosix.cpp` / `SignalExitWatcherWindows.cpp`, `backend/WasapiProvider.cpp` / `backend/PipeWireProvider.cpp`.
    - 6.1.2. Small localized branches — a conditional include plus a call or two, as in `AudioBackendBootstrap.cpp` — may use preprocessor conditionals in place. Once a branch grows beyond that, split it into per-platform files.
  - 6.2. Platform Capability Macros
    - 6.2.1. Audio backend availability comes from the generated `<ao/audio/BackendConfig.h>`: test `AOBUS_HAS_WASAPI` / `AOBUS_HAS_PIPEWIRE` / `AOBUS_HAS_ALSA` with `#if` (they are always defined to `0` or `1`).
    - 6.2.2. Do not introduce new platform macros through target compile definitions; extend the generated config header instead so availability stays visible in one place.
    - 6.2.3. Raw compiler/OS macros (`_WIN32`, `__linux__`) belong only inside platform-suffixed files or the small branches allowed by 6.1.2.
