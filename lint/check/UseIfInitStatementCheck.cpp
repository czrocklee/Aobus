#include "check/UseIfInitStatementCheck.h"
#include "clang/AST/ASTContext.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/Lex/Lexer.h"

using namespace clang::ast_matchers;

namespace clang::tidy::readability {

namespace {

class UsageVisitor : public RecursiveASTVisitor<UsageVisitor> {
public:
  UsageVisitor(const VarDecl *VD) : VD(VD), Found(false) {}

  bool VisitDeclRefExpr(DeclRefExpr *E) {
    if (E->getDecl() == VD) {
      Found = true;
      return false; // Stop traversal
    }
    return true;
  }

  bool found() const { return Found; }

private:
  const VarDecl *VD;
  bool Found;
};

bool isUsedInside(const VarDecl *VD, const Stmt *Target) {
  UsageVisitor Visitor(VD);
  Visitor.TraverseStmt(const_cast<Stmt *>(Target));
  return Visitor.found();
}

bool isUsedAfter(const VarDecl *VD, const CompoundStmt *Block, const Stmt *Target) {
  bool FoundTarget = false;
  for (const auto *S : Block->body()) {
    if (S == Target) {
      FoundTarget = true;
      continue;
    }
    if (FoundTarget) {
      UsageVisitor Visitor(VD);
      Visitor.TraverseStmt(const_cast<Stmt *>(S));
      if (Visitor.found())
        return true;
    }
  }
  return false;
}

} // namespace

void UseIfInitStatementCheck::registerMatchers(MatchFinder *Finder) {
  Finder->addMatcher(
      declStmt(hasParent(compoundStmt().bind("block"))).bind("decl"),
      this);
}

void UseIfInitStatementCheck::check(const MatchFinder::MatchResult &Result) {
  const auto *Block = Result.Nodes.getNodeAs<CompoundStmt>("block");
  const auto *Decl = Result.Nodes.getNodeAs<DeclStmt>("decl");
  const auto &SM = *Result.SourceManager;

  if (!Block || !Decl)
    return;

  if (SM.isInSystemHeader(Decl->getBeginLoc()) || Decl->getBeginLoc().isMacroID())
    return;

  // Ensure DeclStmt has exactly one variable declaration
  if (!Decl->isSingleDecl())
    return;

  const auto *VD = dyn_cast<VarDecl>(Decl->getSingleDecl());
  if (!VD || !VD->hasInit())
    return;

  // Rule: Ignore constexpr or static variables (typically config constants)
  if (VD->isConstexpr() || VD->getStorageClass() == SC_Static)
    return;

  // Rule: Ignore multiline declarations or very long ones to keep it "reasonable"
  SourceRange DeclRange = Decl->getSourceRange();
  if (SM.getSpellingLineNumber(DeclRange.getBegin()) !=
      SM.getSpellingLineNumber(DeclRange.getEnd()))
    return;

  // Find the next statement in the block
  const Stmt *Target = nullptr;
  bool FoundDecl = false;
  for (const auto *S : Block->body()) {
    if (S == Decl) {
      FoundDecl = true;
      continue;
    }
    if (FoundDecl) {
      Target = S;
      break;
    }
  }

  if (!Target)
    return;

  // Check if Target is if or switch without init statement AND without condition variable
  const IfStmt *If = dyn_cast<IfStmt>(Target);
  const SwitchStmt *Switch = dyn_cast<SwitchStmt>(Target);

  if (If) {
    // If it already has an init statement OR a condition variable, don't crowd it.
    if (If->hasInitStorage() || If->getConditionVariable())
      return;
  } else if (Switch) {
    if (Switch->hasInitStorage() || Switch->getConditionVariable())
      return;
  } else {
    return;
  }

  // Ensure the variable is actually used INSIDE the target statement.
  // This prevents swallowing RAII locks that just happen to precede the statement.
  if (!isUsedInside(VD, Target))
    return;

  // Check if variable is used after the target statement
  if (isUsedAfter(VD, Block, Target))
    return;

  // Get declaration text
  StringRef DeclText = Lexer::getSourceText(
      CharSourceRange::getTokenRange(VD->getBeginLoc(), VD->getEndLoc()), SM, Result.Context->getLangOpts());

  // Rule: If the declaration itself is too long, don't merge it.
  if (DeclText.size() > 60)
    return;

  // Everything looks good, issue a warning
  std::string VarName;
  if (isa<DecompositionDecl>(VD)) {
    VarName = "[...]"; // Structured bindings
  } else {
    VarName = "'" + VD->getName().str() + "'";
  }

  auto Diag = diag(Decl->getBeginLoc(),
                   "variable %0 is only used inside the following '%1' "
                   "statement; move its declaration into the init-statement")
              << VarName << (If ? "if" : "switch");

  // Fix-it logic
  SourceLocation StmtBegin = Decl->getBeginLoc();
  SourceLocation StmtEnd = Decl->getEndLoc();
  auto NextTok = Lexer::findNextToken(StmtEnd, SM, Result.Context->getLangOpts());
  SourceLocation RemovalEnd;
  if (NextTok && NextTok->is(tok::semi)) {
    RemovalEnd = NextTok->getEndLoc();
  } else {
    RemovalEnd = Lexer::getLocForEndOfToken(StmtEnd, 0, SM, Result.Context->getLangOpts());
  }

  // Remove the declaration statement
  Diag << FixItHint::CreateRemoval(SourceRange(StmtBegin, RemovalEnd));

  // Insert declaration into target
  SourceLocation InsertLoc;
  if (If) {
    InsertLoc = If->getLParenLoc().getLocWithOffset(1);
  } else if (Switch) {
    InsertLoc = Switch->getLParenLoc().getLocWithOffset(1);
  }

  if (InsertLoc.isInvalid())
    return;

  // Insert the previously fetched declaration text
  Diag << FixItHint::CreateInsertion(InsertLoc, DeclText.str() + "; ");
}

} // namespace clang::tidy::readability
