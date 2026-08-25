// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "check/ForbidRawThrowCheck.h"

#include "check/AstHelpers.h"

#include <clang/AST/ASTContext.h>
#include <clang/AST/Decl.h>
#include <clang/AST/DeclCXX.h>
#include <clang/AST/Expr.h>
#include <clang/AST/ExprCXX.h>
#include <clang/AST/OperationKinds.h>
#include <clang/AST/RecursiveASTVisitor.h>
#include <clang/AST/Stmt.h>
#include <clang/AST/StmtCXX.h>
#include <clang/ASTMatchers/ASTMatchFinder.h>
#include <clang/ASTMatchers/ASTMatchers.h>
#include <clang/Basic/LLVM.h>
#include <clang/Basic/SourceLocation.h>
#include <clang/Basic/SourceManager.h>
#include <clang/Lex/Lexer.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string_view>

using namespace clang;
using clang::ast_matchers::MatchFinder;

namespace clang::tidy::readability
{
  namespace
  {
    constexpr auto kTerminalCatchCalls = std::array{
      std::string_view{"ao::async::throwOperationCancelled"},
      std::string_view{"ao::detail::abortFatal"},
      std::string_view{"ao::detail::abortFatalDiagnostic"},
      std::string_view{"ao::detail::abortFatalEvaluation"},
      std::string_view{"ao::detail::abortRealtime"},
      std::string_view{"ao::detail::abortRealtimeEvaluation"},
      std::string_view{"ao::fatalFromException"},
      std::string_view{"std::rethrow_exception"},
    };

    template<std::size_t Size>
    bool containsName(std::array<std::string_view, Size> const& names, std::string_view const name)
    {
      return std::ranges::contains(names, name);
    }

    template<typename Predicate>
    bool callNames(CallExpr const& call, Predicate const& predicate)
    {
      if (auto const* callee = call.getDirectCallee(); callee != nullptr)
      {
        return std::invoke(predicate, callee->getQualifiedNameAsString());
      }

      auto const* lookup = dyn_cast<UnresolvedLookupExpr>(call.getCallee()->IgnoreParenImpCasts());

      if (lookup == nullptr)
      {
        return false;
      }

      return std::ranges::any_of(lookup->decls(),
                                 [&predicate](NamedDecl const* candidate)
                                 { return std::invoke(predicate, candidate->getQualifiedNameAsString()); });
    }

    bool isTerminalCatchCall(CallExpr const& call)
    {
      return callNames(call, [](std::string_view const name) { return containsName(kTerminalCatchCalls, name); });
    }

    bool isCurrentExceptionCall(CallExpr const& call)
    {
      return callNames(call, [](std::string_view const name) { return name == "std::current_exception"; });
    }

    bool isApprovedExceptionCarrier(ASTContext& context,
                                    SourceManager const& sourceManager,
                                    CXXThrowExpr const& throwExpr)
    {
      return aobus::enclosingFunctionBeginsWithPolicyMarker(
        throwExpr, context, sourceManager, "ao::detail::acknowledgeExceptionCarrier", "AO_EXCEPTION_CARRIER");
    }

    bool isStdBroadCatch(CXXCatchStmt const& catchStmt)
    {
      auto const* exceptionDecl = catchStmt.getExceptionDecl();

      if (exceptionDecl == nullptr)
      {
        return true;
      }

      auto const type = exceptionDecl->getType().getNonReferenceType().getUnqualifiedType().getCanonicalType();
      auto const* record = type->getAsCXXRecordDecl();

      if (record == nullptr || !record->isInStdNamespace())
      {
        return false;
      }

      auto const* identifier = record->getIdentifier();
      return identifier != nullptr && (identifier->getName() == "exception" || identifier->getName() == "bad_alloc");
    }

    bool beginsWithAuditedCatchMarker(CXXCatchStmt const& catchStmt,
                                      ASTContext const& context,
                                      SourceManager const& sourceManager)
    {
      return aobus::blockBeginsWithPolicyMarker(*catchStmt.getHandlerBlock(),
                                                context,
                                                sourceManager,
                                                "ao::detail::acknowledgeAuditedCatch",
                                                "AO_AUDITED_CATCH");
    }

