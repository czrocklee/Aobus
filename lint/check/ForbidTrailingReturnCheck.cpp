#include "check/ForbidTrailingReturnCheck.h"
#include "clang/AST/ASTContext.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"

using namespace clang::ast_matchers;

namespace clang::tidy::readability {

void ForbidTrailingReturnCheck::registerMatchers(MatchFinder *Finder)
{
  Finder->addMatcher(
    functionDecl(
      hasTrailingReturn(),
      unless(isImplicit())
    ).bind("func"),
    this);
}

void ForbidTrailingReturnCheck::check(const MatchFinder::MatchResult &Result)
{
  const auto &SM = *Result.SourceManager;
  const auto *Func = Result.Nodes.getNodeAs<FunctionDecl>("func");

  if (!Func)
    return;

  SourceLocation Loc = Func->getLocation();
  if (Loc.isInvalid() || Loc.isMacroID() || SM.isInSystemHeader(Loc))
    return;

  // Skip lambda call operators — trailing return is allowed on lambdas
  if (auto const *RD = dyn_cast<CXXRecordDecl>(Func->getParent()))
  {
    if (RD->isLambda())
      return;
  }

  // Skip deduction guides (CTAD)
  if (isa<CXXDeductionGuideDecl>(Func))
    return;

  // Skip if already has `auto` return type with no trailing return
  // (hasTrailingReturnType already filtered this, so if we get here
  // it's a non-lambda function with trailing return)

  diag(Loc,
       "non-lambda function '%0' uses trailing return type; "
       "use traditional return type syntax")
    << Func->getName();
}

} // namespace clang::tidy::readability
