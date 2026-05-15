#include "check/ForbidNodiscardCheck.h"
#include "clang/AST/ASTContext.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"

using namespace clang::ast_matchers;

namespace clang::tidy::readability {

void ForbidNodiscardCheck::registerMatchers(MatchFinder *Finder)
{
  Finder->addMatcher(
    functionDecl(
      hasAttr(attr::WarnUnusedResult)
    ).bind("func"),
    this);

  Finder->addMatcher(
    cxxRecordDecl(
      isDefinition(),
      hasAttr(attr::WarnUnusedResult)
    ).bind("record"),
    this);
}

void ForbidNodiscardCheck::check(const MatchFinder::MatchResult &Result)
{
  const auto &SM = *Result.SourceManager;

  if (const auto *Func = Result.Nodes.getNodeAs<FunctionDecl>("func"))
  {
    SourceLocation Loc = Func->getLocation();
    if (Loc.isInvalid() || Loc.isMacroID() || SM.isInSystemHeader(Loc))
      return;

    diag(Loc,
         "remove [[nodiscard]] from '%0'; rely on clang-tidy "
         "unused-return diagnostics instead")
      << Func;
  }

  if (const auto *Record = Result.Nodes.getNodeAs<CXXRecordDecl>("record"))
  {
    SourceLocation Loc = Record->getLocation();
    if (Loc.isInvalid() || Loc.isMacroID() || SM.isInSystemHeader(Loc))
      return;

    diag(Loc,
         "remove [[nodiscard]] from '%0'; rely on clang-tidy "
         "unused-return diagnostics instead")
      << Record;
  }
}

} // namespace clang::tidy::readability