    class CatchOwnershipInspection final : public RecursiveASTVisitor<CatchOwnershipInspection>
    {
    public:
      bool TraverseCXXCatchStmt(CXXCatchStmt* /*catchStmt*/)
      {
        // A nested handler owns its own active exception. Its transfer cannot
        // justify continuation from the outer broad catch under inspection.
        return true;
      }

      bool TraverseLambdaExpr(LambdaExpr* /*lambda*/)
      {
        // Defining a callable that might terminate later does not transfer the
        // exception currently owned by the surrounding catch.
        return true;
      }

      bool VisitCallExpr(CallExpr* call)
      {
        _capturesCurrentException = _capturesCurrentException || isCurrentExceptionCall(*call);
        return true;
      }

      bool capturesCurrentException() const noexcept { return _capturesCurrentException; }

    private:
      bool _capturesCurrentException = false;
    };

    struct CatchFlow final
    {
      bool fallsThrough = true;
      bool approvedTerminal = false;
      bool unapprovedTransfer = false;

      bool transfersOrTerminates() const noexcept { return !fallsThrough && approvedTerminal && !unapprovedTransfer; }
    };

    CatchFlow alternativeFlow(CatchFlow const& first, CatchFlow const& second) noexcept
    {
      return CatchFlow{.fallsThrough = first.fallsThrough || second.fallsThrough,
                       .approvedTerminal = first.approvedTerminal || second.approvedTerminal,
                       .unapprovedTransfer = first.unapprovedTransfer || second.unapprovedTransfer};
    }

    CatchFlow sequenceFlow(CatchFlow const& first, CatchFlow const& second) noexcept
    {
      if (!first.fallsThrough)
      {
        return first;
      }

      return CatchFlow{.fallsThrough = second.fallsThrough,
                       .approvedTerminal = first.approvedTerminal || second.approvedTerminal,
                       .unapprovedTransfer = first.unapprovedTransfer || second.unapprovedTransfer};
    }

    CatchFlow expressionFlow(Expr const& expression);

    CatchFlow statementFlow(Stmt const& statement)
    {
      if (auto const* expression = dyn_cast<Expr>(&statement); expression != nullptr)
      {
        return expressionFlow(*expression);
      }

      if (auto const* block = dyn_cast<CompoundStmt>(&statement); block != nullptr)
      {
        auto flow = CatchFlow{};

        for (auto const* child : block->body())
        {
          if (child != nullptr)
          {
            flow = sequenceFlow(flow, statementFlow(*child));
          }
        }

        return flow;
      }

      if (auto const* conditional = dyn_cast<IfStmt>(&statement); conditional != nullptr)
      {
        auto const* elseBranch = conditional->getElse();
        auto const elseFlow = elseBranch != nullptr ? statementFlow(*elseBranch) : CatchFlow{};
        return alternativeFlow(statementFlow(*conditional->getThen()), elseFlow);
      }

      if (auto const* tryStatement = dyn_cast<CXXTryStmt>(&statement); tryStatement != nullptr)
      {
        auto flow = statementFlow(*tryStatement->getTryBlock());

        for (std::uint32_t index = 0; index < tryStatement->getNumHandlers(); ++index)
        {
          flow = alternativeFlow(flow, statementFlow(*tryStatement->getHandler(index)->getHandlerBlock()));
        }

        return flow;
      }

      if (auto const* doStatement = dyn_cast<DoStmt>(&statement); doStatement != nullptr)
      {
        return statementFlow(*doStatement->getBody());
      }

      if (auto const* attributed = dyn_cast<AttributedStmt>(&statement); attributed != nullptr)
      {
        return statementFlow(*attributed->getSubStmt());
      }

      if (auto const* label = dyn_cast<LabelStmt>(&statement); label != nullptr)
      {
        return statementFlow(*label->getSubStmt());
      }

      if (isa<ReturnStmt, CoreturnStmt, GotoStmt, IndirectGotoStmt, BreakStmt, ContinueStmt>(&statement))
      {
        return CatchFlow{.fallsThrough = false, .unapprovedTransfer = true};
      }

      return {};
    }

