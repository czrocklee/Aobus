---
name: gen-cpp-code
description: Expert C++23 code generation and modification for Aobus. MUST be activated for ANY task that involves writing, editing, or refactoring C++ code (features, bug fixes, architecture changes). Covers all core development across headers, implementations, and gperf files.
---

# Aobus C++ Code Generation

Follow these procedures to ensure architectural consistency and adherence to Aobus's C++23 standards.

## Workflow

1.  **Reference Standards**: Open [CONTRIBUTING.md](../../../CONTRIBUTING.md) to verify naming conventions, member ordering, and feature usage before generating code.
2.  **Context Discovery**: Read a sibling file in the target directory to match existing error handling, logging, and namespace patterns.
3.  **Implement**: Generate code following the exact member order and formatting rules defined in the project guide.
4.  **Verify**: If modifying logic, identify relevant tests in `test/` or create a new test case.

## References

- **Primary Standards**: [CONTRIBUTING.md](../../../CONTRIBUTING.md)
- **Build & Test**: [README.md](../../../README.md)