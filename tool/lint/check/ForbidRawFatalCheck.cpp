// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "check/ForbidRawFatalCheck.h"

#include "check/AstHelpers.h"

#include <clang/AST/Decl.h>
#include <clang/AST/Expr.h>
#include <clang/ASTMatchers/ASTMatchFinder.h>
#include <clang/ASTMatchers/ASTMatchers.h>
#include <clang/Basic/LLVM.h>
#include <clang/Basic/SourceLocation.h>
#include <clang/Basic/SourceManager.h>
#include <clang/Lex/Lexer.h>

#include <algorithm>
#include <array>
#include <string_view>

using namespace clang;
using clang::ast_matchers::MatchFinder;

namespace clang::tidy::readability
{
  namespace
  {
    constexpr auto kContractBackendMacros = std::array{
      std::string_view{"AO_DETAIL_FATAL_CONTRACT"},
      std::string_view{"AO_DETAIL_FATAL_CONTRACT_AT"},
      std::string_view{"AO_EXPECTS"},
      std::string_view{"AO_EXPECTS_AT"},
      std::string_view{"AO_ENSURES"},
      std::string_view{"AO_INVARIANT"},
      std::string_view{"AO_FATAL"},
      std::string_view{"AO_FATAL_AT"},
      std::string_view{"AO_RT_INVARIANT"},
      std::string_view{"AO_RT_FATAL_EXCEPTION_AT"},
    };

    bool isAoFatalBackend(DeclRefExpr const& reference)
    {
      auto const* function = dyn_cast<FunctionDecl>(reference.getDecl());

      if (function == nullptr)
      {
        return false;
      }

      auto const name = function->getQualifiedNameAsString();
      return name == "ao::detail::abortFatal" || name == "ao::detail::abortRealtime";
    }

    bool isContractBackendMacroExpansion(DeclRefExpr const& reference,
                                         SourceManager const& sourceManager,
                                         LangOptions const& langOptions)
    {
      auto location = reference.getBeginLoc();

      while (location.isMacroID())
      {
        auto const macroName = Lexer::getImmediateMacroName(location, sourceManager, langOptions);
        auto const macroNameView = std::string_view{macroName.data(), macroName.size()};

        if (std::ranges::contains(kContractBackendMacros, macroNameView))
        {
          return true;
        }

        location = sourceManager.getImmediateMacroCallerLoc(location);
      }

      return false;
    }
  } // namespace

  void ForbidRawFatalCheck::registerMatchers(MatchFinder* finder)
  {
    using namespace clang::ast_matchers;

    finder->addMatcher(declRefExpr(to(functionDecl(hasAnyName("::abort",
                                                              "std::abort",
                                                              "::terminate",
                                                              "std::terminate",
                                                              "::quick_exit",
                                                              "std::quick_exit",
                                                              "::_Exit",
                                                              "std::_Exit",
                                                              "ao::detail::abortFatal",
                                                              "ao::detail::abortRealtime"))))
                         .bind("rawFatal"),
                       this);
  }

  void ForbidRawFatalCheck::check(MatchFinder::MatchResult const& result)
  {
    auto const* reference = result.Nodes.getNodeAs<DeclRefExpr>("rawFatal");

    if (reference == nullptr ||
        (isAoFatalBackend(*reference) &&
         isContractBackendMacroExpansion(*reference, *result.SourceManager, result.Context->getLangOpts())) ||
        !aobus::isPolicySource(*result.SourceManager, reference->getBeginLoc()) ||
        aobus::enclosingFunctionBeginsWithPolicyMarker(*reference,
                                                       *result.Context,
                                                       *result.SourceManager,
                                                       "ao::detail::acknowledgeRawFatalBackend",
                                                       "AO_RAW_FATAL_BACKEND"))
    {
      return;
    }

    if (isAoFatalBackend(*reference))
    {
      diag(reference->getBeginLoc(),
           "AO fatal implementation backend is permitted only behind the public Contract.h surface or inside a "
           "helper whose first statement is AO_RAW_FATAL_BACKEND()");
      return;
    }

    diag(reference->getBeginLoc(),
         "raw process-termination primitive is permitted only inside a helper whose first statement is "
         "AO_RAW_FATAL_BACKEND()");
  }
} // namespace clang::tidy::readability