    CatchFlow expressionFlow(Expr const& expression)
    {
      auto const* unwrapped = expression.IgnoreParenImpCasts();

      if (auto const* cleanup = dyn_cast<ExprWithCleanups>(unwrapped); cleanup != nullptr)
      {
        return expressionFlow(*cleanup->getSubExpr());
      }

      if (auto const* bind = dyn_cast<CXXBindTemporaryExpr>(unwrapped); bind != nullptr)
      {
        return expressionFlow(*bind->getSubExpr());
      }

      if (auto const* materialized = dyn_cast<MaterializeTemporaryExpr>(unwrapped); materialized != nullptr)
      {
        return expressionFlow(*materialized->getSubExpr());
      }

      if (auto const* throwExpression = dyn_cast<CXXThrowExpr>(unwrapped); throwExpression != nullptr)
      {
        return throwExpression->getSubExpr() == nullptr ? CatchFlow{.fallsThrough = false, .approvedTerminal = true}
                                                        : CatchFlow{.fallsThrough = false, .unapprovedTransfer = true};
      }

      if (auto const* call = dyn_cast<CallExpr>(unwrapped); call != nullptr)
      {
        return isTerminalCatchCall(*call) ? CatchFlow{.fallsThrough = false, .approvedTerminal = true} : CatchFlow{};
      }

      if (auto const* conditional = dyn_cast<ConditionalOperator>(unwrapped); conditional != nullptr)
      {
        return alternativeFlow(
          expressionFlow(*conditional->getTrueExpr()), expressionFlow(*conditional->getFalseExpr()));
      }

      if (auto const* binary = dyn_cast<BinaryOperator>(unwrapped); binary != nullptr)
      {
        if (binary->getOpcode() == BO_Comma)
        {
          return sequenceFlow(expressionFlow(*binary->getLHS()), expressionFlow(*binary->getRHS()));
        }

        if (binary->isLogicalOp())
        {
          return sequenceFlow(
            expressionFlow(*binary->getLHS()), alternativeFlow(CatchFlow{}, expressionFlow(*binary->getRHS())));
        }
      }

      return {};
    }

    bool transfersOrTerminates(CXXCatchStmt const& catchStmt)
    {
      auto inspection = CatchOwnershipInspection{};
      inspection.TraverseStmt(const_cast<Stmt*>(catchStmt.getHandlerBlock()));
      return inspection.capturesCurrentException() ||
             statementFlow(*catchStmt.getHandlerBlock()).transfersOrTerminates();
    }
  } // namespace

  void ForbidRawThrowCheck::registerMatchers(MatchFinder* finder)
  {
    using namespace clang::ast_matchers;

    finder->addMatcher(cxxThrowExpr().bind("throw"), this);
    finder->addMatcher(cxxCatchStmt().bind("catch"), this);
  }

  void ForbidRawThrowCheck::check(MatchFinder::MatchResult const& result)
  {
    auto const& sourceManager = *result.SourceManager;

    if (auto const* throwExpr = result.Nodes.getNodeAs<CXXThrowExpr>("throw"); throwExpr != nullptr)
    {
      auto const location = throwExpr->getThrowLoc();

      if (throwExpr->getSubExpr() == nullptr || !aobus::isPolicySource(sourceManager, location) ||
          isApprovedExceptionCarrier(*result.Context, sourceManager, *throwExpr))
      {
        return;
      }

      diag(location,
           "raw 'throw' is permitted only inside a helper whose first statement is "
           "AO_EXCEPTION_CARRIER(reason)");
      return;
    }

    auto const* catchStmt = result.Nodes.getNodeAs<CXXCatchStmt>("catch");

    if (catchStmt == nullptr || !isStdBroadCatch(*catchStmt) || transfersOrTerminates(*catchStmt) ||
        beginsWithAuditedCatchMarker(*catchStmt, *result.Context, sourceManager))
    {
      return;
    }

    auto const location = catchStmt->getCatchLoc();

    if (!aobus::isPolicySource(sourceManager, location))
    {
      return;
    }

    diag(location,
         "broad catch must rethrow, enter AO fatal handling, capture the current exception for later ownership, or "
         "begin with AO_AUDITED_CATCH(reason); catch the exact adapter exception type otherwise");
  }
} // namespace clang::tidy::readability
