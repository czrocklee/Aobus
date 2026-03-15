# RockStudio C++ Coding Guide

This guide covers C++ coding conventions for RockStudio contributors.

## C++ Standards

- **Target**: C++20 (without modules)

## Code Style

### Indentation & Formatting

- Use `clang-format` for the consistent style
- Keep blank lines before and after logical blocks

### Naming Conventions

| Type | Convention | Example |
|------|------------|---------|
| Classes/Types | PascalCase | `TrackStore`, `Metadata` |
| Functions | CamelCase | `loadMetadata()`, `getString()` |
| Variables | CamelCase | `trackCount`, `filePath` |
| Member variables | `_camelCase` | `_handle`, `_tracks` |
| Constants | `kCamelCase` | `kMaxSize`, `kDefaultFlags` |

### Headers

- Use `#pragma once` for header guards

### Includes (3 Paragraphs)

Separate includes with blank lines:

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

### Namespaces

- Use nested namespace definition: `namespace rs::core { ... }`
- Use `rs::` prefix for project code
- Prefer anonymous namespace over static functions for internal linkage
- Prefix `::` for C functions and types: `::memcpy()`, `::malloc()`

### Types

- Use `std::` for integer types: `std::int32_t`, `std::uint64_t`
- Prefer `std::string` over `char*`
- Use `std::string_view` for string parameters that don't own data

## Modern C++ Features

### C++20 Features (Preferred)

| Feature | Usage |
|---------|-------|
| Concepts | `template<typename T> requires std::integral<T>` |
| std::format | Instead of printf/sprintf |
| std::span | For array views (prefer over gsl::span) |
| std::ranges | Range algorithms and views |
| [[no_unique_address]] | Empty member optimization |
| Generic lambdas | `[]<typename T>(T&& arg) { }` |
| starts_with/ends_with | String prefix/suffix checks |

### C++17 Features (Preferred)

| Feature | Usage |
|---------|-------|
| std::optional | Functions that may or may not return a value |
| std::variant | Type-safe unions |
| std::string_view | Non-owning string parameters |
| if constexpr | Compile-time branch elimination |
| Structured bindings | `auto [key, value] : map` |
| Init statement | `if (auto&& var = get(); condition)` |

### C++11 Features (Preferred)

| Feature | Usage |
|---------|-------|
| RAII | Use `std::unique_ptr` for owned resources |
| `[[nodiscard]]` | Mark functions that must not ignore return values |
| `[[maybe_unused]]` | Suppress unused warnings |
| `noexcept` | Mark functions that won't throw |

## Best Practices

### Getters and Accessors

- Keep simple one-liner getters/setters inline in headers
- Mark getters with `[[nodiscard]]` to prevent ignored return values

### Class Design

- Use `final` on classes not designed for inheritance:
  - POD structs (e.g., `TrackHeader`, `ListHeader`)
  - Concrete data classes (e.g., `Metadata`, `TrackView`)

### Const Correctness

- Use `const` wherever possible
- Prefer `const&` for input references
