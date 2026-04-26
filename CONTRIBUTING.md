# RockStudio C++ Coding Guide

This guide defines RockStudio's C++ coding conventions.

- 1. C++ Standard
  - 1.1. Target `C++23` without modules
- 2. Code Style
  - 2.1. Indentation & Formatting
    - 2.1.1. Use `clang-format`
    - 2.1.2. Use blank lines before and after control blocks, and between distinct statement groups
      - Example: `auto title = view.getTitle();`, then a blank line, then `if (title)`
      - Exception: if the previous line is a comment written specifically for an `if`, keep the comment directly above the `if`
      - Exception: if an `if` is the first effective line in a scope, do not add a leading blank line
  - 2.2. Naming Conventions
    - 2.2.1. Types and classes use `PascalCase`: `TrackStore`, `Metadata`
    - 2.2.2. Functions use `camelCase`: `loadMetadata()`, `getString()`
    - 2.2.3. Variables use `camelCase`: `trackCount`, `filePath`
    - 2.2.4. Non-static data members use `_camelCase`: `_handle`, `_tracks`
    - 2.2.5. Constants use `kCamelCase`: `kMaxSize`, `kDefaultFlags`
  - 2.3. Headers
    - 2.3.1. Use `#pragma once`
  - 2.4. Includes
    - 2.4.1. Group includes with blank lines in this order:
      - paired header and project headers
      - third-party headers
      - standard library headers
  - 2.5. Member Order
    - 2.5.1. Keep `.cpp` member definitions in the same order as the header
    - 2.5.2. Order header access sections as `public` → `protected` → `private`
    - 2.5.3. Within each access section, order members as:
      1. `using` declarations
      2. non-static member functions
      3. static functions
      4. non-static data members
      5. static data members
      6. `friend` declarations
  - 2.6. Namespaces
    - 2.6.1. Use nested namespace syntax: `namespace rs::core { ... }`
    - 2.6.2. Prefer anonymous namespaces to `static` for internal linkage
    - 2.6.3. Prefix C functions and C types with `::`: `::mdb_cursor_open()`, `::pw_core_sync()`, `::snd_pcm_format_t`
  - 2.7. Types
    - 2.7.1. Use `std::` integer types such as `std::int32_t` and `std::uint64_t`; avoid plain `int` and `unsigned` unless matching an external API
    - 2.7.2. Prefer `std::string` to owning `char*`
    - 2.7.3. Avoid raw C arrays; use `std::array` or `std::to_array` for fixed-size buffers and API parameters
    - 2.7.4. Prefer `using` to `typedef`
- 3. Modern C++ Features
  - 3.1. C++20 Features
    - 3.1.1. Use concepts: `template<typename T> requires std::integral<T>`
    - 3.1.2. Prefer `std::format` to `printf` and `sprintf`
    - 3.1.3. Use `std::span` for non-owning buffer and container views
    - 3.1.4. Use `std::ranges`
      - prefer algorithms over iterator-based code when they express the intent clearly
      - chain views with pipe syntax
      - prefer `std::views::` for brevity
      - `std::span` works directly with ranges algorithms
    - 3.1.5. Use `[[no_unique_address]]` for empty-member optimization
    - 3.1.6. Use `starts_with()` and `ends_with()` for prefix and suffix checks
    - 3.1.7. Use designated initializers for structs
  - 3.2. C++17 Features and Attributes
    - 3.2.1. Use `std::optional` for nullable return values
    - 3.2.2. Use `std::variant` for type-safe unions
    - 3.2.3. Use `std::string_view` for non-owning string parameters
    - 3.2.4. Use `if constexpr` to remove compile-time branches
    - 3.2.5. Use structured bindings when they improve clarity: `for (auto& [key, value] : map)`
    - 3.2.6. Use init-statements in `if` and `switch` when they keep temporary scope local: `if (auto var = get(); condition)`
    - 3.2.7. Do not use `[[nodiscard]]`; rely on `clang-tidy` to catch ignored return values
    - 3.2.8. Use `[[maybe_unused]]` for intentionally unused entities instead of warning-suppression casts
  - 3.3. General Language Practices
    - 3.3.1. Use RAII. Prefer `std::unique_ptr` for owned resources and add a custom deleter when needed
      - In `.cpp` files, prefer `rs::utility::makeUniquePtr<::c_func>(ptr)` for local RAII without extra boilerplate
      - In headers, wrap C resources with an explicit deleter type, for example `struct PwLoopDeleter { void operator()(::pw_thread_loop* p) const noexcept { ::pw_thread_loop_destroy(p); } };`
    - 3.3.2. Do not repeat `virtual` on overridden functions; use `override`
    - 3.3.3. Prefer brace initialization in member initializer lists
      - Prefer `T() : _mem{a}` over `T() : _mem(a)`
    - 3.3.4. Mark functions `noexcept` when they cannot throw
    - 3.3.5. Prefer `auto` for non-primitive object construction
      - Prefer `auto x = T{a, b};` over `T x{a, b};`
      - Prefer `auto x = T{};` over `T x;`
      - Exception: for simple null pointer initialization, use `T* ptr = nullptr;` instead of `auto* ptr = static_cast<T*>(nullptr);`
- 4. Best Practices
  - 4.1. Getters and Accessors
    - 4.1.1. Keep trivial one-line getters and setters inline in headers
  - 4.2. Class Design
    - 4.2.1. Prefer `final` on concrete classes that are not designed for inheritance
      - This applies especially to POD structs such as `TrackHeader` and `ListHeader`
      - This applies especially to concrete data types such as `Metadata` and `TrackView`
    - 4.2.2. Minimize type exposure
      - If a type is used only in one `.cpp`, keep it in that file's anonymous namespace
      - If a type must appear in a header, prefer a nested type with the narrowest possible visibility, ideally `private`
  - 4.3. Const Correctness
    - 4.3.1. Use `const` wherever possible
      - locals: `auto const result = compute();`
      - member functions: `std::size_t size() const;`
      - pointers to constant data: `char const* name;`
      - input parameters: `void addTrack(Track const& track);`
      - mandatory services: pass by reference, not smart pointer, to express non-nullability and lifetime requirements
