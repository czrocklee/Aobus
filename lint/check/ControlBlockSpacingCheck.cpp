#include "ControlBlockSpacingCheck.h"
#include "clang/AST/ASTContext.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"

using namespace clang::ast_matchers;

namespace clang::tidy::readability {

void ControlBlockSpacingCheck::registerMatchers(MatchFinder *Finder) {
  Finder->addMatcher(
      stmt(anyOf(
          ifStmt(unless(hasParent(ifStmt()))),
          forStmt(),
          cxxForRangeStmt(),
          whileStmt(),
          doStmt(),
          switchStmt()
      )).bind("ctrl_stmt"), this);

  Finder->addMatcher(compoundStmt().bind("block"), this);

  // Match control-block bodies for blank-line-after check
  Finder->addMatcher(
      compoundStmt(hasParent(stmt(anyOf(
          ifStmt(), forStmt(), cxxForRangeStmt(), whileStmt(), doStmt(),
          switchStmt())))).bind("ctrl_body"), this);
}

const std::vector<Token>& ControlBlockSpacingCheck::getTokens(FileID FID, SourceManager &SM, const LangOptions &LangOpts) {
  auto It = FileTokens.find(FID);
  if (It != FileTokens.end()) return It->second;
  
  StringRef Buffer = SM.getBufferData(FID);
  Lexer Lex(SM.getLocForStartOfFile(FID), LangOpts, Buffer.begin(), Buffer.begin(), Buffer.end());
  Lex.SetCommentRetentionState(true);
  
  std::vector<Token> Tokens;
  Token Tok;
  do {
    Lex.LexFromRawLexer(Tok);
    Tokens.push_back(Tok);
  } while (Tok.isNot(tok::eof));
  
  return FileTokens[FID] = std::move(Tokens);
}

void ControlBlockSpacingCheck::onEndOfTranslationUnit() {
  FileTokens.clear();
  ProcessedFiles.clear();
}

void ControlBlockSpacingCheck::checkSpacingBefore(int TokenIndex, const std::vector<Token> &Tokens, StringRef Buffer, SourceManager &SM, StringRef StmtName) {
  int CurrIdx = TokenIndex;
  while (CurrIdx > 0 && Tokens[CurrIdx - 1].is(tok::comment)) {
    unsigned PrevEnd = SM.getFileOffset(Tokens[CurrIdx - 1].getLocation()) + Tokens[CurrIdx - 1].getLength();
    unsigned NextStart = SM.getFileOffset(Tokens[CurrIdx].getLocation());
    StringRef Gap = Buffer.substr(PrevEnd, NextStart - PrevEnd);
    if (Gap.count('\n') >= 2) {
      if (CurrIdx == TokenIndex) {
        // Only fire for line-start comments, skip trailing // at end of code line
        unsigned CommentOffset = SM.getFileOffset(Tokens[CurrIdx - 1].getLocation());
        bool isLineStart = true;
        if (CommentOffset > 0) {
          for (int i = static_cast<int>(CommentOffset) - 1; i >= 0; --i) {
            char c = Buffer[i];
            if (c == '\n' || c == '\r') break;
            if (c != ' ' && c != '\t') { isLineStart = false; break; }
          }
        }
        if (isLineStart) {
          diag(Tokens[CurrIdx].getLocation(), "comment should be directly above '%0' without a blank line") << StmtName;
        }
      }
      break;
    }
    CurrIdx--;
  }

  int CommentStartIndex = CurrIdx;
  int CodePrevIndex = CommentStartIndex - 1;
  if (CodePrevIndex < 0) return;

  Token PrevCodeTok = Tokens[CodePrevIndex];
  unsigned PrevEnd = SM.getFileOffset(PrevCodeTok.getLocation()) + PrevCodeTok.getLength();
  unsigned NextStart = SM.getFileOffset(Tokens[CommentStartIndex].getLocation());
  StringRef Gap = Buffer.substr(PrevEnd, NextStart - PrevEnd);
  int Newlines = Gap.count('\n');

  bool IsBlockStart = PrevCodeTok.is(tok::l_brace) || PrevCodeTok.is(tok::colon);
  
  // When comments precede the statement, they already provide visual separation;
  // only require a blank line when code and comment are on the same line (trailing comment).
  // When no comments precede, require a blank line between code blocks.
  int const NewlinesMin = (CommentStartIndex < TokenIndex) ? 1 : 2;
  if (!IsBlockStart && Newlines < NewlinesMin) {
    diag(Tokens[CommentStartIndex].getLocation(), "expected a blank line before '%0'")
        << StmtName
        << FixItHint::CreateInsertion(Tokens[CommentStartIndex].getLocation(), "\n");
  }
}

void ControlBlockSpacingCheck::checkControlStatement(SourceLocation Loc, SourceManager &SM, ASTContext &Context, StringRef StmtName) {
  unsigned Offset = SM.getFileOffset(Loc);
  FileID FID = SM.getFileID(Loc);
  const auto &Tokens = getTokens(FID, SM, Context.getLangOpts());
  
  auto It = llvm::lower_bound(Tokens, Offset, [&](const Token &T, unsigned Off) {
      return SM.getFileOffset(T.getLocation()) < Off;
  });
  
  if (It == Tokens.end() || SM.getFileOffset(It->getLocation()) != Offset) return;
  int Index = std::distance(Tokens.begin(), It);
  
  checkSpacingBefore(Index, Tokens, SM.getBufferData(FID), SM, StmtName);
}

void ControlBlockSpacingCheck::checkBlockStart(SourceLocation Loc, SourceManager &SM, ASTContext &Context) {
  unsigned Offset = SM.getFileOffset(Loc);
  FileID FID = SM.getFileID(Loc);
  const auto &Tokens = getTokens(FID, SM, Context.getLangOpts());
  
  auto It = llvm::lower_bound(Tokens, Offset, [&](const Token &T, unsigned Off) {
      return SM.getFileOffset(T.getLocation()) < Off;
  });
  if (It == Tokens.end() || SM.getFileOffset(It->getLocation()) != Offset) return;
  int Index = std::distance(Tokens.begin(), It);
  
  if (Index + 1 < static_cast<int>(Tokens.size())) {
    unsigned PrevEnd = Offset + 1;
    unsigned NextStart = SM.getFileOffset(Tokens[Index+1].getLocation());
    StringRef Gap = SM.getBufferData(FID).substr(PrevEnd, NextStart - PrevEnd);
    if (Gap.count('\n') >= 2) {
      size_t first_nl = Gap.find('\n');
      size_t last_nl = Gap.rfind('\n');
      auto DB = diag(Tokens[Index+1].getLocation(), "do not put blank lines at the start of a block");
      if (first_nl != StringRef::npos && last_nl != StringRef::npos && first_nl != last_nl) {
        SourceLocation RemoveStart = Loc.getLocWithOffset(1 + first_nl + 1);
        SourceLocation RemoveEnd = Loc.getLocWithOffset(1 + last_nl + 1);
        DB << FixItHint::CreateRemoval(CharSourceRange::getCharRange(RemoveStart, RemoveEnd));
      }
    }
  }
}

void ControlBlockSpacingCheck::checkBlockEnd(SourceLocation Loc, SourceManager &SM, ASTContext &Context) {
  unsigned Offset = SM.getFileOffset(Loc);
  FileID FID = SM.getFileID(Loc);
  const auto &Tokens = getTokens(FID, SM, Context.getLangOpts());
  
  auto It = llvm::lower_bound(Tokens, Offset, [&](const Token &T, unsigned Off) {
      return SM.getFileOffset(T.getLocation()) < Off;
  });
  if (It == Tokens.end() || SM.getFileOffset(It->getLocation()) != Offset) return;
  int Index = std::distance(Tokens.begin(), It);
  
  if (Index > 0) {
    unsigned PrevEnd = SM.getFileOffset(Tokens[Index-1].getLocation()) + Tokens[Index-1].getLength();
    unsigned NextStart = Offset;
    StringRef Gap = SM.getBufferData(FID).substr(PrevEnd, NextStart - PrevEnd);
    if (Gap.count('\n') >= 2) {
      size_t first_nl = Gap.find('\n');
      size_t last_nl = Gap.rfind('\n');
      auto DB = diag(Loc, "do not put blank lines at the end of a block");
      if (first_nl != StringRef::npos && last_nl != StringRef::npos && first_nl != last_nl) {
        SourceLocation RemoveStart = Tokens[Index-1].getLocation().getLocWithOffset(Tokens[Index-1].getLength() + first_nl + 1);
        SourceLocation RemoveEnd = Tokens[Index-1].getLocation().getLocWithOffset(Tokens[Index-1].getLength() + last_nl + 1);
        DB << FixItHint::CreateRemoval(CharSourceRange::getCharRange(RemoveStart, RemoveEnd));
      }
    }
  }
}

void ControlBlockSpacingCheck::checkFileOnce(FileID FID, SourceManager &SM, ASTContext &Context) {
  if (!ProcessedFiles.insert(FID).second) return;
  
  const auto &Tokens = getTokens(FID, SM, Context.getLangOpts());
  StringRef Buffer = SM.getBufferData(FID);
  for (size_t i = 1; i < Tokens.size(); ++i) {
    if (Tokens[i].is(tok::raw_identifier)) {
      StringRef Name = Tokens[i].getRawIdentifier();
      if (Name == "TEST_CASE" || Name == "SECTION") {
        checkSpacingBefore(i, Tokens, Buffer, SM, Name);
      }
    }
  }
}

void ControlBlockSpacingCheck::checkSpacingAfterBlock(SourceLocation RBraceLoc, SourceManager &SM, ASTContext &Context) {
  unsigned Offset = SM.getFileOffset(RBraceLoc);
  FileID FID = SM.getFileID(RBraceLoc);
  const auto &Tokens = getTokens(FID, SM, Context.getLangOpts());
  StringRef Buffer = SM.getBufferData(FID);

  auto It = llvm::lower_bound(Tokens, Offset, [&](const Token &T, unsigned Off) {
      return SM.getFileOffset(T.getLocation()) < Off;
  });

  if (It == Tokens.end() || SM.getFileOffset(It->getLocation()) != Offset)
    return;

  int RBraceIdx = std::distance(Tokens.begin(), It);
  int NextIdx = RBraceIdx + 1;
  if (NextIdx >= static_cast<int>(Tokens.size()))
    return;

  // Skip tokens on same line as `}` (trailing comment, semicolon for do-while)
  unsigned RBraceLine = SM.getSpellingLineNumber(RBraceLoc);
  while (NextIdx < static_cast<int>(Tokens.size()) &&
         SM.getSpellingLineNumber(Tokens[NextIdx].getLocation()) == RBraceLine) {
    NextIdx++;
  }
  if (NextIdx >= static_cast<int>(Tokens.size()))
    return;

  // If next token is `while` (do-while), skip it and its line too, then check after
  if (Tokens[NextIdx].is(tok::raw_identifier) &&
      Tokens[NextIdx].getRawIdentifier() == "while") {
    NextIdx++;
    unsigned WhileLine = SM.getSpellingLineNumber(Tokens[NextIdx - 1].getLocation());
    while (NextIdx < static_cast<int>(Tokens.size()) &&
           SM.getSpellingLineNumber(Tokens[NextIdx].getLocation()) == WhileLine) {
      // Skip the while(...); line
      if (Tokens[NextIdx].is(tok::semi))
        break;
      NextIdx++;
    }
    if (Tokens[NextIdx].is(tok::semi))
      NextIdx++; // skip the ;
    // Now find first token on next line
    if (NextIdx >= static_cast<int>(Tokens.size()))
      return;
    unsigned NextLine = SM.getSpellingLineNumber(Tokens[NextIdx].getLocation());
    while (NextIdx < static_cast<int>(Tokens.size()) &&
           SM.getSpellingLineNumber(Tokens[NextIdx].getLocation()) == NextLine &&
           (Tokens[NextIdx].is(tok::comment) ||
            SM.getSpellingLineNumber(Tokens[NextIdx].getLocation()) == NextLine)) {
      if (SM.getSpellingLineNumber(Tokens[NextIdx].getLocation()) != NextLine)
        break;
      NextIdx++;
    }
  }

  if (NextIdx >= static_cast<int>(Tokens.size()))
    return;

  // Skip leading comments that follow (they may be for the next statement)
  int CodeIdx = NextIdx;
  while (CodeIdx < static_cast<int>(Tokens.size()) &&
         Tokens[CodeIdx].is(tok::comment)) {
    CodeIdx++;
    // Move past the comment line
    unsigned CommentLine = SM.getSpellingLineNumber(Tokens[CodeIdx - 1].getLocation());
    while (CodeIdx < static_cast<int>(Tokens.size()) &&
           SM.getSpellingLineNumber(Tokens[CodeIdx].getLocation()) == CommentLine) {
      CodeIdx++;
    }
  }

  if (CodeIdx >= static_cast<int>(Tokens.size()) ||
      Tokens[CodeIdx].is(tok::r_brace))
    return;

  // Now count newlines between `}` line's end and the next code
  unsigned RBraceEndOffset = SM.getFileOffset(RBraceLoc) + 1;
  unsigned NextCodeStart = SM.getFileOffset(Tokens[CodeIdx].getLocation());
  StringRef Gap = Buffer.substr(RBraceEndOffset, NextCodeStart - RBraceEndOffset);

  int Newlines = Gap.count('\n');
  if (Newlines < 2) {
    // If there's a comment directly above the next code (for the next code),
    // a single newline between `}` and that comment is acceptable
    if (Newlines >= 1 && CodeIdx > NextIdx && Tokens[CodeIdx - 1].is(tok::comment))
      return;

    auto DB = diag(RBraceLoc,
                   "expected a blank line after closing '}' of control block");
    DB << FixItHint::CreateInsertion(Tokens[CodeIdx].getLocation(), "\n");
  }
}

void ControlBlockSpacingCheck::check(const MatchFinder::MatchResult &Result) {
  SourceManager &SM = *Result.SourceManager;

  if (const auto *Ctrl = Result.Nodes.getNodeAs<Stmt>("ctrl_stmt")) {
    SourceLocation Loc = Ctrl->getBeginLoc();
    if (Loc.isInvalid() || Loc.isMacroID() || SM.isInSystemHeader(Loc))
      return;
      
    StringRef StmtName = "control statement";
    if (isa<IfStmt>(Ctrl)) StmtName = "if";
    else if (isa<ForStmt>(Ctrl) || isa<CXXForRangeStmt>(Ctrl)) StmtName = "for";
    else if (isa<WhileStmt>(Ctrl)) StmtName = "while";
    else if (isa<DoStmt>(Ctrl)) StmtName = "do";
    else if (isa<SwitchStmt>(Ctrl)) StmtName = "switch";

    checkControlStatement(Loc, SM, *Result.Context, StmtName);
    checkFileOnce(SM.getFileID(Loc), SM, *Result.Context);
  }

  if (const auto *Block = Result.Nodes.getNodeAs<CompoundStmt>("block")) {
    SourceLocation LBraceLoc = Block->getLBracLoc();
    SourceLocation RBraceLoc = Block->getRBracLoc();
    if (!LBraceLoc.isInvalid() && !LBraceLoc.isMacroID() && !SM.isInSystemHeader(LBraceLoc)) {
      checkBlockStart(LBraceLoc, SM, *Result.Context);
      checkFileOnce(SM.getFileID(LBraceLoc), SM, *Result.Context);
    }
    if (!RBraceLoc.isInvalid() && !RBraceLoc.isMacroID() && !SM.isInSystemHeader(RBraceLoc)) {
      checkBlockEnd(RBraceLoc, SM, *Result.Context);
    }
  }

  if (const auto *CtrlBody = Result.Nodes.getNodeAs<CompoundStmt>("ctrl_body")) {
    SourceLocation RBraceLoc = CtrlBody->getRBracLoc();
    if (!RBraceLoc.isInvalid() && !RBraceLoc.isMacroID() && !SM.isInSystemHeader(RBraceLoc)) {
      // Skip blank-line-after check when the `}` is directly followed by
      // `else` or `else if` — only the final branch closure needs spacing.
      bool IsIntermediateBranch = false;
      for (auto &Parent : Result.Context->getParents(*CtrlBody)) {
        if (const auto *ParentIf = Parent.get<IfStmt>()) {
          if (ParentIf->getThen() == CtrlBody && ParentIf->getElse() != nullptr)
            IsIntermediateBranch = true;
        }
      }
      if (!IsIntermediateBranch)
        checkSpacingAfterBlock(RBraceLoc, SM, *Result.Context);
    }
  }
}

} // namespace clang::tidy::readability