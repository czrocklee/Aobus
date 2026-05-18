#include "check/ThreadingPolicyCheck.h"
#include "clang/AST/ASTContext.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"
#include <clang/AST/Decl.h>
#include <clang/AST/Expr.h>
#include <clang/AST/ExprCXX.h>
#include <clang/AST/Type.h>
#include <clang/ASTMatchers/ASTMatchers.h>
#include <clang/Basic/LLVM.h>
#include <clang/Basic/SourceLocation.h>

using namespace clang::ast_matchers;

namespace clang::tidy::readability
{
  namespace
  {
    /// Returns true if the unique_lock variable is used in a way that requires
    /// unique_lock (explicit lock/unlock/try_lock, or passed to condvar::wait).
    bool needsUniqueLock(VarDecl const* lockVar, ASTContext& /*unused*/)
    {
      auto const* dc = lockVar->getParentFunctionOrMethod();
      auto const* func = dyn_cast_or_null<FunctionDecl>(dc);

      if ((func == nullptr) || !func->hasBody())
      {
        return false;
      }

      // Check constructor args for defer_lock / try_to_lock / adopt_lock
      if (lockVar->hasInit())
      {
        auto const* ctorExpr = dyn_cast<CXXConstructExpr>(lockVar->getInit()->IgnoreImplicit());

        if (ctorExpr != nullptr)
        {
          for (unsigned i = 0; i < ctorExpr->getNumArgs(); ++i)
          {
            auto const* arg = ctorExpr->getArg(i);
            auto const* declRef = dyn_cast<DeclRefExpr>(arg->IgnoreImplicit());

            if (declRef != nullptr)
            {
              if (StringRef const name = declRef->getDecl()->getName();
                  name == "defer_lock" || name == "try_to_lock" || name == "adopt_lock")
              {
                return true;
              }
            }
          }
        }
      }

      // Walk the function body looking for uses that require unique_lock.
      struct UseFinder : RecursiveASTVisitor<UseFinder>
      {
        VarDecl const* target{};
        bool found = false;

        bool VisitCXXMemberCallExpr(CXXMemberCallExpr* call)
        {
          if (found)
          {
            return false;
          }

          auto const* method = call->getMethodDecl();

          if (method == nullptr)
          {
            return true;
          }

          auto const* obj = call->getImplicitObjectArgument();

          if (obj == nullptr)
          {
            return true;
          }

          auto const* declRef = dyn_cast<DeclRefExpr>(obj->IgnoreParenImpCasts());

          if ((declRef == nullptr) || declRef->getDecl() != target)
          {
            return true;
          }

          if (StringRef const name = method->getName(); name == "lock" || name == "unlock" || name == "try_lock" ||
                                                        name == "try_lock_for" || name == "try_lock_until" ||
                                                        name == "release" || name == "mutex")
          {
            found = true;
            return false;
          }

          return true;
        }
      };

      auto finder = UseFinder{};
      finder.target = lockVar;
      finder.TraverseStmt(func->getBody());
      return finder.found;
    }
  } // namespace

  void ThreadingPolicyCheck::registerMatchers(MatchFinder* finder)
  {
    // Match std::thread variable declarations (not std::thread::id)
    finder->addMatcher(
      varDecl(hasType(hasUnqualifiedDesugaredType(recordType(hasDeclaration(cxxRecordDecl(hasName("::std::thread")))))))
        .bind("thread_var"),
      this);

    // Match constructor calls to std::thread (catches local+temp construction)
    finder->addMatcher(
      cxxConstructExpr(hasDeclaration(cxxConstructorDecl(ofClass(cxxRecordDecl(hasName("::std::thread"))))),
                       unless(isInTemplateInstantiation()))
        .bind("thread_ctor"),
      this);

    // Match volatile-qualified variables (4.4.4)
    finder->addMatcher(varDecl(hasType(qualType(isVolatileQualified()))).bind("volatile_var"), this);

    // Match std::unique_lock variables
    finder->addMatcher(varDecl(hasType(hasUnqualifiedDesugaredType(
                                 recordType(hasDeclaration(cxxRecordDecl(hasName("::std::unique_lock")))))))
                         .bind("unique_lock_var"),
                       this);
  }

  void ThreadingPolicyCheck::check(MatchFinder::MatchResult const& result)
  {
    auto const& sm = *result.SourceManager;

    // --- std::thread variable declarations ---
    if (auto const* var = result.Nodes.getNodeAs<VarDecl>("thread_var"))
    {
      SourceLocation const loc = var->getLocation();

      if (loc.isInvalid() || loc.isMacroID() || sm.isInSystemHeader(loc))
      {
        return;
      }

      // Skip std::thread::id — it's not a thread object
      if (QualType const type = var->getType().getCanonicalType(); type->isReferenceType() || type->isPointerType())
      {
        return;
      }

      diag(loc,
           "prefer std::jthread over std::thread for '%0' (Rule 4.4.2); "
           "std::jthread provides automatic joining and stop_token support")
        << var;
    }

    // --- std::thread constructor calls ---
    if (auto const* ctor = result.Nodes.getNodeAs<CXXConstructExpr>("thread_ctor"))
    {
      SourceLocation const loc = ctor->getBeginLoc();

      if (loc.isInvalid() || loc.isMacroID() || sm.isInSystemHeader(loc))
      {
        return;
      }

      diag(loc,
           "prefer std::jthread over std::thread (Rule 4.4.2); "
           "std::jthread provides automatic joining and stop_token support");
    }

    // --- volatile variables ---
    if (auto const* var = result.Nodes.getNodeAs<VarDecl>("volatile_var"))
    {
      SourceLocation const loc = var->getLocation();

      if (loc.isInvalid() || loc.isMacroID() || sm.isInSystemHeader(loc))
      {
        return;
      }

      diag(loc,
           "'%0' is volatile; use std::atomic<> for inter-thread communication "
           "instead (Rule 4.4.4)")
        << var;
    }

    // --- std::unique_lock variables ---
    if (auto const* var = result.Nodes.getNodeAs<VarDecl>("unique_lock_var"))
    {
      SourceLocation const loc = var->getLocation();

      if (loc.isInvalid() || loc.isMacroID() || sm.isInSystemHeader(loc))
      {
        return;
      }

      if (needsUniqueLock(var, *result.Context))
      {
        return;
      }

      diag(loc,
           "prefer std::scoped_lock over std::unique_lock for '%0' "
           "unless you need deferred locking, early unlock, or "
           "condition_variable integration (Rule 4.4.3)")
        << var;
    }
  }
} // namespace clang::tidy::readability
