#include "check/LocalInitializationStyleCheck.h"
#include "clang/AST/ASTContext.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"
#include "clang/Lex/Lexer.h"

using namespace clang::ast_matchers;

namespace clang::tidy::readability {

namespace {

bool isPrimitiveType(QualType type)
{
  type = type.getCanonicalType();
  if (type->isPointerType() || type->isReferenceType())
    return true;
  // Note: We exclude isEnumeralType() so that enums (like std::byte)
  // follow the 'auto x = T{...}' rule for non-primitives.
  return type->isArithmeticType() ||
         type->isAnyCharacterType() || type->isNullPtrType();
}

bool isContainerWithInitializerList(QualType type)
{
  auto const *record = type->getAsCXXRecordDecl();
  if (!record)
    return false;

  StringRef name = record->getName();
  return llvm::StringSwitch<bool>(name)
    .Cases("vector", "deque", "list", "forward_list", true)
    .Cases("set", "multiset", "unordered_set", "unordered_multiset", true)
    .Cases("map", "multimap", "unordered_map", "unordered_multimap", true)
    .Cases("basic_string", "string", true)
    .Default(false);
}

bool isStringOrStringView(QualType type)
{
  auto const *record = type->getAsCXXRecordDecl();
  if (!record)
    return false;

  StringRef name = record->getName();
  return name == "basic_string" || name == "string" || 
         name == "basic_string_view" || name == "string_view";
}

bool hasStringLiteralArg(Expr const* Init)
{
  if (!Init) return false;
  if (auto const* ctor = dyn_cast<CXXConstructExpr>(Init->IgnoreImplicit()))
  {
    if (ctor->getNumArgs() > 0)
    {
      auto const* arg = ctor->getArg(0)->IgnoreImplicit();
      return isa<StringLiteral>(arg);
    }
  }
  return false;
}

} // namespace

void LocalInitializationStyleCheck::registerMatchers(MatchFinder *Finder)
{
  // Match explicit type declarations that use CXXConstructExpr or InitListExpr.
  // We want to skip 'auto x = some_function()' which binds a CallExpr to the VarDecl.
  auto HasBadInit = anyOf(
      hasInitializer(cxxConstructExpr().bind("init")),
      hasInitializer(initListExpr().bind("init"))
  );

  Finder->addMatcher(
    varDecl(
      unless(parmVarDecl()),
      unless(isImplicit()),
      HasBadInit,
      unless(hasType(isAnyCharacter())),
      unless(hasType(qualType(anyOf(
        isInteger(), booleanType(), realFloatingPointType()
      ))))
    ).bind("var"),
    this);

  // Primitive: brace-initialized (Rule 3.4.5: primitives use assignment-style)
  Finder->addMatcher(
    varDecl(
      unless(parmVarDecl()),
      hasInitializer(cxxConstructExpr(isListInitialization()).bind("ctor")),
      hasType(qualType(anyOf(
        isInteger(), booleanType(), realFloatingPointType(),
        isAnyCharacter()
      )))
    ).bind("var"),
    this);
}

void LocalInitializationStyleCheck::check(
  const MatchFinder::MatchResult &Result)
{
  const auto &SM = *Result.SourceManager;
  const auto *Var = Result.Nodes.getNodeAs<VarDecl>("var");
  const auto *Init = Result.Nodes.getNodeAs<Expr>("init");
  const auto *Ctor = Result.Nodes.getNodeAs<CXXConstructExpr>("ctor");

  if (!Var)
    return;

  // Only check local (non-static) variables
  if (!Var->hasLocalStorage() || Var->isStaticLocal())
    return;

  // Skip auto-deduced variables
  if (Var->getType()->isUndeducedType())
    return;
  if (TypeSourceInfo *TSI = Var->getTypeSourceInfo()) {
    if (TSI->getType()->getContainedAutoType())
      return;
  }

  SourceLocation Loc = Var->getLocation();
  if (Loc.isInvalid() || Loc.isMacroID() || SM.isInSystemHeader(Loc))
    return;

  // Skip range-based for loop variables — their init is implicit.
  {
    auto Node = DynTypedNode::create(*Var);
    for (;;)
    {
      auto Parents = Result.Context->getParents(Node);
      if (Parents.empty())
        break;
      Node = Parents[0];
      if (Node.get<CXXForRangeStmt>() || Node.get<LambdaExpr>())
        return;
      if (Node.get<TranslationUnitDecl>())
        break;
    }
  }

  QualType VarType = Var->getType();
  bool Primitive = isPrimitiveType(VarType);

  if (!Primitive)
  {
    bool IsListInit = false;
    bool IsEmptyInit = false;
    bool IsContainer = isContainerWithInitializerList(VarType);
    bool IsString = isStringOrStringView(VarType);
    bool HasLiteral = hasStringLiteralArg(Init);

    if (Init)
    {
      if (auto const *ctor = dyn_cast<CXXConstructExpr>(Init->IgnoreImplicit()))
      {
        // Skip nullptr initialization — T* ptr = nullptr is fine
        if (ctor->getNumArgs() == 1 &&
            ctor->getArg(0)->getType()->isNullPtrType())
          return;

        // Skip lambda closure types
        auto const *ctorDecl = ctor->getConstructor();
        if (ctorDecl)
        {
          auto const *record = ctorDecl->getParent();
          if (record && record->isLambda())
            return;
        }

        IsListInit = ctor->isListInitialization();
        IsEmptyInit = ctor->getNumArgs() == 0;
      }
      else if (isa<InitListExpr>(Init->IgnoreImplicit()))
      {
        IsListInit = true;
      }
    }
    else
    {
      // If there's no initializer, it's default initialization (e.g. `Type var;`)
      IsEmptyInit = true;
    }

    if (IsString && HasLiteral)
    {
      StringRef suffix = VarType.getAsString().find("string_view") != StringRef::npos ? "sv" : "s";
      diag(Loc, "prefer standard literals 'auto %0 = \"...\"%1' over explicit string construction")
        << Var->getName() << suffix;
    }
    else if (IsContainer && !IsListInit && !IsEmptyInit)
    {
      diag(Loc, "use 'auto %0 = Type(...)' for container initialization to avoid ambiguity")
        << Var->getName();
    }
    else
    {
      diag(Loc, "use 'auto %0 = Type{...}' instead of explicit type initialization")
        << Var->getName();
    }
  }
  else if (Ctor)
  {
    // Primitive type with brace initialization — use T x = val instead.
    diag(Loc,
         "primitive type should use assignment-style initialization "
         "'Type %0 = ...', not brace initialization")
      << Var->getName();
  }
}

} // namespace clang::tidy::readability
