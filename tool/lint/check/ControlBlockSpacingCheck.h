// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <clang-tidy/ClangTidyCheck.h>
#include <clang-tidy/ClangTidyDiagnosticConsumer.h>
#include <clang/AST/ASTContext.h>
#include <clang/AST/Stmt.h>
#include <clang/ASTMatchers/ASTMatchFinder.h>
#include <clang/Basic/LLVM.h>
#include <clang/Basic/SourceLocation.h>
#include <clang/Lex/Lexer.h>
#include <clang/Lex/Token.h>
#include <llvm/ADT/DenseMap.h>
#include <llvm/ADT/DenseSet.h>

#include <cstddef>
#include <vector>

namespace clang::tidy::readability
{
  class ControlBlockSpacingCheck : public ClangTidyCheck
  {
  public:
    ControlBlockSpacingCheck(StringRef name, ClangTidyContext* context)
      : ClangTidyCheck{name, context}
    {
    }

    void registerMatchers(ast_matchers::MatchFinder* finder) override;
    void check(ast_matchers::MatchFinder::MatchResult const& result) override;
    void onEndOfTranslationUnit() override;

  private:
    std::vector<Token> const& getTokens(FileID fid, SourceManager& sm, LangOptions const& langOpts);
    void checkFileOnce(FileID fid, SourceManager& sm, ASTContext& context);
    void checkSpacingBefore(size_t tokenIndex,
                            std::vector<Token> const& tokens,
                            StringRef buffer,
                            SourceManager& sm,
                            StringRef stmtName);
    void checkControlStatement(SourceLocation loc, SourceManager& sm, ASTContext& context, StringRef stmtName);
    void checkBlockStart(SourceLocation loc, SourceManager& sm, ASTContext& context);
    void checkBlockEnd(SourceLocation loc, SourceManager& sm, ASTContext& context);
    void checkSpacingAfterBlock(SourceLocation rBraceLoc, SourceManager& sm, ASTContext& context, bool isDoWhile);

    void handleControlStatement(Stmt const* ctrl,
                                SourceManager& sm,
                                ast_matchers::MatchFinder::MatchResult const& result);
    void handleCompoundBlock(CompoundStmt const* block,
                             SourceManager& sm,
                             ast_matchers::MatchFinder::MatchResult const& result);
    void handleControlBody(CompoundStmt const* ctrlBody,
                           SourceManager& sm,
                           ast_matchers::MatchFinder::MatchResult const& result);

    llvm::DenseMap<FileID, std::vector<Token>> _fileTokens;
    llvm::DenseSet<FileID> _processedFiles;
  };
} // namespace clang::tidy::readability