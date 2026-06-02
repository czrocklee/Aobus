---
name: generate-cpp-code
description: Mandatory C++ code generation skill for Aobus. You MUST activate this skill whenever you write, edit, refactor, or fix ANY C++ code (.cpp, .h, .hpp files). This applies to feature implementation, bug fixes, test generation, or any C++ task, regardless of whether the user explicitly asks for it.
---

# Aobus C++ Code Generation

Follow these procedures to ensure architectural consistency and adherence to Aobus's C++26 standards.

## Workflow

1.  **Reference Examples**: You MUST read the most relevant snippet references listed below BEFORE writing or modifying any code. They are organized by `CONTRIBUTING.md` rule number and show focused examples rather than complete source files. Match the patterns you see — don't invent new ones.
2.  **Context Discovery**: Read a sibling file in the target directory to match existing error handling, logging, and namespace patterns. Prefer the closest neighbor over the reference files when they differ.
3.  **Error Contract Check**: When adding or changing a fallible API, read `../../../doc/design/error-handling.md` and choose `ao::Result<T>`, `std::optional<T>`, or an exception according to that design before writing code.
4.  **Implement**: Generate code following the exact conventions observed in the snippet references and nearby project code.
5.  **Defer Formatting**: Do not run `clang-format` during normal implementation, debugging, validation, or final response prep. Keep edits manually style-aware while iterating. Run formatting only when the user explicitly asks for formatting or when creating a commit.
6.  **Verify**: If modifying logic, identify relevant tests in `test/` and match the test style shown in the test snippets. Do not run clang-tidy or lint-only validation unless the user explicitly asks for linting, clang-tidy, or lint cleanup in the current session.

## Snippet References

Use `references/00-rule-index.md` to choose the smallest set of focused snippets for the current task. The index maps every `CONTRIBUTING.md` rule group to a snippet file.

- **`references/00-rule-index.md`** — coverage map from `CONTRIBUTING.md` rule numbers to snippets
- **`references/01-style-and-structure.md`** — C++26 target, formatting, naming, headers, includes, member order, namespaces
- **`references/02-types-and-modern-cpp.md`** — integer/string/buffer types, casts, output/logging, C++20/17/23 features, initialization, lambdas
- **`references/03-design-threading-and-errors.md`** — accessors, class design, Pimpl, const correctness, threading, `ao::Result`, exceptions, optional absence
- **`references/04-test-snippets.md`** — Catch2, sections, generators, matchers, FakeIt, integration-test setup

When editing tests, always read `references/04-test-snippets.md` in addition to the production-code snippets that match the changed code.

## References

- **Full Standards**: [CONTRIBUTING.md](../../../CONTRIBUTING.md) (Read this if unsure about general C++ standards)
- **Error Handling Design**: [doc/design/error-handling.md](../../../doc/design/error-handling.md) (Read this when adding or changing fallible APIs)
- **Build & Test**: [README.md](../../../README.md)
- **Type-to-Header Map**: [use-clang-tidy/references/type-to-header-map.md](../use-clang-tidy/references/type-to-header-map.md) — exact header for every type used in Aobus
