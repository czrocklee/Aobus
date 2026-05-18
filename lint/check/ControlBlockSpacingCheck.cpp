#include "ControlBlockSpacingCheck.h"

#include <clang-tidy/ClangTidyCheck.h>
#include <clang/AST/ASTContext.h>
#include <clang/AST/Stmt.h>
#include <clang/AST/StmtCXX.h>
#include <clang/ASTMatchers/ASTMatchFinder.h>
#include <clang/ASTMatchers/ASTMatchers.h>
#include <clang/Basic/Diagnostic.h>
#include <clang/Basic/LLVM.h>
#include <clang/Basic/SourceLocation.h>
#include <clang/Basic/SourceManager.h>
#include <clang/Basic/TokenKinds.h>
#include <clang/Lex/Lexer.h>
#include <clang/Lex/Token.h>
#include <llvm/ADT/STLExtras.h>
#include <llvm/ADT/StringRef.h>
#include <llvm/Support/Casting.h>

#include <cstddef>
#include <iterator>
#include <utility>
#include <vector>

using namespace clang::ast_matchers;

namespace clang::tidy::readability
{
  namespace
  {
    bool isCommentAtLineStart(unsigned commentOffset, StringRef buffer)
    {
      if (commentOffset == 0)
      {
        return true;
      }

      for (int i = static_cast<int>(commentOffset) - 1; i >= 0; --i)
      {
        char const ch = buffer[static_cast<size_t>(i)];

        if (ch == '\n' || ch == '\r')
        {
          break;
        }

        if (ch != ' ' && ch != '\t')
        {
          return false;
        }
      }

      return true;
    }

    bool checkCommentGapAndLineStart(size_t tokenIndex,
                                     std::vector<Token> const& tokens,
                                     StringRef buffer,
                                     SourceManager& sm,
                                     StringRef stmtName,
                                     ClangTidyCheck& check,
                                     size_t& commentStartIndex)
    {
      size_t currIdx = tokenIndex;
      bool hasFired = false;

      while (currIdx > 0 && tokens[currIdx - 1].is(tok::comment))
      {
        auto const& prevToken = tokens[currIdx - 1];
        auto const& currToken = tokens[currIdx];
        unsigned const prevEnd = sm.getFileOffset(prevToken.getLocation()) + prevToken.getLength();
        unsigned const nextStart = sm.getFileOffset(currToken.getLocation());

        if (StringRef const gap = buffer.substr(prevEnd, nextStart - prevEnd); gap.count('\n') >= 2)
        {
          if (currIdx == tokenIndex)
          {
            unsigned const commentOffset = sm.getFileOffset(prevToken.getLocation());

            if (isCommentAtLineStart(commentOffset, buffer))
            {
              check.diag(currToken.getLocation(), "comment should be directly above '%0' without a blank line")
                << stmtName;
              hasFired = true;
            }
          }

          break;
        }

        currIdx--;
      }

      commentStartIndex = currIdx;

      return hasFired;
    }

    size_t skipSameLineTokens(size_t startIndex,
                              std::vector<Token> const& tokens,
                              SourceManager const& sm,
                              unsigned line)
    {
      size_t idx = startIndex;

      while (idx < tokens.size() && sm.getSpellingLineNumber(tokens[idx].getLocation()) == line)
      {
        idx++;
      }

      return idx;
    }

    size_t skipDoWhileCondition(size_t nextIdx, std::vector<Token> const& tokens, SourceManager const& sm)
    {
      size_t idx = nextIdx;

      if (idx < tokens.size() && tokens[idx].is(tok::raw_identifier) && tokens[idx].getRawIdentifier() == "while")
      {
        idx++;
        unsigned const whileLine = sm.getSpellingLineNumber(tokens[idx - 1].getLocation());

        while (idx < tokens.size() && sm.getSpellingLineNumber(tokens[idx].getLocation()) == whileLine)
        {
          if (tokens[idx].is(tok::semi))
          {
            break;
          }

          idx++;
        }

        if (idx < tokens.size() && tokens[idx].is(tok::semi))
        {
          idx++;
        }

        if (idx < tokens.size())
        {
          unsigned const nextLine = sm.getSpellingLineNumber(tokens[idx].getLocation());

          while (idx < tokens.size() && sm.getSpellingLineNumber(tokens[idx].getLocation()) == nextLine &&
                 (tokens[idx].is(tok::comment) || sm.getSpellingLineNumber(tokens[idx].getLocation()) == nextLine))
          {
            if (sm.getSpellingLineNumber(tokens[idx].getLocation()) != nextLine)
            {
              break;
            }

            idx++;
          }
        }
      }

      return idx;
    }

