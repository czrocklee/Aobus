#include "check/ThreadingPolicyCheck.h"
#include "clang/AST/ASTContext.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"

using namespace clang::ast_matchers;

namespace clang::tidy::readability {

namespace {

/// Returns true if the unique_lock variable is used in a way that requires
/// unique_lock (explicit lock/unlock/try_lock, or passed to condvar::wait).
bool needsUniqueLock(VarDecl const *lockVar, ASTContext &)
{
  auto const *dc = lockVar->getParentFunctionOrMethod();
  auto const *func = dyn_cast_or_null<FunctionDecl>(dc);
  if (!func || !func->hasBody())
    return false;

  // Check constructor args for defer_lock / try_to_lock / adopt_lock
  if (lockVar->hasInit())
  {
    auto const *ctorExpr =
      dyn_cast<CXXConstructExpr>(lockVar->getInit()->IgnoreImplicit());
    if (ctorExpr)
    {
      for (unsigned i = 0; i < ctorExpr->getNumArgs(); ++i)
      {
        auto const *arg = ctorExpr->getArg(i);
        auto const *declRef = dyn_cast<DeclRefExpr>(arg->IgnoreImplicit());
        if (declRef)
        {
          StringRef name = declRef->getDecl()->getName();
          if (name == "defer_lock" || name == "try_to_lock" ||
              name == "adopt_lock")
            return true;
        }
      }
    }
  }

  // Walk the function body looking for uses that require unique_lock.
  struct UseFinder : RecursiveASTVisitor<UseFinder>
  {
    VarDecl const *Target;
    bool Found = false;

    bool VisitCXXMemberCallExpr(CXXMemberCallExpr *Call)
    {
      if (Found) return false;

      auto const *method = Call->getMethodDecl();
      if (!method) return true;

      auto const *obj = Call->getImplicitObjectArgument();
      if (!obj) return true;

      auto const *declRef = dyn_cast<DeclRefExpr>(
        obj->IgnoreParenImpCasts());
      if (!declRef || declRef->getDecl() != Target)
        return true;

      StringRef name = method->getName();
      if (name == "lock" || name == "unlock" || name == "try_lock" ||
          name == "try_lock_for" || name == "try_lock_until" ||
          name == "release" || name == "mutex")
      {
        Found = true;
        return false;
      }
      return true;
    }
  };

  UseFinder finder;
  finder.Target = lockVar;
  finder.TraverseStmt(func->getBody());
  return finder.Found;
}

} // namespace

void ThreadingPolicyCheck::registerMatchers(MatchFinder *Finder)
{
  // Match std::thread variable declarations (not std::thread::id)
  Finder->addMatcher(
    varDecl(
      hasType(hasUnqualifiedDesugaredType(
        recordType(hasDeclaration(
          cxxRecordDecl(hasName("::std::thread"))
        ))
      ))
    ).bind("thread_var"),
    this);

  // Match constructor calls to std::thread (catches local+temp construction)
  Finder->addMatcher(
    cxxConstructExpr(
      hasDeclaration(
        cxxConstructorDecl(ofClass(cxxRecordDecl(hasName("::std::thread"))))
      ),
      unless(isInTemplateInstantiation())
    ).bind("thread_ctor"),
    this);

  // Match volatile-qualified variables (4.4.4)
  Finder->addMatcher(
    varDecl(
      hasType(qualType(isVolatileQualified()))
    ).bind("volatile_var"),
    this);

  // Match std::unique_lock variables
  Finder->addMatcher(
    varDecl(
      hasType(hasUnqualifiedDesugaredType(
        recordType(hasDeclaration(
          cxxRecordDecl(hasName("::std::unique_lock"))
        ))
      ))
    ).bind("unique_lock_var"),
    this);
}

void ThreadingPolicyCheck::check(const MatchFinder::MatchResult &Result)
{
  const auto &SM = *Result.SourceManager;

  // --- std::thread variable declarations ---
  if (const auto *Var = Result.Nodes.getNodeAs<VarDecl>("thread_var"))
  {
    SourceLocation Loc = Var->getLocation();
    if (Loc.isInvalid() || Loc.isMacroID() || SM.isInSystemHeader(Loc))
      return;

    // Skip std::thread::id — it's not a thread object
    QualType type = Var->getType().getCanonicalType();
    if (type->isReferenceType() || type->isPointerType())
      return;

    diag(Loc,
         "prefer std::jthread over std::thread for '%0' (Rule 4.4.2); "
         "std::jthread provides automatic joining and stop_token support")
      << Var;
  }

  // --- std::thread constructor calls ---
  if (const auto *Ctor = Result.Nodes.getNodeAs<CXXConstructExpr>("thread_ctor"))
  {
    SourceLocation Loc = Ctor->getBeginLoc();
    if (Loc.isInvalid() || Loc.isMacroID() || SM.isInSystemHeader(Loc))
      return;

    diag(Loc,
         "prefer std::jthread over std::thread (Rule 4.4.2); "
         "std::jthread provides automatic joining and stop_token support");
  }

  // --- volatile variables ---
  if (const auto *Var = Result.Nodes.getNodeAs<VarDecl>("volatile_var"))
  {
    SourceLocation Loc = Var->getLocation();
    if (Loc.isInvalid() || Loc.isMacroID() || SM.isInSystemHeader(Loc))
      return;

    diag(Loc,
         "'%0' is volatile; use std::atomic<> for inter-thread communication "
         "instead (Rule 4.4.4)")
      << Var;
  }

  // --- std::unique_lock variables ---
  if (const auto *Var = Result.Nodes.getNodeAs<VarDecl>("unique_lock_var"))
  {
    SourceLocation Loc = Var->getLocation();
    if (Loc.isInvalid() || Loc.isMacroID() || SM.isInSystemHeader(Loc))
      return;

    if (needsUniqueLock(Var, *Result.Context))
      return;

    diag(Loc,
         "prefer std::scoped_lock over std::unique_lock for '%0' "
         "unless you need deferred locking, early unlock, or "
         "condition_variable integration (Rule 4.4.3)")
      << Var;
  }
}

} // namespace clang::tidy::readability
