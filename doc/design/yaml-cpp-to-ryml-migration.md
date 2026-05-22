# YAML Library Migration Plan (yaml-cpp to rapidyaml)

## Overview

This document outlines the migration from `yaml-cpp` to `rapidyaml` (`ryml`) across the Aobus codebase. `rapidyaml` provides significant performance improvements by parsing YAML in-situ (modifying the input buffer) and using an arena-allocated tree of indices rather than heavily allocating individual nodes like `yaml-cpp`.

This change aims to reduce memory allocations and improve parsing/emitting performance for the library manifest, application configuration, and GTK layout documents.

## Architectural Decisions

### Buffer Lifetime Management
`rapidyaml` achieves zero allocations by returning `string_view`s that point directly into the source text buffer. This requires the buffer to outlive any `c4::yml::Tree` access.
- For **LibraryImporter** and **LayoutDocument**, the tree is immediately converted into domain objects (`TrackBuilder`, `LayoutDocument`). The tree and buffer can be safely discarded once the function returns.
- For **ConfigStore**, the parsed data is currently stored as a `YAML::Node _root`. We will maintain the buffer by storing a `std::vector<char> _buffer` alongside the `c4::yml::Tree _root` in the `ConfigStore` class.

### Error Handling
`yaml-cpp` throws `YAML::Exception` on parse errors. By default, `rapidyaml` uses a callback that aborts the program. We will register a custom error handler callback using `c4::yml::set_callbacks()` that throws an `ao::Exception` internally because the callback cannot return `ao::Result<T>`.

This exception is a parser-adapter mechanism, not the public recoverable-error contract. `ConfigStore`, `LibraryImporter`, and layout loading code should catch parse exceptions at their YAML boundary and translate recoverable failures to `ao::Result<T>` or per-item import errors according to [Error Handling Model](error-handling.md).

## Implementation Steps

### 1. Build System and Environment Updates
- **`shell.nix`**: Remove `yaml-cpp` and add `rapidyaml`.
- **`cmake/Dependencies.cmake`**: Replace `find_package(yaml-cpp REQUIRED)` with `find_package(ryml REQUIRED)`. Define an interface target `PkgRapidYaml`.
- **`app/CMakeLists.txt` & `test/CMakeLists.txt`**: Update link libraries to use `PkgRapidYaml`.

### 2. Configuration Store Integration
- **`app/runtime/ConfigYamlTraits.h`**: Replace `YAML::convert<T>` specializations with `rapidyaml`'s node serialization overloads:
  ```cpp
  void write(c4::yml::NodeRef* n, T const& val);
  bool read(c4::yml::NodeRef const& n, T* val);
  ```
- **`app/runtime/ConfigStore.cpp`**: 
  - Update `ensureLoaded()` to read the file into `_buffer` and parse using `ryml::parse_in_situ`. 
  - Update `flush()` to serialize the tree to a file stream using `ryml::emitrs_yaml`.

### 3. Library Import and Export
- **`app/runtime/LibraryExporter.cpp`**: Refactor from a streaming `YAML::Emitter` approach to building a `c4::yml::Tree` in memory. `ryml` builds trees efficiently using its arena allocator. We will append nodes to the tree and finally emit the tree to the file stream.
- **`app/runtime/LibraryImporter.cpp`**: Read the YAML file into a `std::vector<char>` buffer, parse with `ryml::parse_in_situ`, and traverse the tree using `c4::yml::NodeRef`. Adapt the exception handling to use the custom `ryml` callback.

### 4. Layout Document Deserialization
- **`app/linux-gtk/layout/document/LayoutYaml.h`**: Replace `YAML::convert` specializations for `LayoutValue`, `LayoutNode`, and `LayoutDocument`.
- **`app/linux-gtk/layout/document/LayoutDocument.cpp`**: Implement the `ryml` serialization logic. `ryml` handles variants, maps, and sequences efficiently by iterating over `NodeRef` children.

### 5. Test Verification
- Update includes and fix any tests that relied directly on `yaml-cpp` behavior.
- Ensure all tests pass (`./build.sh debug`), focusing on:
  - `ao_test --rng-seed time "[layout]"`
  - `ao_test --rng-seed time "[library]"`
  - `ao_test --rng-seed time "[import_export]"`
- Enable memory sanitizers to catch any dangling `string_view` issues from `ryml`'s in-situ parsing.
