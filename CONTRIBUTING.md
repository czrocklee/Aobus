# RockStudio C++ Coding Guide

This guide covers C++ coding conventions for RockStudio contributors.

## 1. C++ Standards

1.1. Target: C++20 (without modules)

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

```cpp
// 1. Paired header and project local
#include "rs/core/TrackStore.h"
#include "rs/core/MusicLibrary.h"

// 2. Third-party
#include <boost/filesystem.hpp>
#include <lmdb.h>

// 3. Standard library
#include <string>
#include <vector>
```

### 2.5 Namespaces

2.5.1. Use nested namespace definition: `namespace rs::core { ... }`
2.5.2. Use `rs::` prefix for project code
2.5.3. Prefer anonymous namespace over static functions for internal linkage
2.5.4. Prefix `::` for C functions and types: `::memcpy()`, `::malloc()`

### 2.6 Types

2.6.1. Use `std::` for integer types: `std::int32_t`, `std::uint64_t`
2.6.2. Prefer `std::string` over `char*`
2.6.3. Use `std::string_view` for string parameters that don't own data

## 3. Modern C++ Features

### 3.1 C++20 Features (Preferred)

3.1.1. Concepts: `template<typename T> requires std::integral<T>`
3.1.2. std::format: Instead of printf/sprintf
3.1.3. std::span: For array views (prefer over gsl::span)
3.1.4. std::ranges: Range algorithms and views
3.1.5. [[no_unique_address]]: Empty member optimization
3.1.6. Generic lambdas: `[]<typename T>(T&& arg) { }`
3.1.7. starts_with/ends_with: String prefix/suffix checks
3.1.8. Designated initializers: For struct initialization

### 3.2 C++17 Features (Preferred)

3.2.1. std::optional: Functions that may or may not return a value
3.2.2. std::variant: Type-safe unions
3.2.3. std::string_view: Non-owning string parameters
3.2.4. if constexpr: Compile-time branch elimination
3.2.5. Structured bindings: `auto [key, value] : map`
3.2.6. Init statement: `if (auto&& var = get(); condition)`

### 3.3 C++11 Features (Preferred)

3.3.1. RAII: Use `std::unique_ptr` for owned resources
3.3.2. [[nodiscard]]: Mark functions that must not ignore return values
3.3.3. [[maybe_unused]]: Suppress unused warnings
3.3.4. noexcept: Mark functions that won't throw

## 4. Best Practices

### 4.1 Getters and Accessors

4.1.1. Keep simple one-liner getters/setters inline in headers
4.1.2. Mark getters with `[[nodiscard]]` to prevent ignored return values

### 4.2 Class Design

4.2.1. Use `final` on classes not designed for inheritance:
  - POD structs (e.g., `TrackHeader`, `ListHeader`)
  - Concrete data classes (e.g., `Metadata`, `TrackView`)

### 4.3 Const Correctness

4.3.1. Use `const` wherever possible
4.3.2. Prefer `const&` for input references