    size_t skipLeadingComments(size_t nextIdx, std::vector<Token> const& tokens, SourceManager const& sm)
    {
      size_t codeIdx = nextIdx;

      while (codeIdx < tokens.size() && tokens[codeIdx].is(tok::comment))
      {
        codeIdx++;
        unsigned const commentLine = sm.getSpellingLineNumber(tokens[codeIdx - 1].getLocation());

        while (codeIdx < tokens.size() && sm.getSpellingLineNumber(tokens[codeIdx].getLocation()) == commentLine)
        {
          codeIdx++;
        }
      }

      return codeIdx;
    }
  } // namespace

  void ControlBlockSpacingCheck::registerMatchers(MatchFinder* finder)
  {
    finder->addMatcher(stmt(anyOf(ifStmt(unless(hasParent(ifStmt()))),
                                  forStmt(),
                                  cxxForRangeStmt(),
                                  whileStmt(),
                                  doStmt(),
                                  switchStmt(),
                                  cxxTryStmt(),
                                  cxxCatchStmt()))
                         .bind("ctrl_stmt"),
                       this);

    finder->addMatcher(compoundStmt().bind("block"), this);

    // Match control-block bodies for blank-line-after check
    finder->addMatcher(
      compoundStmt(
        hasParent(stmt(anyOf(
          ifStmt(), forStmt(), cxxForRangeStmt(), whileStmt(), doStmt(), switchStmt(), cxxTryStmt(), cxxCatchStmt()))))
        .bind("ctrl_body"),
      this);
  }

  std::vector<Token> const& ControlBlockSpacingCheck::getTokens(FileID fid,
                                                                SourceManager& sm,
                                                                LangOptions const& langOpts)
  {
    if (auto const it = _fileTokens.find(fid); it != _fileTokens.end())
    {
      return it->second;
    }

    StringRef const buffer = sm.getBufferData(fid);
    auto lex = Lexer{sm.getLocForStartOfFile(fid), langOpts, buffer.begin(), buffer.begin(), buffer.end()};
    lex.SetCommentRetentionState(true);

    auto tokens = std::vector<Token>{};
    auto tok = Token{};

    while (true)
    {
      lex.LexFromRawLexer(tok);
      tokens.push_back(tok);

      if (tok.is(tok::eof))
      {
        break;
      }
    }

    return _fileTokens[fid] = std::move(tokens);
  }

  void ControlBlockSpacingCheck::onEndOfTranslationUnit()
  {
    _fileTokens.clear();
    _processedFiles.clear();
  }

  void ControlBlockSpacingCheck::checkSpacingBefore(size_t tokenIndex,
                                                    std::vector<Token> const& tokens,
                                                    StringRef buffer,
                                                    SourceManager& sm,
                                                    StringRef stmtName)
  {
    size_t commentStartIndex = 0;
    checkCommentGapAndLineStart(tokenIndex, tokens, buffer, sm, stmtName, *this, commentStartIndex);

    if (commentStartIndex > 0)
    {
      size_t const codePrevIndex = commentStartIndex - 1;
      auto const prevCodeTok = Token{tokens[codePrevIndex]};
      unsigned const prevEnd = sm.getFileOffset(prevCodeTok.getLocation()) + prevCodeTok.getLength();
      auto const nextToken = Token{tokens[commentStartIndex]};
      unsigned const nextStart = sm.getFileOffset(nextToken.getLocation());
      StringRef const gap = buffer.substr(prevEnd, nextStart - prevEnd);
      int const newlines = static_cast<int>(gap.count('\n'));

      bool const isBlockStart = prevCodeTok.is(tok::l_brace) || prevCodeTok.is(tok::colon);

      // When comments precede the statement, they already provide visual separation;
      // only require a blank line when code and comment are on the same line (trailing comment).
      // When no comments precede, require a blank line between code blocks.
      int const newlinesMin = (commentStartIndex < tokenIndex) ? 1 : 2;

      bool const isChainAfterBrace = (stmtName == "catch" || stmtName == "try") && prevCodeTok.is(tok::r_brace);

      if (!isBlockStart && !isChainAfterBrace && newlines < newlinesMin)
      {
        diag(nextToken.getLocation(), "expected a blank line before '%0'")
          << stmtName << FixItHint::CreateInsertion(nextToken.getLocation(), "\n");
      }
    }
  }

