#include "check/IdentifierNamingExtensionsCheck.h"
#include "clang/AST/ASTContext.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"
#include "clang/AST/DeclCXX.h"

using namespace clang::ast_matchers;

namespace clang::tidy::readability {

void IdentifierNamingExtensionsCheck::registerMatchers(MatchFinder *Finder) {
  Finder->addMatcher(
      fieldDecl(hasParent(cxxRecordDecl(isDefinition()).bind("record")))
          .bind("field"),
      this);
}

static bool isCamelBack(StringRef name) {
  return !name.empty() && isLowercase(name[0]) && !name.contains('_');
}

static bool hasPrivateMembers(const CXXRecordDecl *record) {
  for (const auto *field : record->fields()) {
    if (field->getAccess() == AS_private)
      return true;
  }
  return false;
}

void IdentifierNamingExtensionsCheck::check(
    const MatchFinder::MatchResult &Result) {
  const auto &SM = *Result.SourceManager;
  const auto *Field = Result.Nodes.getNodeAs<FieldDecl>("field");
  const auto *Record = Result.Nodes.getNodeAs<CXXRecordDecl>("record");

  if (!Field || !Record)
    return;

  if (SM.isInSystemHeader(Field->getLocation()) ||
      Field->getLocation().isMacroID())
    return;

  StringRef Name = Field->getName();
  if (Name.empty() || Name == "_")
    return;

  bool IsClass = Record->isClass();

  if (IsClass) {
    // Class members must be _camelCase
    if (Name.starts_with("_")) {
      // Check the part after underscore is camelBack
      StringRef rest = Name.substr(1);
      if (!rest.empty() && !isCamelBack(rest)) {
        diag(Field->getLocation(),
             "class data member '%0' should be _camelCase after the underscore")
            << Name;
      }
    } else {
      diag(Field->getLocation(),
           "class data member '%0' must use _camelCase (underscore prefix)")
          << Name;
    }
  } else {
    // Struct members use camelCase — no underscore prefix.
    // Skip structs with private members (may be class-like).
    if (hasPrivateMembers(Record))
      return;

    if (Name.starts_with("_")) {
      diag(Field->getLocation(),
           "struct data member '%0' should use camelCase without underscore prefix")
          << Name;
    }
  }
}

} // namespace clang::tidy::readability
