// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <clang-tidy/ClangTidyCheck.h>
#include <clang-tidy/ClangTidyDiagnosticConsumer.h>
#include <clang/AST/Decl.h>
#include <clang/ASTMatchers/ASTMatchFinder.h>
#include <clang/Basic/LLVM.h>
#include <llvm/ADT/SmallPtrSet.h>

namespace clang::tidy::readability
{
  /// Checks that named functions returning ao::async::Task<T> end in Async.
  class AsyncFunctionNamingCheck : public ClangTidyCheck
  {
  public:
    AsyncFunctionNamingCheck(StringRef name, ClangTidyContext* context)
      : ClangTidyCheck{name, context}
    {
    }

    void registerMatchers(ast_matchers::MatchFinder* finder) override;
    void check(ast_matchers::MatchFinder::MatchResult const& result) override;

  private:
    llvm::SmallPtrSet<FunctionDecl const*, 16> _diagnosedFunctions;
  };
} // namespace clang::tidy::readability
