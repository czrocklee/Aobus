// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "check/CApiGlobalQualificationCheck.h"

#include "check/CalleeQualificationHelpers.h"

#include <clang/AST/ASTContext.h>
#include <clang/AST/Decl.h>
#include <clang/AST/Expr.h>
#include <clang/AST/ExprCXX.h>
#include <clang/ASTMatchers/ASTMatchFinder.h>
#include <clang/ASTMatchers/ASTMatchers.h>
#include <clang/Basic/Diagnostic.h>
#include <clang/Basic/LLVM.h>
#include <clang/Basic/SourceLocation.h>

using namespace clang::ast_matchers;

namespace clang::tidy::readability
{
  void CApiGlobalQualificationCheck::registerMatchers(MatchFinder* finder)
  {
    finder->addMatcher(callExpr(callee(functionDecl(isExternC()).bind("func"))).bind("call"), this);
  }

  void CApiGlobalQualificationCheck::check(MatchFinder::MatchResult const& result)
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

    // Skip calls routed through a non-type template parameter (e.g. `Fn(ptr)`
    // where a C function is passed as a template argument). The call site spells
    // the parameter name, not the function, so `::` cannot be inserted there.
    // Walk the cast/paren chain explicitly because IgnoreParenCasts() strips the
    // SubstNonTypeTemplateParmExpr node and would unwrap past the marker.
    for (Expr const* callee = call->getCallee(); callee != nullptr;)
    {
      if (isa<SubstNonTypeTemplateParmExpr>(callee))
      {
        return;
      }

      if (auto const* cast = dyn_cast<CastExpr>(callee); cast != nullptr)
      {
        callee = cast->getSubExpr();
      }
      else if (auto const* paren = dyn_cast<ParenExpr>(callee); paren != nullptr)
      {
        callee = paren->getSubExpr();
      }
      else
      {
        break;
      }
    }

    // Skip functions defined in the main file — these are project-local
    // extern "C" wrappers, not external C API calls.
    if (sm.isInMainFile(func->getLocation()))
    {
      return;
    }

    // Skip C standard library functions — those need std::, handled by Check 2
    if (aobus::isCStandardLibraryFunction(func->getName()))
    {
      return;
    }

    auto const optCallee = aobus::getCalleeForQualification(*call, sm, result.Context->getLangOpts());

    if (!optCallee)
    {
      return;
    }

    if (optCallee->text.starts_with("::") || optCallee->text.starts_with("std::"))
    {
      return;
    }

    diag(optCallee->loc, "external C library function '%0' must use global qualification '::%0'")
      << func->getName() << FixItHint::CreateInsertion(optCallee->loc, "::");
  }
} // namespace clang::tidy::readability
