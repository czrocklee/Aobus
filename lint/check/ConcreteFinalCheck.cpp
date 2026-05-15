#include "check/ConcreteFinalCheck.h"
#include "clang/AST/ASTContext.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"
#include "clang/Basic/FileManager.h"

using namespace clang::ast_matchers;

namespace clang::tidy::readability {

namespace {

bool isSafeConcreteCandidate(CXXRecordDecl const *record)
{
  if (!record)
    return false;

  // Skip if already final (should not happen due to matcher, but safe)
  if (record->hasAttr<FinalAttr>())
    return false;

  // Skip if has virtual member functions
  if (record->isPolymorphic())
    return false;

  // Skip if inherits from another class
  if (record->getNumBases() > 0)
    return false;

  // Skip if the class name suggests interface or base
  auto *id = record->getIdentifier();
  if (!id)
    return false;
  StringRef name = id->getName();
  if (name.starts_with("I") && name.size() > 1 && isUppercase(name[1]))
    return false;
  if (name.ends_with("Base"))
    return false;

  // Skip if has protected ctor or dtor — suggests designed for inheritance
  for (auto *ctor : record->ctors())
  {
    if (ctor->getAccess() == AS_protected)
      return false;
  }
  if (auto *dtor = record->getDestructor())
  {
    if (dtor->getAccess() == AS_protected)
      return false;
  }

  // Skip template specializations
  if (record->getTemplateSpecializationKind() != TSK_Undeclared)
    return false;

  return true;
}

} // namespace

void ConcreteFinalCheck::registerMatchers(MatchFinder *Finder)
{
  Finder->addMatcher(
    cxxRecordDecl(
      isDefinition(),
      unless(isFinal()),
      unless(isExpansionInSystemHeader())
    ).bind("record"),
    this);
}

void ConcreteFinalCheck::check(const MatchFinder::MatchResult &Result)
{
  const auto &SM = *Result.SourceManager;
  const auto *Record = Result.Nodes.getNodeAs<CXXRecordDecl>("record");

  if (!Record)
    return;

  SourceLocation Loc = Record->getLocation();
  if (Loc.isInvalid() || Loc.isMacroID() || SM.isInSystemHeader(Loc))
    return;

  // Only check:
  // - types in anonymous namespace
  // - private nested types
  bool InAnonymousNamespace = false;
  bool IsPrivateNested = false;

  auto *DC = Record->getDeclContext();
  if (auto *NS = dyn_cast<NamespaceDecl>(DC))
  {
    if (NS->isAnonymousNamespace())
      InAnonymousNamespace = true;
  }
  else if (DC && isa<CXXRecordDecl>(DC))
  {
    if (Record->getAccess() == AS_private)
      IsPrivateNested = true;
  }

  if (!InAnonymousNamespace && !IsPrivateNested)
    return;

  if (!isSafeConcreteCandidate(Record))
    return;

  auto Diag = diag(Loc,
    "concrete %0 '%1' should be marked 'final'")
    << (Record->isClass() ? "class" : "struct")
    << Record;

  // Provide fix-it: insert 'final' before the opening brace or class body
  if (auto *ClassDef = Record->getDefinition())
  {
    SourceLocation BraceLoc = ClassDef->getBraceRange().getBegin();
    if (BraceLoc.isValid())
    {
      Diag << FixItHint::CreateInsertion(BraceLoc, "final ");
    }
  }
}

} // namespace clang::tidy::readability
