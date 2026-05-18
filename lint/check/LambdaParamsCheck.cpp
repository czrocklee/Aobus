#include "check/LambdaParamsCheck.h"
#include "clang/AST/ASTContext.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"
#include "clang/Lex/Lexer.h"
#include <clang/AST/ExprCXX.h>
#include <clang/ASTMatchers/ASTMatchers.h>
#include <clang/Basic/Diagnostic.h>
#include <clang/Basic/TokenKinds.h>
#include <clang/Lex/Token.h>

using namespace clang::ast_matchers;

namespace clang::tidy::readability
{
  void LambdaParamsCheck::registerMatchers(MatchFinder* finder)
  {
    finder->addMatcher(lambdaExpr().bind("lambda"), this);
  }

  void LambdaParamsCheck::check(MatchFinder::MatchResult const& result)
  {
    auto const* lambda = result.Nodes.getNodeAs<LambdaExpr>("lambda");
    auto const& sm = *result.SourceManager;

    if (sm.isInSystemHeader(lambda->getBeginLoc()) || lambda->getBeginLoc().isMacroID())
    {
      return;
    }

    // Rule 3.4.7: Omit empty parameter list '()'
    if (lambda->hasExplicitParameters() && lambda->getCallOperator()->getNumParams() == 0)
    {
      // We found a lambda with explicit but empty parameters.
      // We need to check if they can be safely removed.
      // In C++23, nearly all empty parameter lists can be omitted even with
      // mutable, noexcept, etc.

      // We'll use the lexer to find the parentheses.
      SourceLocation const introducerEnd = lambda->getIntroducerRange().getEnd();
      SourceLocation const bodyStart = lambda->getBody()->getBeginLoc();

      if (introducerEnd.isInvalid() || bodyStart.isInvalid())
      {
        return;
      }

      // Search for '(' between the introducer and the body.
      // Note: We skip the introducer end itself (the ']').
      auto searchLoc = introducerEnd.getLocWithOffset(1);

      // Use Lexer to find the first '(' token.
      auto tok = Token{};
      bool foundLParen = false;
      auto lParenLoc = SourceLocation{};
      auto rParenLoc = SourceLocation{};

      while (searchLoc < bodyStart)
      {
        if (Lexer::getRawToken(searchLoc, tok, sm, result.Context->getLangOpts(), true))
        {
          break;
        }

        if (tok.is(tok::l_paren))
        {
          foundLParen = true;
          lParenLoc = tok.getLocation();
        }
        else if (tok.is(tok::r_paren))
        {
          if (foundLParen)
          {
            rParenLoc = tok.getLocation();
            break;
          }
        }

        searchLoc = tok.getEndLoc();
      }

      if (foundLParen && rParenLoc.isValid())
      {
        auto diagBuilder = diag(lParenLoc, "omit empty parameter list '()' in lambda");
        diagBuilder << FixItHint::CreateRemoval(SourceRange(lParenLoc, rParenLoc));
      }
    }
  }
} // namespace clang::tidy::readability
