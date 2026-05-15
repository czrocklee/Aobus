#include "check/OptionalNamingAndUsageCheck.h"
#include "clang/AST/ASTContext.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"
#include "clang/Lex/Lexer.h"

using namespace clang::ast_matchers;

namespace clang::tidy::readability {

void OptionalNamingAndUsageCheck::registerMatchers(MatchFinder *Finder) {
  // Robustly match std::optional by looking for the template specialization
  auto IsOptionalType = hasType(qualType(hasUnqualifiedDesugaredType(
      recordType(hasDeclaration(classTemplateSpecializationDecl(
          hasName("::std::optional")))))));

  // Rule 1: Match all variables of type std::optional
  Finder->addMatcher(varDecl(IsOptionalType).bind("optional_var"), this);

  // Rule 2: Match calls to .has_value() on std::optional
  Finder->addMatcher(
      cxxMemberCallExpr(
          on(expr(IsOptionalType)),
          callee(cxxMethodDecl(hasName("has_value"))))
          .bind("has_value_call"),
      this);
}

void OptionalNamingAndUsageCheck::check(const MatchFinder::MatchResult &Result) {
  const auto &SM = *Result.SourceManager;

  // Check Rule 1: Naming
  if (const auto *Var = Result.Nodes.getNodeAs<VarDecl>("optional_var")) {
    if (SM.isInSystemHeader(Var->getLocation()) || Var->getLocation().isMacroID())
      return;

    StringRef Name = Var->getName();
    if (Name.empty() || Name.starts_with("opt"))
      return;

    // Special case: ignore common iterator names or similar if needed, 
    // but for now we follow the strict "opt" prefix rule.
    diag(Var->getLocation(),
         "std::optional variable %0 must have 'opt' prefix (e.g., 'opt%1')")
        << Var
        << (Name.substr(0, 1).upper() + Name.substr(1).str());
  }

  // Check Rule 2: .has_value() usage
  if (const auto *Call = Result.Nodes.getNodeAs<CXXMemberCallExpr>("has_value_call")) {
    if (SM.isInSystemHeader(Call->getBeginLoc()) || Call->getBeginLoc().isMacroID())
      return;

    diag(Call->getExprLoc(),
         "prefer concise 'if (opt)' or 'if (!opt)' over '.has_value()'");
  }
}

} // namespace clang::tidy::readability
