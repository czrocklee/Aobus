// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "ThreadingPolicyCheck.h"

#include <clang/AST/ASTContext.h>
#include <clang/AST/Decl.h>
#include <clang/AST/DeclCXX.h>
#include <clang/AST/Expr.h>
#include <clang/AST/ExprCXX.h>
#include <clang/AST/RecursiveASTVisitor.h>
#include <clang/AST/Stmt.h>
#include <clang/AST/Type.h>
#include <clang/ASTMatchers/ASTMatchFinder.h>
#include <clang/ASTMatchers/ASTMatchers.h>
#include <clang/Basic/DiagnosticIDs.h>
#include <clang/Basic/LLVM.h>

#include <cstdint>

using namespace clang;

using clang::ast_matchers::MatchFinder;

namespace clang::tidy::readability
{
  namespace
  {
    bool isLockTagType(QualType type)
    {
      if (type.isNull())
      {
        return false;
      }

      type = type.getNonReferenceType().getUnqualifiedType().getCanonicalType();

      if (auto const* record = type->getAsCXXRecordDecl(); record != nullptr)
      {
        if (auto const* ii = record->getIdentifier(); ii != nullptr)
        {
          StringRef const name = ii->getName();
          return (name == "defer_lock_t" || name == "try_to_lock_t" || name == "adopt_lock_t") &&
                 record->isInStdNamespace();
        }
      }

      return false;
    }

    bool hasLockTagArgument(CXXConstructExpr const* ctorExpr)
    {
      for (std::uint32_t i = 0; i < ctorExpr->getNumArgs(); ++i)
      {
        if (auto const* argument = ctorExpr->getArg(i)->IgnoreParenImpCasts(); isLockTagType(argument->getType()))
        {
          return true;
        }
      }

      return false;
    }

    bool hasLockTagInit(VarDecl const* lockVar)
    {
      auto const* init = lockVar->getInit();

      if (init == nullptr)
      {
        return false;
      }

      init = init->IgnoreUnlessSpelledInSource();

      if (auto const* ctorExpr = dyn_cast<CXXConstructExpr>(init); ctorExpr != nullptr)
      {
        return hasLockTagArgument(ctorExpr);
      }

      return false;
    }

    // RecursiveASTVisitor customization points intentionally shadow its CRTP defaults.
    struct UseFinder final : RecursiveASTVisitor<UseFinder>
    {
      VarDecl const* target{};
      bool found = false;

      bool VisitCXXMemberCallExpr(CXXMemberCallExpr* call) // NOLINT(bugprone-derived-method-shadowing-base-method)
      {
        if (auto const* method = call->getMethodDecl(); method != nullptr)
        {
          if (auto const* ii = method->getIdentifier(); ii != nullptr)
          {
            if (StringRef const name = ii->getName();
                name == "lock" || name == "unlock" || name == "try_lock" || name == "release")
            {
              if (auto const* obj = call->getImplicitObjectArgument(); obj != nullptr)
              {
                obj = obj->IgnoreParenImpCasts();

                if (auto const* dre = dyn_cast<DeclRefExpr>(obj); dre != nullptr)
                {
                  if (dre->getDecl() == target || dre->getDecl()->getCanonicalDecl() == target->getCanonicalDecl())
                  {
                    found = true;
                    return false;
                  }
                }
              }
            }
          }
        }

        return checkCallArgs(call);
      }

      bool VisitCallExpr(CallExpr* call) // NOLINT(bugprone-derived-method-shadowing-base-method)
      {
        return checkCallArgs(call);
      }

    private:
      bool isConditionVariableWait(CallExpr const* call)
      {
        auto const* func = call->getDirectCallee();

        if (func == nullptr)
        {
          return false;
        }

        auto const* method = dyn_cast<CXXMethodDecl>(func);

        if (method == nullptr)
        {
          return false;
        }

        auto const* record = method->getParent();

        if (record == nullptr)
        {
          return false;
        }

        if (auto const* ii = record->getIdentifier(); ii != nullptr)
        {
          StringRef const name = ii->getName();
          return (name == "condition_variable" || name == "condition_variable_any") && record->isInStdNamespace();
        }

        return false;
      }

      bool isUniqueLockParameter(CallExpr const* call, std::uint32_t argumentIndex)
      {
        auto const* func = call->getDirectCallee();

        if (func == nullptr || argumentIndex >= func->getNumParams())
        {
          return false;
        }

        auto const* parameter = func->getParamDecl(argumentIndex);
        QualType const type = parameter->getType().getNonReferenceType().getUnqualifiedType().getCanonicalType();

        if (auto const* record = type->getAsCXXRecordDecl(); record != nullptr)
        {
          if (auto const* ii = record->getIdentifier(); ii != nullptr)
          {
            return ii->getName() == "unique_lock" && record->isInStdNamespace();
          }
        }

        return false;
      }

