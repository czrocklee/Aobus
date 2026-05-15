#include "check/UnusedSuppressionStyleCheck.h"
#include "clang/AST/ASTContext.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"

using namespace clang::ast_matchers;

namespace clang::tidy::readability {

void UnusedSuppressionStyleCheck::registerMatchers(MatchFinder *Finder)
{
  Finder->addMatcher(
    cStyleCastExpr().bind("cast"),
    this);

  Finder->addMatcher(
    cxxStaticCastExpr().bind("cast"),
    this);
}

void UnusedSuppressionStyleCheck::check(
  const MatchFinder::MatchResult &Result)
{
  const auto &SM = *Result.SourceManager;
  const auto *Cast = Result.Nodes.getNodeAs<ExplicitCastExpr>("cast");

  if (!Cast)
    return;

  SourceLocation Loc = Cast->getBeginLoc();
  if (Loc.isInvalid() || Loc.isMacroID() || SM.isInSystemHeader(Loc))
    return;

  // Only flag casts to void
  if (!Cast->getType()->isVoidType())
    return;

  // Get the sub-expression being cast to void
  auto const *SubExpr = Cast->getSubExpr();
  if (!SubExpr)
    return;

  // Skip if the sub-expression is already [[maybe_unused]] — the cast is
  // redundant alongside the attribute, but the attribute is the correct fix.
  if (auto const *DeclRef = dyn_cast<DeclRefExpr>(SubExpr->IgnoreParenImpCasts()))
  {
    if (auto const *VD = dyn_cast<VarDecl>(DeclRef->getDecl()))
    {
      if (VD->hasAttr<UnusedAttr>())
        return;
    }
  }

  bool isCStyle = isa<CStyleCastExpr>(Cast);
  diag(Loc,
       "void cast %0 suppresses unused warnings; use [[maybe_unused]] on "
       "the declaration or an anonymous parameter instead")
    << (isCStyle ? "'(void)expr'" : "'static_cast<void>(expr)'");
}

} // namespace clang::tidy::readability
