#include "check/OptionalNamingAndUsageCheck.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"
#include "clang/Lex/Lexer.h"
#include <clang/AST/Decl.h>
#include <clang/AST/ExprCXX.h>
#include <clang/ASTMatchers/ASTMatchers.h>

using namespace clang::ast_matchers;

namespace clang::tidy::readability
{
  void OptionalNamingAndUsageCheck::registerMatchers(MatchFinder* finder)
  {
    // Robustly match std::optional by looking for the template specialization
    auto isOptionalType = hasType(qualType(hasUnqualifiedDesugaredType(
      recordType(hasDeclaration(classTemplateSpecializationDecl(hasName("::std::optional")))))));

    // Rule 1: Match all variables of type std::optional
    finder->addMatcher(varDecl(isOptionalType).bind("optional_var"), this);

    // Rule 2: Match calls to .has_value() on std::optional
    finder->addMatcher(
      cxxMemberCallExpr(on(expr(isOptionalType)), callee(cxxMethodDecl(hasName("has_value")))).bind("has_value_call"),
      this);
  }

  void OptionalNamingAndUsageCheck::check(MatchFinder::MatchResult const& result)
  {
    auto const& sm = *result.SourceManager;

    // Check Rule 1: Naming
    if (auto const* var = result.Nodes.getNodeAs<VarDecl>("optional_var"))
    {
      if (sm.isInSystemHeader(var->getLocation()) || var->getLocation().isMacroID())
      {
        return;
      }

      StringRef const name = var->getName();

      if (name.empty() || name.starts_with("opt"))
      {
        return;
      }

      // Special case: ignore common iterator names or similar if needed,
      // but for now we follow the strict "opt" prefix rule.
      diag(var->getLocation(), "std::optional variable %0 must have 'opt' prefix (e.g., 'opt%1')")
        << var << (name.substr(0, 1).upper() + name.substr(1).str());
    }

    // Check Rule 2: .has_value() usage
    if (auto const* call = result.Nodes.getNodeAs<CXXMemberCallExpr>("has_value_call"))
    {
      if (sm.isInSystemHeader(call->getBeginLoc()) || call->getBeginLoc().isMacroID())
      {
        return;
      }

      diag(call->getExprLoc(), "prefer concise 'if (opt)' or 'if (!opt)' over '.has_value()'");
    }
  }
} // namespace clang::tidy::readability
