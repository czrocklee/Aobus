---
name: gen-cpp-code
description: Mandatory C++ code generation skill for Aobus. You MUST activate this skill whenever you write, edit, refactor, or fix ANY C++ code (.cpp, .h, .hpp files). This applies to feature implementation, bug fixes, test generation, or any C++ task, regardless of whether the user explicitly asks for it.
---

# Aobus C++ Code Generation

Follow these procedures to ensure architectural consistency and adherence to Aobus's C++23 standards.

## Workflow

1.  **Reference Examples**: You MUST read and analyze the most relevant representative source files listed below BEFORE writing or modifying any code. This is required to absorb naming conventions, member ordering, include grouping, error handling, and modern C++ feature usage. Match the patterns you see — don't invent new ones.
2.  **Context Discovery**: Read a sibling file in the target directory to match existing error handling, logging, and namespace patterns. Prefer the closest neighbor over the reference files when they differ.
3.  **Implement**: Generate code following the exact conventions observed in the reference files.
4.  **Verify**: If modifying logic, identify relevant tests in `test/` and match the test style shown in the reference test files.

## Representative Source Files

You MUST read the files most relevant to your task before writing code — they show the canonical patterns:

- **`include/ao/Error.h`** — result types, scoped enums, `using` aliases, designated init
- **`include/ao/Exception.h`** — `AO_THROW` macros, exception hierarchy, `override`+`noexcept`
- **`include/ao/query/Expression.h`** — `std::variant`, `std::optional` with `opt` prefix
- **`lib/audio/FormatNegotiator.cpp`** — `std::ranges::contains` + `max_element`, template + `std::forward`, designated init, anon ns
- **`lib/audio/StreamingSource.cpp`** — `std::jthread`+`stop_token`, `std::mutex`+`lock_guard`, `std::atomic`, `ao::Result<>`
- **`lib/audio/backend/PipeWireBackend.cpp`** — `::` C prefix, `extern "C"`, RAII custom deleters, `[[maybe_unused]]`, `ao::makeError()`

## Representative Test Files

You MUST read these files before writing or modifying tests to match patterns:

- **`test/unit/audio/PlayerTest.cpp`** — fakeit mocking, `SECTION()`, `Catch::Approx`
- **`test/integration/tag/TagTest.cpp`** — `GENERATE()` parameterized tests, integration I/O
- **`test/unit/audio/FormatNegotiatorTest.cpp`** — Catch2 matchers, mutable shared setup

## References

- **Full Standards**: [CONTRIBUTING.md](../../../CONTRIBUTING.md) (Read this if unsure about general C++ standards)
- **Build & Test**: [README.md](../../../README.md)
