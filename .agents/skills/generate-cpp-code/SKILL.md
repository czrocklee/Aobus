---
name: generate-cpp-code
description: Mandatory C++ code generation skill for Aobus. You MUST activate this skill whenever you write, edit, refactor, or fix ANY C++ code (.cpp, .h, .hpp files). This applies to feature implementation, bug fixes, test generation, or any C++ task, regardless of whether the user explicitly asks for it.
---

# Aobus C++ Code Generation

Follow these procedures to ensure architectural consistency and adherence to Aobus's C++23 standards.

## Workflow

1.  **Reference Examples**: You MUST read the most relevant snippet references listed below BEFORE writing or modifying any code. They are organized by `CONTRIBUTING.md` rule number and show focused examples rather than complete source files. Match the patterns you see — don't invent new ones.
2.  **Context Discovery**: Read a sibling file in the target directory to match existing error handling, logging, and namespace patterns. Prefer the closest neighbor over the reference files when they differ.
3.  **Implement**: Generate code following the exact conventions observed in the snippet references and nearby project code.
4.  **Verify**: If modifying logic, identify relevant tests in `test/` and match the test style shown in the test snippets.

## Snippet References

Use `references/00-rule-index.md` to choose the smallest set of focused snippets for the current task. The index maps every `CONTRIBUTING.md` rule group to a snippet file.

- **`references/00-rule-index.md`** — coverage map from `CONTRIBUTING.md` rule numbers to snippets
- **`references/01-style-and-structure.md`** — C++23 target, formatting, naming, headers, includes, member order, namespaces
- **`references/02-types-and-modern-cpp.md`** — integer/string/buffer types, casts, output/logging, C++20/17/23 features, initialization, lambdas
- **`references/03-design-threading-and-errors.md`** — accessors, class design, Pimpl, const correctness, threading, `ao::Result`, exceptions, optional absence
- **`references/04-test-snippets.md`** — Catch2, sections, generators, matchers, FakeIt, integration-test setup

When editing tests, always read `references/04-test-snippets.md` in addition to the production-code snippets that match the changed code.

## References

- **Full Standards**: [CONTRIBUTING.md](../../../CONTRIBUTING.md) (Read this if unsure about general C++ standards)
- **Build & Test**: [README.md](../../../README.md)
