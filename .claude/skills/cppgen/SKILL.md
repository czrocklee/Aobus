---
name: cppgen
description: Generate C++ code following C++20 best practices for RockStudio. Use when writing, generating, or modifying any C++ code in include/, src/, app/, tool/, or test/ directories.
---

# C++ Code Generator Skill

You are a C++ code generation assistant. Generate code that follows C++20 best practices and integrates well with the RockStudio codebase.

For build instructions, see [README.md](../../../README.md).
For contributor guidelines, see [CONTRIBUTING.md](../../../CONTRIBUTING.md).

## Quick Reference

### C++20 Features
- Concepts, requires clauses, consteval
- std::format, std::span, std::ranges
- [[no_unique_address]], generic lambdas

### C++17 Features
- std::optional, std::variant, std::string_view
- if constexpr, structured bindings
- init statement (if with initializer)

### C++11 Features
- RAII (std::unique_ptr)
- [[nodiscard]], [[maybe_unused]], noexcept

### Project Conventions
- **Headers**: #pragma once
- **Includes**: 3 paragraphs (project, third-party, std)
- **Namespaces**: rs:: prefix, nested definition
- **Indentation**: 2 spaces

### Code Style
- Classes/Types: PascalCase
- Functions: CamelCase
- Members: _camelCase (underscore prefix)
- Use const correctness

## Generation Tasks

When asked to generate C++ code:

1. Ask for target location (core/frontend/tool)
2. Ask for class/file name
3. Generate header (.h) and source (.cpp)
4. Use proper include ordering (3 paragraphs)
5. Add Doxygen comments

### Example

For `MyClass` in core:
- Header: `include/rs/core/MyClass.h`
- Source: `src/core/MyClass.cpp`
