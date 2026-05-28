---
name: develop-lint-checker
description: Guides the development of custom Clang-Tidy lint checkers for Aobus. Use this skill when asked to create, debug, or extend a lint checker.
---

# Developing Lint Checkers in Aobus

This guide provides the mandatory workflow and debugging strategies for developing custom Clang-Tidy lint checks in the Aobus codebase. Aobus relies heavily on custom AST matchers to enforce C++26 standards and Modern C++ paradigms.

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

## 2. AST Debugging — MANDATORY: `clang-query` First

**Never implement `check()` before the matcher is confirmed in `clang-query`.** Guessing AST structure is the #1 cause of "compiles but silently doesn't fire."

### Workflow

1. Create a scratch file with the target pattern.
2. Write the query in a file (`clang-query` only parses single-line `match` interactively):
    ```bash
    cat > /tmp/query.txt << 'EOF'
    enable output dump
    match cxxMemberCallExpr(callee(cxxMethodDecl(hasName("erase")))).bind("root")
    EOF
    ```
3. Add one clause at a time, re-run after each change:
    ```bash
    nix-shell -p clang-tools --run "clang-query -p /tmp/build/debug-clang-tidy -f /tmp/query.txt /tmp/scratch.cpp"
    ```
4. Also test against your fixture file to catch include path differences.
5. Only write C++ once the full matcher hits in `clang-query`.

### Raw AST Dump (supplement)

```bash
nix-shell -p clang-tools --run "clang++ -std=c++26 -fsyntax-only -Xclang -ast-dump scratch.cpp" 2>&1 > /tmp/ast.txt
```

### ⚠️ Common Pitfalls

- **`ignoringParenImpCasts` only strips `ImplicitCastExpr`**: Arguments are often wrapped in deeper chains (`ImplicitCastExpr → CXXConstructExpr → MaterializeTemporaryExpr → ...`). Use `hasDescendant(...)` in the matcher, or manually strip these nodes in `check()`.
- **Bind type MUST match GetNodeAs type**: `.bind("x")` on `varDecl()` means `getNodeAs<VarDecl>("x")`. Writing `getNodeAs<DeclRefExpr>("x")` silently returns null — the #1 reason "matcher works in clang-query but check doesn't fire."
- **`equalsBoundNode` on `varDecl`, not `declRefExpr`**: Two `DeclRefExpr` nodes are different AST nodes even if they reference the same variable. Bind `varDecl` for container identity checks.
- **Niebloids**: `std::ranges` algorithms are function objects, matched via `CXXOperatorCallExpr` + `hasOverloadedOperatorName("()")`. Arguments start at index 1.

## 3. Implementation Steps

1.  **Create Source Files**: Add your `MyCheck.h` and `MyCheck.cpp` in `lint/check/`.
2.  **Namespace**: Place your check in the correct namespace (usually `clang::tidy::readability` or `clang::tidy::modernize`).
3.  **Register the Check**:
    -   Include your header and register it via `checkFactories.registerCheck<MyCheck>("aobus-your-alias");` in `lint/AobusLintModule.cpp`.
    -   Add `check/MyCheck.cpp` to the `lint/CMakeLists.txt`.
4.  **Verify**: Re-run the integration test script. The test runner will automatically rebuild `libAobusLintPlugin.so`, run `clang-tidy`, apply `--fix`, and compile the fixed output to guarantee valid C++ code generation.
