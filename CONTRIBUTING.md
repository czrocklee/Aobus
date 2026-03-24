# RockStudio C++ Coding Guide

This guide covers C++ coding conventions for RockStudio contributors.

## 1. C++ Standards

1.1. Target: C++23 (without modules)

## 2. Code Style

### 2.1 Indentation & Formatting

2.1.1. Use `clang-format` for consistent style
2.1.2. Keep blank lines before and after logical blocks

### 2.2 Naming Conventions

2.2.1. Classes/Types: PascalCase - `TrackStore`, `Metadata`
2.2.2. Functions: CamelCase - `loadMetadata()`, `getString()`
2.2.3. Variables: CamelCase - `trackCount`, `filePath`
2.2.4. Member variables: `_camelCase` - `_handle`, `_tracks`
2.2.5. Constants: `kCamelCase` - `kMaxSize`, `kDefaultFlags`

### 2.3 Headers

2.3.1. Use `#pragma once` for header guards

### 2.4 Includes

2.4.1. Separate includes with blank lines in order:
  - Paired header and project local
  - Third-party
  - Standard library

### 2.5 Namespaces

2.5.1. Use nested namespace definition: `namespace rs::core { ... }`
2.5.2. Use `rs::` prefix for project code
2.5.3. Prefer anonymous namespace over static functions for internal linkage
2.5.4. Prefix `::` for C functions and types: `::memcpy()`, `::malloc()`

### 2.6 Types

2.6.1. Use `std::` for integer types: `std::int32_t`, `std::uint64_t`, avoid `int`, `unsigned`. 
2.6.2. Prefer `std::string` over `char*`
2.6.3. Use `std::string_view` for string parameters that don't own data

## 3. Modern C++ Features

### 3.1 C++20 Features

3.1.1. Use Concepts: `template<typename T> requires std::integral<T>`
3.1.2. Use std::format: Instead of printf/sprintf
3.1.3. Use std::span: For container views and data buffer views
3.1.4. Use std::ranges: Range algorithms and views
3.1.5. Use [[no_unique_address]]: Empty member optimization
3.1.7. Use starts_with/ends_with: String prefix/suffix checks
3.1.8. Use designated initializers: For struct initialization

### 3.2 C++17 Features

3.2.1. Use std::optional: Functions that may or may not return a value
3.2.2. Use std::variant: Type-safe unions
3.2.3. Use std::string_view: Non-owning string parameters
3.2.4. Use if constexpr: Compile-time branch elimination
3.2.5. Use structured bindings: `auto [key, value] : map`
3.2.6. Use init statement: `if (auto var = get(); condition)`

### 3.3 C++11 Features

3.3.1. Use RAII: dopts `std::unique_ptr` for owned resources, use custom deleter if needed
3.3.2. DON'T use [[nodiscard]]: Too verbose, rely on clang-tidy for check
3.3.3. Use [[maybe_unused]]: Suppress unused warnings other than (void)
3.3.4. Use noexcept: Mark functions that won't throw
3.3.5. Use uniform initialization `{}` other than parentheses `()` for constructors and member initializer lists
3.3.6. Prefer `auto x = T{a, b};` over `T x{a, b};` for object construction

## 4. Best Practices

### 4.1 Getters and Accessors

4.1.1. Keep simple one-liner getters/setters inline in headers

### 4.2 Class Design

4.2.1. Use `final` on classes not designed for inheritance:
  - POD structs (e.g., `TrackHeader`, `ListHeader`)
  - Concrete data classes (e.g., `Metadata`, `TrackView`)

### 4.3 Const Correctness

4.3.1. Use `const` wherever possible
4.3.2. Prefer `&const` for input references
