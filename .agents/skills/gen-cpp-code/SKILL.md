---
name: gen-cpp-code
description: Expert C++23 code generation for Aobus. TRIGGER when: task involves writing or modifying C++ code (features, bug fixes, refactoring, implementing plans); working in .cpp/.h/.hpp files. SKIP: build scripts, CMake config, markdown/docs, non-C++ files.
---

# Aobus C++ Code Generation

Follow these procedures to ensure architectural consistency and adherence to Aobus's C++23 standards.

## Workflow

1.  **Reference Examples**: Open the representative source files listed below to absorb naming conventions, member ordering, include grouping, error handling, and modern C++ feature usage before generating code. Match the patterns you see — don't invent new ones.
2.  **Context Discovery**: Read a sibling file in the target directory to match existing error handling, logging, and namespace patterns. Prefer the closest neighbor over the reference files when they differ.
3.  **Implement**: Generate code following the exact conventions observed in the reference files.
4.  **Verify**: If modifying logic, identify relevant tests in `test/` and match the test style shown in the reference test files.

## Representative Source Files

Read the files most relevant to your task before writing code — they show the canonical patterns:

- **`include/ao/Error.h`** — result types, scoped enums, `using` aliases, designated init
- **`include/ao/Exception.h`** — `AO_THROW` macros, exception hierarchy, `override`+`noexcept`
- **`include/ao/query/Expression.h`** — `std::variant`, `std::optional` with `opt` prefix
- **`lib/audio/FormatNegotiator.cpp`** — `std::ranges::contains` + `max_element`, template + `std::forward`, designated init, anon ns
- **`lib/audio/StreamingSource.cpp`** — `std::jthread`+`stop_token`, `std::mutex`+`lock_guard`, `std::atomic`, `ao::Result<>`
- **`lib/audio/backend/PipeWireBackend.cpp`** — `::` C prefix, `extern "C"`, RAII custom deleters, `[[maybe_unused]]`, `ao::makeError()`

## Representative Test Files

Match these patterns when writing or modifying tests:

- **`test/unit/audio/PlayerTest.cpp`** — fakeit mocking, `SECTION()`, `Catch::Approx`
- **`test/integration/tag/TagTest.cpp`** — `GENERATE()` parameterized tests, integration I/O
- **`test/unit/audio/FormatNegotiatorTest.cpp`** — Catch2 matchers, mutable shared setup

## References

- **Full Standards**: [CONTRIBUTING.md](../../../CONTRIBUTING.md)
- **Build & Test**: [README.md](../../../README.md)