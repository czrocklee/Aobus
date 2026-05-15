#include "check/MemberOrderCheck.h"
#include "clang/AST/ASTContext.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"

using namespace clang::ast_matchers;

namespace clang::tidy::readability {

namespace {

char const *accessName(AccessSpecifier AS)
{
  switch (AS)
  {
    case AS_public: return "public";
    case AS_protected: return "protected";
    case AS_private: return "private";
    default: return "";
  }
}

} // namespace

void MemberOrderCheck::registerMatchers(MatchFinder *Finder)
{
  Finder->addMatcher(
    cxxRecordDecl(
      isDefinition(),
      unless(isExpansionInSystemHeader()),
      unless(isImplicit())
    ).bind("record"),
    this);
}

void MemberOrderCheck::check(const MatchFinder::MatchResult &Result)
{
  const auto &SM = *Result.SourceManager;
  const auto *Record = Result.Nodes.getNodeAs<CXXRecordDecl>("record");

  if (!Record)
    return;

  SourceLocation Loc = Record->getLocation();
  if (Loc.isInvalid() || Loc.isMacroID() || SM.isInSystemHeader(Loc))
    return;

  // Only check header files (.h / .hpp)
  auto FID = SM.getFileID(Loc);
  auto File = SM.getFileEntryRefForID(FID);
  if (!File)
    return;
  StringRef filename = File->getName();
  if (!filename.ends_with(".h") && !filename.ends_with(".hpp") &&
      !filename.ends_with(".hxx"))
    return;

  // Skip truly empty records (no fields, no methods, no bases)
  if (Record->field_empty() &&
      Record->method_begin() == Record->method_end() &&
      Record->getNumBases() == 0)
    return;

  // class: default is private (2), struct: default is public (0)
  int lastAccessValue = (Record->isClass() && !Record->isStruct()) ? 2 : 0;

  // If the implicit-default section has real members, treat it as an
  // explicit section so that a later, earlier access specifier triggers
  // Rule 2.5.2 (e.g. class { int x_; public: ... }).
  bool sawMemberInImplicit = false;
  for (auto *D : Record->decls())
  {
    if (D->isImplicit() || D == Record) continue;
    if (isa<AccessSpecDecl>(D)) break;
    if (!D->isImplicit() && !isa<AccessSpecDecl>(D))
      { sawMemberInImplicit = true; break; }
  }
  bool sawExplicitAccess = sawMemberInImplicit;

  bool reported = false;

  for (auto *D : Record->decls())
  {
    if (D->isImplicit() || D == Record)
      continue;

    SourceLocation DLOC = D->getLocation();
    if (DLOC.isInvalid() || DLOC.isMacroID())
      continue;

    if (auto const *AS = dyn_cast<AccessSpecDecl>(D))
    {
      int newAccess = static_cast<int>(AS->getAccess());
      if (sawExplicitAccess && newAccess < lastAccessValue && !reported)
      {
        diag(AS->getLocation(),
             "'%0' access section appears after '%1'; "
             "expected public before protected before private (Rule 2.5.2)")
          << accessName(AS->getAccess())
          << accessName(static_cast<AccessSpecifier>(lastAccessValue));
        reported = true;
      }
      if (!sawExplicitAccess)
        lastAccessValue = newAccess;
      else if (newAccess > lastAccessValue)
        lastAccessValue = newAccess;
      sawExplicitAccess = true;
    }
  }
}

} // namespace clang::tidy::readability
