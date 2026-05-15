#include "check/LambdaParamsCheck.h"
#include "clang/AST/ASTContext.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"
#include "clang/Lex/Lexer.h"

using namespace clang::ast_matchers;

namespace clang::tidy::readability {

void LambdaParamsCheck::registerMatchers(MatchFinder *Finder) {
  Finder->addMatcher(lambdaExpr().bind("lambda"), this);
}

void LambdaParamsCheck::check(const MatchFinder::MatchResult &Result) {
  const auto *Lambda = Result.Nodes.getNodeAs<LambdaExpr>("lambda");
  const auto &SM = *Result.SourceManager;

  if (SM.isInSystemHeader(Lambda->getBeginLoc()) || Lambda->getBeginLoc().isMacroID())
    return;

  // Rule 3.4.7: Omit empty parameter list '()'
  if (Lambda->hasExplicitParameters() && Lambda->getCallOperator()->getNumParams() == 0) {
    // We found a lambda with explicit but empty parameters.
    // We need to check if they can be safely removed.
    // In C++23, nearly all empty parameter lists can be omitted even with
    // mutable, noexcept, etc.
    
    // We'll use the lexer to find the parentheses.
    SourceLocation IntroducerEnd = Lambda->getIntroducerRange().getEnd();
    SourceLocation BodyStart = Lambda->getBody()->getBeginLoc();
    
    if (IntroducerEnd.isInvalid() || BodyStart.isInvalid())
      return;

    // Search for '(' between the introducer and the body.
    // Note: We skip the introducer end itself (the ']').
    SourceLocation SearchLoc = IntroducerEnd.getLocWithOffset(1);
    
    // Use Lexer to find the first '(' token.
    Token Tok;
    bool FoundLParen = false;
    SourceLocation LParenLoc;
    SourceLocation RParenLoc;

    while (SearchLoc < BodyStart) {
      if (Lexer::getRawToken(SearchLoc, Tok, SM, Result.Context->getLangOpts(), true))
        break;
      
      if (Tok.is(tok::l_paren)) {
        FoundLParen = true;
        LParenLoc = Tok.getLocation();
      } else if (Tok.is(tok::r_paren)) {
        if (FoundLParen) {
          RParenLoc = Tok.getLocation();
          break;
        }
      }
      
      SearchLoc = Tok.getEndLoc();
    }

    if (FoundLParen && RParenLoc.isValid()) {
      auto Diag = diag(LParenLoc, "omit empty parameter list '()' in lambda");
      Diag << FixItHint::CreateRemoval(SourceRange(LParenLoc, RParenLoc));
    }
  }
}

} // namespace clang::tidy::readability
