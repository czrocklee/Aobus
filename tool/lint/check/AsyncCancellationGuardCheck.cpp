// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "check/AsyncCancellationGuardCheck.h"

#include <clang/AST/ASTTypeTraits.h>
#include <clang/AST/DeclCXX.h>
#include <clang/AST/Expr.h>
#include <clang/AST/ExprCXX.h>
#include <clang/AST/ParentMapContext.h>
#include <clang/AST/Stmt.h>
#include <clang/AST/StmtCXX.h>
#include <clang/ASTMatchers/ASTMatchFinder.h>
#include <clang/ASTMatchers/ASTMatchers.h>
#include <clang/Basic/SourceLocation.h>
#include <clang/Basic/SourceManager.h>
#include <llvm/Support/Casting.h>

using namespace clang;

using clang::ast_matchers::MatchFinder;

namespace clang::tidy::readability
{
  namespace
  {
    bool isStdExceptionCatch(CXXCatchStmt const* catchStmt)
    {
      auto const* exceptionDecl = catchStmt->getExceptionDecl();

      if (exceptionDecl == nullptr)
      {
        return false;
      }

      auto const type = exceptionDecl->getType().getNonReferenceType().getUnqualifiedType().getCanonicalType();
      auto const* record = type->getAsCXXRecordDecl();

      if (record == nullptr)
      {
        return false;
      }

      auto const* identifier = record->getIdentifier();
      return identifier != nullptr && identifier->getName() == "exception" && record->isInStdNamespace();
    }

    bool isBroadCatch(CXXCatchStmt const* catchStmt)
    {
      return catchStmt->getExceptionDecl() == nullptr || isStdExceptionCatch(catchStmt);
    }

    CallExpr const* unwrapSingleExpression(Stmt const* stmt)
    {
      auto const* current = stmt;

      while (current != nullptr)
      {
        if (auto const* call = llvm::dyn_cast<CallExpr>(current); call != nullptr)
        {
          return call;
        }

        if (auto const* expr = llvm::dyn_cast<Expr>(current); expr != nullptr)
        {
          if (auto const* ignored = expr->IgnoreImplicit(); ignored != expr)
          {
            current = ignored;
            continue;
          }
        }

        if (auto const* cleanups = llvm::dyn_cast<ExprWithCleanups>(current); cleanups != nullptr)
        {
          current = cleanups->getSubExpr();
          continue;
        }

        if (auto const* temporary = llvm::dyn_cast<CXXBindTemporaryExpr>(current); temporary != nullptr)
        {
          current = temporary->getSubExpr();
          continue;
        }

        if (auto const* materialized = llvm::dyn_cast<MaterializeTemporaryExpr>(current); materialized != nullptr)
        {
          current = materialized->getSubExpr();
          continue;
        }

        return nullptr;
      }

      return nullptr;
    }

    DeclRefExpr const* asDeclRef(Expr const* expr)
    {
      if (expr == nullptr)
      {
        return nullptr;
      }

      return llvm::dyn_cast<DeclRefExpr>(expr->IgnoreParenImpCasts());
    }

    bool referencesCatchVariable(Expr const* expr, VarDecl const* exceptionDecl)
    {
      auto const* ref = asDeclRef(expr);
      return ref != nullptr && ref->getDecl()->getCanonicalDecl() == exceptionDecl->getCanonicalDecl();
    }

    bool hasValidGuardArguments(CallExpr const* call, CXXCatchStmt const* catchStmt)
    {
      auto const* exceptionDecl = catchStmt->getExceptionDecl();

      if (exceptionDecl == nullptr)
      {
        return call->getNumArgs() == 0;
      }

      return call->getNumArgs() == 1 && referencesCatchVariable(call->getArg(0), exceptionDecl);
    }

    bool isCancellationGuardCall(Stmt const* stmt, CXXCatchStmt const* catchStmt)
    {
      auto const* call = unwrapSingleExpression(stmt);

      if (call == nullptr)
      {
        return false;
      }

      auto const* callee = call->getDirectCallee();
      return callee != nullptr && callee->getQualifiedNameAsString() == "ao::async::rethrowIfOperationCancelled" &&
             hasValidGuardArguments(call, catchStmt);
    }

    bool startsWithCancellationGuard(CXXCatchStmt const* catchStmt)
    {
      auto const* body = llvm::dyn_cast_or_null<CompoundStmt>(catchStmt->getHandlerBlock());

      if (body == nullptr || body->body_empty())
      {
        return false;
      }

      return isCancellationGuardCall(*body->body_begin(), catchStmt);
    }

    FunctionDecl const* nearestEnclosingFunction(ASTContext& context, CXXCatchStmt const* catchStmt)
    {
      auto current = DynTypedNode::create(*catchStmt);

      while (true)
      {
        auto const parents = context.getParents(current);

        if (parents.empty())
        {
          return nullptr;
        }

        auto const& parent = *parents.begin();

        if (auto const* function = parent.get<FunctionDecl>(); function != nullptr)
        {
          return function;
        }

        current = parent;
      }
    }

    bool isCoroutine(FunctionDecl const* function)
    {
      return function != nullptr && llvm::isa_and_nonnull<CoroutineBodyStmt>(function->getBody());
    }
  } // namespace

  void AsyncCancellationGuardCheck::registerMatchers(MatchFinder* finder)
  {
    using namespace clang::ast_matchers;

    finder->addMatcher(cxxCatchStmt().bind("catch"), this);
  }

  void AsyncCancellationGuardCheck::check(MatchFinder::MatchResult const& result)
  {
    auto const* catchStmt = result.Nodes.getNodeAs<CXXCatchStmt>("catch");

    if (catchStmt == nullptr || !isBroadCatch(catchStmt))
    {
      return;
    }

    if (!isCoroutine(nearestEnclosingFunction(*result.Context, catchStmt)))
    {
      return;
    }

    auto const& sm = *result.SourceManager;
    auto const loc = catchStmt->getBeginLoc();

    if (loc.isInvalid() || loc.isMacroID() || sm.isInSystemHeader(loc))
    {
      return;
    }

    if (startsWithCancellationGuard(catchStmt))
    {
      return;
    }

    diag(loc,
         "broad catch handler in a coroutine must first call "
         "ao::async::rethrowIfOperationCancelled to let lifetime cancellation escape");
  }
} // namespace clang::tidy::readability
