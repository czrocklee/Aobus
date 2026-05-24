---
name: develop-lint-checker
description: Guides the development of custom Clang-Tidy lint checkers for Aobus. Use this skill when asked to create, debug, or extend a lint checker.
---

# Developing Lint Checkers in Aobus

This guide provides the mandatory workflow and debugging strategies for developing custom Clang-Tidy lint checks in the Aobus codebase. Aobus relies heavily on custom AST matchers to enforce C++23 standards and Modern C++ paradigms.

## 1. Test-Driven Development (TDD) Workflow

Always prioritize integration tests. You must define the expected behavior before writing the AST Matcher.

1.  **Create the Fixture**: Add a file named `<CheckName>Fixture.cpp` in `test/integration/lint/fixture/`.
2.  **Define Expected Diagnostics**:
    -   Place `// POSITIVE` on the line *immediately preceding* the code that should trigger a warning.
    -   Use `// NEGATIVE` to mark code that looks similar but must *not* trigger the checker (avoids false positives).
3.  **Run and Fail**: Execute the integration script and verify it fails (as the check isn't built yet):
    ```bash
    nix-shell --run "./test/integration/lint/run_integration_test.sh aobus-your-check-alias"
    ```

## 2. AST Debugging & Exploration (Crucial)

Do not guess the AST structure. Always dump it.

1.  **Create a Scratch File**: Write a minimal reproducible C++ file (e.g., `scratch.cpp`) containing the pattern you want to match.
2.  **Dump the AST**:
    ```bash
    nix-shell -p clang-tools --run "clang++ -std=c++23 -fsyntax-only -Xclang -ast-dump scratch.cpp > scratch_ast.txt"
    ```
3.  **Analyze the Output**: Search the dumped text for your variables or function calls to see the exact Clang AST node names.

### 🔍 Finding Internal Clang/LLVM Headers
If you need to find the header for an AST node or a Clang-Tidy class:
- **`compile_commands.json`**: For the most accurate truth, grep `-isystem` paths in `/tmp/build/debug-clang-tidy/compile_commands.json` for a successfully compiled lint check.
- **`llvm-config`**: Use `nix-shell --run "llvm-config --cxxflags"` to see the base include paths for the current LLVM version.
- **Header Convention**: Clang headers are almost always in `<clang/AST/...>` or `<clang-tidy/...>` and follow the class name.

### ⚠️ Important AST Pitfalls in C++20 / Aobus:
-   **Niebloids (`std::ranges` algorithms)**: Standard range algorithms (like `std::ranges::find_if`) are *not* matched by standard `callExpr()`. They are function objects (Niebloids) and appear as `CXXOperatorCallExpr` with `hasOverloadedOperatorName("()")`. The 0th argument is the functor itself, so your actual arguments start at index `1`.
-   **Implicit Casts**: C++ introduces many silent AST nodes (e.g., `IntegralCast`, `LValueToRValue`, `FunctionToPointerDecay`). If your matcher fails inexplicably, liberally wrap your inner matchers in `ignoringParenImpCasts(...)`.
-   **Return Values**: When extracting `return item.id;`, the `memberExpr` will likely be wrapped in an `ImplicitCastExpr`. Always use `hasReturnValue(ignoringParenImpCasts(...))`.

## 3. Implementation Steps

1.  **Create Source Files**: Add your `MyCheck.h` and `MyCheck.cpp` in `lint/check/`.
2.  **Namespace**: Place your check in the correct namespace (usually `clang::tidy::readability` or `clang::tidy::modernize`).
3.  **Register the Check**:
    -   Include your header and register it via `checkFactories.registerCheck<MyCheck>("aobus-your-alias");` in `lint/AobusLintModule.cpp`.
    -   Add `check/MyCheck.cpp` to the `lint/CMakeLists.txt`.
4.  **Verify**: Re-run the integration test script. The test runner will automatically rebuild `libAobusLintPlugin.so`, run `clang-tidy`, apply `--fix`, and compile the fixed output to guarantee valid C++ code generation.
