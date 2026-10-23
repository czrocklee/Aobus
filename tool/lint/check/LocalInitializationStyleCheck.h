// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <clang-tidy/ClangTidyCheck.h>
#include <clang-tidy/ClangTidyDiagnosticConsumer.h>
#include <clang/AST/Decl.h>
#include <clang/AST/Expr.h>
#include <clang/ASTMatchers/ASTMatchFinder.h>
#include <clang/Basic/LLVM.h>

namespace clang::tidy::readability
{
  /// Enforces the local-initialization style: non-primitive locals use auto x = Type{args},
  /// primitives use T x = val (no braces). Namespace-scope, static member, and static
  /// local primitives follow the same rule. Diagnostics do not offer automatic fixes.
  class LocalInitializationStyleCheck : public ClangTidyCheck
  {
  public:
    LocalInitializationStyleCheck(StringRef name, ClangTidyContext* context)
      : ClangTidyCheck{name, context}
    {
    }

    void registerMatchers(ast_matchers::MatchFinder* finder) override;
    void check(ast_matchers::MatchFinder::MatchResult const& result) override;

  private:
    void checkStaticPrimitive(VarDecl const* var, Expr const* init);
    void diagnosePrimitiveAutoType(VarDecl const* var, Expr const* init);
  };
} // namespace clang::tidy::readability
