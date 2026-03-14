---
name: cppgen
description: Generate C++ code following C++20 best practices for RockStudio. Use when writing, generating, or modifying any C++ code in include/, src/, app/, tool/, or test/ directories.
---

# C++ Code Generator Skill

You are a C++ code generation assistant. Generate code that follows C++20 best practices and integrates well with the RockStudio codebase. Do NOT use C++20 modules.

## C++20 Features to Use

When appropriate, prefer these C++20 features (in addition to C++17):

- **Concepts**: Use `std::integral`, `std::ranges::range`, etc. for template constraints
- **requires clauses**: `template<typename T> requires std::integral<T>`
- **constinit**: For compile-time initialization
- **consteval**: For immediate functions (compile-time only)
- **std::format**: Instead of printf/sprintf/stream
- **std::span**: For array views (prefer over gsl::span)
- **std::ranges**: Use ranges algorithms and views when appropriate
- **Three-way comparison**: `<=>` operator with `std::compare_three_way`
- **starts_with / ends_with**: For string prefix/suffix checks
- **remove_if / erase idiom**: Use `std::erase_if` for containers
- **[[no_unique_address]]**: For empty members optimization
- **lambda captures**: Generic lambdas `[]<typename T>(T&&)` in C++20

## C++17 Features (Still Valid)

These C++17 features remain valid and preferred:

- **Structured Bindings**: `auto [key, value] : map`
- **std::optional**: For functions that may or may not return a value
- **std::variant**: Instead of union or void* for type-safe unions
- **std::string_view**: For string parameters that don't own their data
- **if constexpr**: For compile-time branch elimination
- **inline variables**: For inline static members
- **constexpr lambdas**: Compile-time computation
- **namespace attributes**: `[[nodiscard]], [[maybe_unused]]`
- **std::clamp**: For value clamping
- **std::size**: For array/container sizes

## Project Conventions

1. **Headers**: Use `#pragma once` for header guards
2. **Includes**: 3 Paragraphs seperated by blank lines
    - paired header and project local
    - third-party
    - standard library
3. **Namespaces**:
    - Use `rs::` prefix for project code
    - Use nested namespace definition
    - **Prefer anonymous namespace over static functions** for internal linkage
4. **Naming**:
    - PascalCase for class/types
    - CamelCase for functions and variables
    - Use `_` prefix for member variables
    - Prefix `::` for C function invokation
    - Use `std::` for integer types. e.g., `std::int64_t`  

5. **Memory**:
    - Use RAII for resources, prefer `std::unique_ptr` if possible 

## Code Style

- Use **2 spaces** for indentation (not tabs, not 4 spaces)
- Use `const` correctness
- Prefer `const&` for input references
- Use `noexcept` where applicable
- Prefer `std::string` over `char*`
- Use `std::span` for array views (C++20)
- Use concepts for template constraints instead of SFINAE where possible
- **Inline getters/setters**: Keep simple one-liner getters and setters inline in headers
- **Brace initialization**: Prefer `{}` uniform initialization for all occasions including constructors and initializer lists
- **Control blocks**: Use `{}` for all control blocks (if/for/while/switch/try), even single-line bodies
- **Blank lines**: Keep blank lines before and after logical blocks such as if/for/while/switch/try etc.
- **Init statements**: Use `if (init; condition)` for scoped initialization when applicable

## Generation Tasks

When asked to generate C++ code:

1. Ask for the target location (core/frontend/tool)
2. Ask for the class/file name
3. Generate appropriate header (.h) and source (.cpp) files
4. Include necessary includes with proper ordering
5. Add Doxygen comments for public APIs

## Examples

For a new class `MyClass` in core:
- Header: `include/rs/core/MyClass.h`
- Source: `src/core/MyClass.cpp`

Use proper include guards and forward declarations where needed.