  void ControlBlockSpacingCheck::checkControlStatement(SourceLocation loc,
                                                       SourceManager& sm,
                                                       ASTContext& context,
                                                       StringRef stmtName)
  {
    unsigned const offset = sm.getFileOffset(loc);
    FileID const fid = sm.getFileID(loc);
    auto const& tokens = getTokens(fid, sm, context.getLangOpts());

    auto const it = llvm::lower_bound(
      tokens, offset, [&](Token const& token, unsigned off) { return sm.getFileOffset(token.getLocation()) < off; });

    if (it == tokens.end() || sm.getFileOffset(it->getLocation()) != offset)
    {
      return;
    }

    size_t const index = static_cast<size_t>(std::distance(tokens.begin(), it));

    checkSpacingBefore(index, tokens, sm.getBufferData(fid), sm, stmtName);
  }

  void ControlBlockSpacingCheck::checkBlockStart(SourceLocation loc, SourceManager& sm, ASTContext& context)
  {
    unsigned const offset = sm.getFileOffset(loc);
    FileID const fid = sm.getFileID(loc);
    auto const& tokens = getTokens(fid, sm, context.getLangOpts());

    auto const it = llvm::lower_bound(
      tokens, offset, [&](Token const& token, unsigned off) { return sm.getFileOffset(token.getLocation()) < off; });

    if (it == tokens.end() || sm.getFileOffset(it->getLocation()) != offset)
    {
      return;
    }

    size_t const index = static_cast<size_t>(std::distance(tokens.begin(), it));

    if (index + 1 < tokens.size())
    {
      unsigned const prevEnd = offset + 1;
      auto const& nextToken = tokens[index + 1];
      unsigned const nextStart = sm.getFileOffset(nextToken.getLocation());
      StringRef const gap = sm.getBufferData(fid).substr(prevEnd, nextStart - prevEnd);

      if (gap.count('\n') >= 2)
      {
        size_t const firstNl = gap.find('\n');
        size_t const lastNl = gap.rfind('\n');
        auto db = diag(nextToken.getLocation(), "do not put blank lines at the start of a block");

        if (firstNl != StringRef::npos && lastNl != StringRef::npos && firstNl != lastNl)
        {
          SourceLocation const removeStart = loc.getLocWithOffset(1 + static_cast<int>(firstNl) + 1);
          SourceLocation const removeEnd = loc.getLocWithOffset(1 + static_cast<int>(lastNl) + 1);
          db << FixItHint::CreateRemoval(CharSourceRange::getCharRange(removeStart, removeEnd));
        }
      }
    }
  }

  void ControlBlockSpacingCheck::checkBlockEnd(SourceLocation loc, SourceManager& sm, ASTContext& context)
  {
    unsigned const offset = sm.getFileOffset(loc);
    FileID const fid = sm.getFileID(loc);
    auto const& tokens = getTokens(fid, sm, context.getLangOpts());

    auto const it = llvm::lower_bound(
      tokens, offset, [&](Token const& token, unsigned off) { return sm.getFileOffset(token.getLocation()) < off; });

    if (it == tokens.end() || sm.getFileOffset(it->getLocation()) != offset)
    {
      return;
    }

    size_t const index = static_cast<size_t>(std::distance(tokens.begin(), it));

    if (index > 0)
    {
      auto const& prevToken = tokens[index - 1];
      unsigned const prevEnd = sm.getFileOffset(prevToken.getLocation()) + prevToken.getLength();
      unsigned const nextStart = offset;
      StringRef const gap = sm.getBufferData(fid).substr(prevEnd, nextStart - prevEnd);

      if (gap.count('\n') >= 2)
      {
        size_t const firstNl = gap.find('\n');
        size_t const lastNl = gap.rfind('\n');
        auto db = diag(loc, "do not put blank lines at the end of a block");

        if (firstNl != StringRef::npos && lastNl != StringRef::npos && firstNl != lastNl)
        {
          SourceLocation const removeStart = prevToken.getLocation().getLocWithOffset(
            static_cast<int>(prevToken.getLength()) + static_cast<int>(firstNl) + 1);
          SourceLocation const removeEnd = prevToken.getLocation().getLocWithOffset(
            static_cast<int>(prevToken.getLength()) + static_cast<int>(lastNl) + 1);
          db << FixItHint::CreateRemoval(CharSourceRange::getCharRange(removeStart, removeEnd));
        }
      }
    }
  }

