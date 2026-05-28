// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "check/OptionalNamingAndUsageCheck.h"

#include "clang/ASTMatchers/ASTMatchFinder.h"
#include "clang/Lex/Lexer.h"

#include <clang/AST/Decl.h>
#include <clang/AST/ExprCXX.h>
#include <clang/ASTMatchers/ASTMatchers.h>
#include <llvm/Support/Casting.h>

using namespace clang::ast_matchers;

namespace clang::tidy::readability
{
  void OptionalNamingAndUsageCheck::registerMatchers(MatchFinder* finder)
  {
    // Robustly match std::optional by looking for the template specialization
    auto isOptionalType = hasType(qualType(hasUnqualifiedDesugaredType(
      recordType(hasDeclaration(classTemplateSpecializationDecl(hasName("::std::optional")))))));

    // Rule 1: Match all variables and fields of type std::optional
    finder->addMatcher(declaratorDecl(isOptionalType).bind("optional_decl"), this);

    // Rule 2: Match calls to .has_value() on std::optional
    finder->addMatcher(
      cxxMemberCallExpr(on(expr(isOptionalType)), callee(cxxMethodDecl(hasName("has_value")))).bind("has_value_call"),
      this);
  }

  void OptionalNamingAndUsageCheck::check(MatchFinder::MatchResult const& result)
  {
    auto const& sm = *result.SourceManager;

    // Check Rule 1: Naming
    if (auto const* decl = result.Nodes.getNodeAs<DeclaratorDecl>("optional_decl"))
    {
      if (sm.isInSystemHeader(decl->getLocation()) || decl->getLocation().isMacroID())
      {
        return;
      }

      StringRef name = decl->getName();

      while (name.consume_front("_"))
      {
      }

      if (!name.empty() && !name.starts_with_insensitive("opt"))
      {
        diag(decl->getLocation(), "std::optional %0 %1 should start with 'opt' to indicate it is optional")
          << (llvm::isa<FieldDecl>(decl) ? "member" : "variable") << decl;
      }
    }

    // Check Rule 2: .has_value() usage
    if (auto const* call = result.Nodes.getNodeAs<CXXMemberCallExpr>("has_value_call"))
    {
      if (sm.isInSystemHeader(call->getBeginLoc()) || call->getBeginLoc().isMacroID())
      {
        return;
      }

      diag(call->getExprLoc(), "prefer concise 'if (opt)' or 'if (!opt)' over '.has_value()'");
    }
  }
} // namespace clang::tidy::readability
