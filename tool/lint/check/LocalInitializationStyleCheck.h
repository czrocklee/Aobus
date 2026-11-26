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
  /// Checks initialization style for automatic locals, including redundant wrapping of
  /// a same-type enumerator. Only primitive rules extend to namespace-scope variables,
  /// static data members, and static locals. Diagnostics do not offer automatic fixes.
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