  void ControlBlockSpacingCheck::checkFileOnce(FileID fid, SourceManager& sm, ASTContext& context)
  {
    if (!_processedFiles.insert(fid).second)
    {
      return;
    }

    auto const& tokens = getTokens(fid, sm, context.getLangOpts());
    StringRef const buffer = sm.getBufferData(fid);

    for (size_t i = 1; i < tokens.size(); ++i)
    {
      if (tokens[i].is(tok::raw_identifier))
      {
        if (StringRef const name = tokens[i].getRawIdentifier(); name == "TEST_CASE" || name == "SECTION")
        {
          checkSpacingBefore(i, tokens, buffer, sm, name);
        }
      }
    }
  }

  void ControlBlockSpacingCheck::checkSpacingAfterBlock(SourceLocation rBraceLoc,
                                                        SourceManager& sm,
                                                        ASTContext& context)
  {
    unsigned const offset = sm.getFileOffset(rBraceLoc);
    FileID const fid = sm.getFileID(rBraceLoc);
    auto const& tokens = getTokens(fid, sm, context.getLangOpts());
    StringRef const buffer = sm.getBufferData(fid);

    auto const it = llvm::lower_bound(
      tokens, offset, [&](Token const& token, unsigned off) { return sm.getFileOffset(token.getLocation()) < off; });

    if (it == tokens.end() || sm.getFileOffset(it->getLocation()) != offset)
    {
      return;
    }

    size_t const rBraceIdx = static_cast<size_t>(std::distance(tokens.begin(), it));
    unsigned const rBraceLine = sm.getSpellingLineNumber(rBraceLoc);

    size_t nextIdx = skipSameLineTokens(rBraceIdx + 1, tokens, sm, rBraceLine);

    if (nextIdx >= tokens.size())
    {
      return;
    }

    nextIdx = skipDoWhileCondition(nextIdx, tokens, sm);

    if (nextIdx >= tokens.size())
    {
      return;
    }

    size_t const codeIdx = skipLeadingComments(nextIdx, tokens, sm);

    if (codeIdx >= tokens.size() || tokens[codeIdx].is(tok::r_brace))
    {
      return;
    }

    unsigned const rBraceEndOffset = sm.getFileOffset(rBraceLoc) + 1;
    unsigned const nextCodeStart = sm.getFileOffset(tokens[codeIdx].getLocation());
    StringRef const gap = buffer.substr(rBraceEndOffset, nextCodeStart - rBraceEndOffset);

    if (int const newlines = static_cast<int>(gap.count('\n')); newlines < 2)
    {
      if (newlines >= 1 && codeIdx > nextIdx && tokens[codeIdx - 1].is(tok::comment))
      {
        return;
      }

      auto db = diag(rBraceLoc, "expected a blank line after closing '}' of control block");
      db << FixItHint::CreateInsertion(tokens[codeIdx].getLocation(), "\n");
    }
  }

  void ControlBlockSpacingCheck::check(MatchFinder::MatchResult const& result)
  {
    SourceManager& sm = *result.SourceManager;

    if (auto const* ctrl = result.Nodes.getNodeAs<Stmt>("ctrl_stmt"))
    {
      handleControlStatement(ctrl, sm, result);
    }

    if (auto const* block = result.Nodes.getNodeAs<CompoundStmt>("block"))
    {
      handleCompoundBlock(block, sm, result);
    }

    if (auto const* ctrlBody = result.Nodes.getNodeAs<CompoundStmt>("ctrl_body"))
    {
      handleControlBody(ctrlBody, sm, result);
    }
  }