      bool checkCallArgs(CallExpr* call)
      {
        for (std::uint32_t i = 0; i < call->getNumArgs(); ++i)
        {
          auto const* argument = call->getArg(i)->IgnoreParenImpCasts();
          auto const* dre = dyn_cast<DeclRefExpr>(argument);

          if (dre == nullptr)
          {
            continue;
          }

          if (dre->getDecl() == target || dre->getDecl()->getCanonicalDecl() == target->getCanonicalDecl())
          {
            if (isConditionVariableWait(call) || isUniqueLockParameter(call, i))
            {
              found = true;
              return false;
            }
          }
        }

        return true;
      }
    };

    /// Returns true if the unique_lock variable is used in a way that requires
    /// unique_lock (explicit lock/unlock/try_lock, or passed to condvar::wait).
    bool needsUniqueLock(VarDecl const* lockVar, ASTContext& /*context*/)
    {
      // A unique_lock parameter represents an established lock lifetime or an
      // ownership transfer. It cannot be replaced locally with scoped_lock.
      if (isa<ParmVarDecl>(lockVar))
      {
        return true;
      }

      auto const* dc = lockVar->getParentFunctionOrMethod();

      if (dc == nullptr)
      {
        return false;
      }

      if (auto const* decl = dyn_cast<Decl>(dc); decl != nullptr)
      {
        if (auto const* body = decl->getBody(); body != nullptr)
        {
          if (hasLockTagInit(lockVar))
          {
            return true;
          }

          auto finder = UseFinder{};
          finder.target = lockVar;
          finder.TraverseStmt(const_cast<Stmt*>(body)); // NOLINT(cppcoreguidelines-pro-type-const-cast)

          return finder.found;
        }
      }

      return false;
    }
  } // namespace

  void ThreadingPolicyCheck::registerMatchers(MatchFinder* finder)
  {
    using namespace clang::ast_matchers;

    // Match std::thread usages (Rule 4.4.2)
    finder->addMatcher(
      varDecl(hasType(hasUnqualifiedDesugaredType(recordType(hasDeclaration(cxxRecordDecl(hasName("::std::thread")))))),
              unless(hasAncestor(functionDecl(isDefinition(), isMain()))))
        .bind("threadVar"),
      this);

    // Match std::unique_lock that could be std::scoped_lock (Rule 4.4.3)
    finder->addMatcher(varDecl(hasType(hasUnqualifiedDesugaredType(recordType(
                                 hasDeclaration(anyOf(classTemplateSpecializationDecl(hasName("::std::unique_lock")),
                                                      cxxRecordDecl(hasName("::std::unique_lock"))))))))
                         .bind("uniqueLock"),
                       this);

    // Match volatile variables (Rule 4.4.4)
    finder->addMatcher(varDecl(hasType(isVolatileQualified())).bind("volatileVar"), this);
  }

  void ThreadingPolicyCheck::check(MatchFinder::MatchResult const& result)
  {
    if (auto const* threadVar = result.Nodes.getNodeAs<VarDecl>("threadVar"); threadVar != nullptr)
    {
      diag(threadVar->getLocation(),
           "prefer std::jthread over std::thread (Rule 4.4.2); std::jthread provides automatic joining and stop_token "
           "support");
      diag(threadVar->getLocation(),
           "prefer std::jthread over std::thread for %0 (Rule 4.4.2); std::jthread provides automatic joining and "
           "stop_token support",
           DiagnosticIDs::Note)
        << threadVar;
    }

    if (auto const* lockVar = result.Nodes.getNodeAs<VarDecl>("uniqueLock"); lockVar != nullptr)
    {
      if (!needsUniqueLock(lockVar, *result.Context))
      {
        diag(lockVar->getLocation(),
             "prefer std::scoped_lock over std::unique_lock for %0 unless you need deferred locking, early unlock, or "
             "condition_variable integration (Rule 4.4.3)")
          << lockVar;
      }
    }

    if (auto const* volatileVar = result.Nodes.getNodeAs<VarDecl>("volatileVar"); volatileVar != nullptr)
    {
      diag(volatileVar->getLocation(),
           "%0 is volatile; use std::atomic<> for inter-thread communication instead (Rule 4.4.4)")
        << volatileVar;
    }
  }
} // namespace clang::tidy::readability
