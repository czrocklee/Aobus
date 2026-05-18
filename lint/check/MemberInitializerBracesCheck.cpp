#include "check/MemberInitializerBracesCheck.h"
#include "clang/AST/ASTContext.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"
#include "clang/Lex/Lexer.h"
#include <clang/AST/DeclCXX.h>
#include <clang/AST/ExprCXX.h>
#include <clang/ASTMatchers/ASTMatchers.h>
#include <clang/Basic/Diagnostic.h>
#include <clang/Basic/SourceLocation.h>
#include <cstddef>

using namespace clang::ast_matchers;

namespace clang::tidy::readability
{
  void MemberInitializerBracesCheck::registerMatchers(MatchFinder* finder)
  {
    finder->addMatcher(
      cxxCtorInitializer(isWritten(),
                         isMemberInitializer(),
                         withInitializer(cxxConstructExpr(unless(isListInitialization())).bind("ctor")))
        .bind("init"),
      this);
  }

  void MemberInitializerBracesCheck::check(MatchFinder::MatchResult const& result)
  {
    auto const& sm = *result.SourceManager;
    auto const* init = result.Nodes.getNodeAs<CXXCtorInitializer>("init");
    auto const* ctor = result.Nodes.getNodeAs<CXXConstructExpr>("ctor");

    if ((init == nullptr) || (ctor == nullptr))
    {
      return;
    }

    SourceLocation const loc = init->getSourceLocation();

    if (loc.isInvalid() || loc.isMacroID() || sm.isInSystemHeader(loc))
    {
      return;
    }

    // Skip zero-arg and multi-arg constructors. Only single-arg parenthesized
    // inits are low-risk for brace conversion.
    if (ctor->getNumArgs() != 1)
    {
      return;
    }

    if (auto const* ctorDecl = ctor->getConstructor(); ctorDecl == nullptr)
    {
      return;
    }

    auto const* member = init->getMember();
    auto const memberName = StringRef{member != nullptr ? member->getName() : ""};
    auto diagBuilder = diag(loc, "member initializer should use brace initialization '%0{...}'") << memberName;

    // Provide fix-it for single-argument constructors.
    auto const range = CharSourceRange::getTokenRange(ctor->getSourceRange());
    StringRef const text = Lexer::getSourceText(range, sm, result.Context->getLangOpts());

    size_t const openParen = text.find('(');

    if (size_t const closeParen = text.rfind(')');
        openParen != StringRef::npos && closeParen != StringRef::npos && openParen < closeParen)
    {
      SourceLocation const openLoc = ctor->getBeginLoc().getLocWithOffset(static_cast<int>(openParen));
      SourceLocation const closeLoc = ctor->getBeginLoc().getLocWithOffset(static_cast<int>(closeParen));

      diagBuilder << FixItHint::CreateReplacement(
                       CharSourceRange::getCharRange(openLoc, openLoc.getLocWithOffset(1)), "{")
                  << FixItHint::CreateReplacement(
                       CharSourceRange::getCharRange(closeLoc, closeLoc.getLocWithOffset(1)), "}");
    }
  }
} // namespace clang::tidy::readability
