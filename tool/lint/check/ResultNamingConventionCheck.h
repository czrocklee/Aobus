// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <clang-tidy/ClangTidyCheck.h>
#include <clang-tidy/ClangTidyDiagnosticConsumer.h>
#include <clang/AST/Decl.h>
#include <clang/ASTMatchers/ASTMatchFinder.h>
#include <clang/Basic/LLVM.h>
#include <llvm/ADT/DenseMap.h>
#include <llvm/ADT/DenseSet.h>
#include <llvm/ADT/SmallVector.h>

namespace clang::tidy::readability
{
  /// Checks that Result<T> variables, fields, and parameters use 'res' or the
  /// 'Res' suffix, with the ordinary underscore prefix for class members.
  class ResultNamingConventionCheck : public ClangTidyCheck
  {
  public:
    ResultNamingConventionCheck(StringRef name, ClangTidyContext* context)
      : ClangTidyCheck{name, context}
    {
    }

    void registerMatchers(ast_matchers::MatchFinder* finder) override;
    void check(ast_matchers::MatchFinder::MatchResult const& result) override;
    void onEndOfTranslationUnit() override;

  private:
    void diagnoseDeclaration(DeclaratorDecl const& declaration);

    llvm::DenseMap<unsigned, VarDecl const*> _sourceAutoVariables;
    llvm::SmallVector<VarDecl const*> _instantiatedAutoVariables;
    llvm::DenseSet<unsigned> _diagnosedLocations;
  };
} // namespace clang::tidy::readability
