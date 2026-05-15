#include "check/MemberInitializerBracesCheck.h"
#include "clang/AST/ASTContext.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"
#include "clang/Lex/Lexer.h"

using namespace clang::ast_matchers;

namespace clang::tidy::readability {

void MemberInitializerBracesCheck::registerMatchers(MatchFinder *Finder)
{
  Finder->addMatcher(
    cxxCtorInitializer(
      isWritten(),
      isMemberInitializer(),
      withInitializer(cxxConstructExpr(
        unless(isListInitialization())
      ).bind("ctor"))
    ).bind("init"),
    this);
}

void MemberInitializerBracesCheck::check(
  const MatchFinder::MatchResult &Result)
{
  const auto &SM = *Result.SourceManager;
  const auto *Init = Result.Nodes.getNodeAs<CXXCtorInitializer>("init");
  const auto *Ctor = Result.Nodes.getNodeAs<CXXConstructExpr>("ctor");

  if (!Init || !Ctor)
    return;

  SourceLocation Loc = Init->getSourceLocation();
  if (Loc.isInvalid() || Loc.isMacroID() || SM.isInSystemHeader(Loc))
    return;

  // Skip zero-arg and multi-arg constructors. Only single-arg parenthesized
  // inits are low-risk for brace conversion.
  if (Ctor->getNumArgs() != 1)
    return;

  auto const *ctorDecl = Ctor->getConstructor();
  if (!ctorDecl)
    return;

  auto Diag = diag(Loc,
    "member initializer should use brace initialization '%0{...}'");

  // Provide fix-it for single-argument constructors.
  auto Range = CharSourceRange::getTokenRange(Ctor->getSourceRange());
  StringRef Text = Lexer::getSourceText(
    Range, SM, Result.Context->getLangOpts());

  size_t OpenParen = Text.find('(');
  size_t CloseParen = Text.rfind(')');

  if (OpenParen != StringRef::npos && CloseParen != StringRef::npos &&
      OpenParen < CloseParen)
  {
    SourceLocation OpenLoc =
      Ctor->getBeginLoc().getLocWithOffset(OpenParen);
    SourceLocation CloseLoc =
      Ctor->getBeginLoc().getLocWithOffset(CloseParen);

    Diag << FixItHint::CreateReplacement(
      CharSourceRange::getCharRange(OpenLoc,
                                    OpenLoc.getLocWithOffset(1)),
      "{")
         << FixItHint::CreateReplacement(
      CharSourceRange::getCharRange(CloseLoc,
                                    CloseLoc.getLocWithOffset(1)),
      "}");
  }
}

} // namespace clang::tidy::readability
