---
name: develop-lint-checker
description: Guides the development of custom Clang-Tidy lint checkers for Aobus. Use this skill when asked to create, debug, or extend a lint checker.
---

# Developing Lint Checkers in Aobus

This skill is an explicit linting workflow. It is the exception to the repository's session-level clang-tidy opt-in rule because the user has asked to create, debug, or extend lint behavior.

This guide provides the mandatory workflow and debugging strategies for developing custom Clang-Tidy lint checks in the Aobus codebase. Aobus relies heavily on custom AST matchers to enforce C++26 standards and Modern C++ paradigms.

## Delegation Boundary

Checker behavior, AST matcher design, diagnostic policy, checker registration, and fixture design are
chair work. Lint-checker implementation is a poor fit for council review because new or registered checker
files touch CMake and the checker registry, and the council only returns advisory text. Treat checker
development as chair-owned end-to-end work.

## 1. Test-Driven Development (TDD) Workflow

Always prioritize integration tests. You must define the expected behavior before writing the AST Matcher.

1.  **Create the Fixture**: Add a file named `<CheckName>Fixture.cpp` in `test/integration/lint/fixture/`.
2.  **Define Expected Diagnostics** (markers go on the line *immediately preceding* the code):
    -   `// POSITIVE: FIX-TO: <fixed line>` — diagnostic must fire AND the auto-fix output must match.
    -   `// POSITIVE` (bare) — diagnostic must fire; no fix verification. Use for diag-only cases, e.g. a FixIt deliberately suppressed at a macro location.
    -   `// NEGATIVE` — must *not* trigger the checker (avoids false positives).
3.  **Run and Fail**: Execute the integration script and verify it fails (as the check isn't built yet):
    ```bash
    nix-shell --run "./test/integration/lint/run_integration_test.sh aobus-your-check-alias"
    ```

## Shared Helpers — Check These Before Writing Your Own

Header-only helpers live in `tool/lint/check/*.h` under `namespace clang::tidy::aobus` (no CMake changes needed). Reuse them; do not re-implement per-check copies:

- **`AstUtil.h`**: `getExprSourceText`, `stripImplicitNodes` (implicit cast/construct/materialize chains), `isInMacro` (FixIt safety), `refersToVarDecl` (canonical-decl identity), `isWithinRewrittenOperator` (C++20 rewritten comparisons), `getRangesCpoName` (ranges niebloid identification), `isEndCall` / `verifyEndObject` (algo-vs-end comparison checks).
- **`CalleeQualificationUtil.h`**: C standard library function list, callee extraction for qualification FixIts.
- **`RaiiHeuristics.h`**: RAII-type detection.

If a new helper is general (used or usable by ≥2 checks), add it to `AstUtil.h` instead of an anonymous namespace.

## Hardening Checklist for FixIt-Emitting Checks

Every check that emits a `FixItHint` must satisfy these, each locked by a fixture case:

1. **Macro guard**: reject when the replaced range is in a macro expansion (`aobus::isInMacro`) — a FixIt there edits the macro *definition*. For insertion-style fixes you may keep the diagnostic and drop only the FixIt (bare `// POSITIVE`). Fixture: a macro-spelled occurrence as `// NEGATIVE` (or bare `// POSITIVE`).
2. **Identify symbols by qualified name, never by source-text substrings**: `decl->getQualifiedNameAsString()` with exact match (`"std::remove_if"`) or prefix match (`starts_with("std::ranges::")` — exact CPO names break on implementation-detail inline namespaces like `__cpo`). Text heuristics like `calleeText.find("end")` flag `legend()`. Fixture: a same-named function in another namespace as `// NEGATIVE`.
3. **Object/container identity via decls, not text**: use `aobus::refersToVarDecl` or `equalsBoundNode` on `varDecl`. Fixture: a cross-container case (e.g. `find(v, x) != w.end()`) as `// NEGATIVE`.
4. **Cheap AST filters before `Lexer::getSourceText`**: order `check()` so name/kind/null filters run before any source-text extraction.

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
- **`getNumArgs()` counts defaulted arguments**: `std::ranges::find(v, 5)` has FOUR operator() arguments (CPO object + range + value + a `CXXDefaultArgExpr` for the defaulted projection). Exact arg-count checks silently reject everything; count only non-`CXXDefaultArgExpr` arguments.
- **C++20 rewritten comparisons double-match**: source `a != b` lowers to a `CXXRewrittenBinaryOperator` containing a *synthesized* `operator==` call. In AsIs traversal an `==` matcher also hits that inner node (source operator is actually `!=`), producing duplicate/wrong diagnostics. Guard with `aobus::isWithinRewrittenOperator`; `binaryOperation(...)` matches the rewritten node itself with the correct operator name.
- **clang 21 API gaps**: `CXXConstructorDecl::isInitListConstructor()` does not exist on the Decl (Sema-only API) — check the first parameter type for `std::initializer_list` manually. `context.getParents(...)` needs `#include <clang/AST/ParentMapContext.h>`.

## 3. Implementation Steps

1.  **Create Source Files**: Add your `MyCheck.h` and `MyCheck.cpp` in `tool/lint/check/`.
2.  **Namespace**: Place your check in the correct namespace (usually `clang::tidy::readability` or `clang::tidy::modernize`).
3.  **Register the Check**:
    -   Include your header and register it via `checkFactories.registerCheck<MyCheck>("aobus-your-alias");` in `tool/lint/AobusLintModule.cpp`.
    -   Add `check/MyCheck.cpp` to the `tool/lint/CMakeLists.txt`.
4.  **Verify**: Re-run the integration test script. The test runner will automatically rebuild `libAobusLintPlugin.so`, run `clang-tidy`, apply `--fix`, and compile the fixed output to guarantee valid C++ code generation.
