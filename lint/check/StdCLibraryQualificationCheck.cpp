// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "check/StdCLibraryQualificationCheck.h"

#include "check/CalleeQualificationUtil.h"

#include <clang/AST/ASTContext.h>
#include <clang/AST/Decl.h>
#include <clang/AST/Expr.h>
#include <clang/ASTMatchers/ASTMatchFinder.h>
#include <clang/ASTMatchers/ASTMatchers.h>
#include <clang/Basic/Diagnostic.h>
#include <clang/Basic/LLVM.h>
#include <clang/Basic/SourceLocation.h>

using namespace clang::ast_matchers;

namespace clang::tidy::readability
{
  void StdCLibraryQualificationCheck::registerMatchers(MatchFinder* finder)
  {
    finder->addMatcher(callExpr(callee(functionDecl(isExternC(),
                                                    hasAnyName("memcpy",
                                                               "memmove",
                                                               "memcmp",
                                                               "memset",
                                                               "strlen",
                                                               "strcmp",
                                                               "strncmp",
                                                               "strcpy",
                                                               "strncpy",
                                                               "strcat",
                                                               "strncat",
                                                               "strchr",
                                                               "strrchr",
                                                               "strstr",
                                                               "abs",
                                                               "fabs",
                                                               "malloc",
                                                               "free",
                                                               "isalpha",
                                                               "isdigit",
                                                               "tolower",
                                                               "toupper",
                                                               "sin",
                                                               "cos",
                                                               "sqrt",
                                                               "pow"))
                                         .bind("func")))
                         .bind("call"),
                       this);
  }

  void StdCLibraryQualificationCheck::check(MatchFinder::MatchResult const& result)
  {
    auto const& sm = *result.SourceManager;
    auto const* func = result.Nodes.getNodeAs<FunctionDecl>("func");
    auto const* call = result.Nodes.getNodeAs<CallExpr>("call");

    if ((func == nullptr) || (call == nullptr))
    {
      return;
    }

    if (SourceLocation const loc = call->getBeginLoc(); loc.isInvalid() || loc.isMacroID() || sm.isInSystemHeader(loc))
    {
      return;
    }

    // Skip functions defined in the main file (project-local extern "C" wrappers).
    // Everything else — system headers, third-party libs, project headers — gets flagged.
    if (sm.isInMainFile(func->getLocation()))
    {
      return;
    }

    // Full name check (the matcher only lists the most common ones for performance)
    if (!aobus::isCStandardLibraryFunction(func->getName()))
    {
      return;
    }

    auto const optCallee = aobus::getCalleeForQualification(*call, sm, result.Context->getLangOpts());

    if (!optCallee)
    {
      return;
    }

    if (optCallee->text.starts_with("std::"))
    {
      return;
    }

    auto diagBuilder = diag(optCallee->loc, "C standard library function '%0' should use 'std::%0' via <c...> header")
                       << func->getName();

    if (optCallee->text.starts_with("::"))
    {
      diagBuilder << FixItHint::CreateReplacement(
        CharSourceRange::getCharRange(optCallee->loc, optCallee->loc.getLocWithOffset(2)), "std::");
    }
    else
    {
      diagBuilder << FixItHint::CreateInsertion(optCallee->loc, "std::");
    }
  }
} // namespace clang::tidy::readability