  void ControlBlockSpacingCheck::handleControlStatement(Stmt const* ctrl,
                                                        SourceManager& sm,
                                                        MatchFinder::MatchResult const& result)
  {
    SourceLocation const loc = ctrl->getBeginLoc();

    if (loc.isInvalid() || loc.isMacroID() || sm.isInSystemHeader(loc))
    {
      return;
    }

    auto stmtName = StringRef{"control statement"};

    if (llvm::isa<IfStmt>(ctrl))
    {
      stmtName = "if";
    }
    else if (llvm::isa<ForStmt>(ctrl) || llvm::isa<CXXForRangeStmt>(ctrl))
    {
      stmtName = "for";
    }
    else if (llvm::isa<WhileStmt>(ctrl))
    {
      stmtName = "while";
    }
    else if (llvm::isa<DoStmt>(ctrl))
    {
      stmtName = "do";
    }
    else if (llvm::isa<SwitchStmt>(ctrl))
    {
      stmtName = "switch";
    }
    else if (llvm::isa<CXXTryStmt>(ctrl))
    {
      stmtName = "try";
    }
    else if (llvm::isa<CXXCatchStmt>(ctrl))
    {
      stmtName = "catch";
    }

    checkControlStatement(loc, sm, *result.Context, stmtName);
    checkFileOnce(sm.getFileID(loc), sm, *result.Context);
  }

  void ControlBlockSpacingCheck::handleCompoundBlock(CompoundStmt const* block,
                                                     SourceManager& sm,
                                                     MatchFinder::MatchResult const& result)
  {
    SourceLocation const lBraceLoc = block->getLBracLoc();
    SourceLocation const rBraceLoc = block->getRBracLoc();

    if (!lBraceLoc.isInvalid() && !lBraceLoc.isMacroID() && !sm.isInSystemHeader(lBraceLoc))
    {
      checkBlockStart(lBraceLoc, sm, *result.Context);
      checkFileOnce(sm.getFileID(lBraceLoc), sm, *result.Context);
    }

    if (!rBraceLoc.isInvalid() && !rBraceLoc.isMacroID() && !sm.isInSystemHeader(rBraceLoc))
    {
      checkBlockEnd(rBraceLoc, sm, *result.Context);
    }
  }

  void ControlBlockSpacingCheck::handleControlBody(CompoundStmt const* ctrlBody,
                                                   SourceManager& sm,
                                                   MatchFinder::MatchResult const& result)
  {
    SourceLocation const rBraceLoc = ctrlBody->getRBracLoc();

    if (rBraceLoc.isInvalid() || rBraceLoc.isMacroID() || sm.isInSystemHeader(rBraceLoc))
    {
      return;
    }

    // Skip blank-line-after check when the `}` is directly followed by
    // `else` or `else if` — only the final branch closure needs spacing.
    bool isIntermediateBranch = false;

    for (auto const& parent : result.Context->getParents(*ctrlBody))
    {
      if (auto const* parentIf = parent.get<IfStmt>())
      {
        if (parentIf->getThen() == ctrlBody && parentIf->getElse() != nullptr)
        {
          isIntermediateBranch = true;
        }
      }
      else if (auto const* parentTry = parent.get<CXXTryStmt>())
      {
        if (parentTry->getTryBlock() == ctrlBody && parentTry->getNumHandlers() > 0)
        {
          isIntermediateBranch = true;
        }
      }
      else if (auto const* parentCatch = parent.get<CXXCatchStmt>())
      {
        for (auto const& grandparent : result.Context->getParents(*parentCatch))
        {
          if (auto const* parentTry = grandparent.get<CXXTryStmt>())
          {
            if (unsigned const numHandlers = parentTry->getNumHandlers();
                numHandlers > 0 && parentTry->getHandler(numHandlers - 1) != parentCatch)
            {
              isIntermediateBranch = true;
            }
          }
        }
      }
    }

    if (!isIntermediateBranch)
    {
      checkSpacingAfterBlock(rBraceLoc, sm, *result.Context);
    }
  }
} // namespace clang::tidy::readability