#pragma once

#include "clang-tidy/ClangTidyCheck.h"
#include "clang/Lex/Lexer.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include <vector>

namespace clang::tidy::readability {

class ControlBlockSpacingCheck : public ClangTidyCheck {
public:
  ControlBlockSpacingCheck(StringRef Name, ClangTidyContext *Context)
      : ClangTidyCheck(Name, Context) {}

  void registerMatchers(ast_matchers::MatchFinder *Finder) override;
  void check(const ast_matchers::MatchFinder::MatchResult &Result) override;
  void onEndOfTranslationUnit() override;

private:
  const std::vector<Token>& getTokens(FileID FID, SourceManager &SM, const LangOptions &LangOpts);
  void checkFileOnce(FileID FID, SourceManager &SM, ASTContext &Context);
  void checkSpacingBefore(int TokenIndex, const std::vector<Token> &Tokens, StringRef Buffer, SourceManager &SM, StringRef StmtName);
  void checkControlStatement(SourceLocation Loc, SourceManager &SM, ASTContext &Context, StringRef StmtName);
  void checkBlockStart(SourceLocation Loc, SourceManager &SM, ASTContext &Context);
  void checkBlockEnd(SourceLocation Loc, SourceManager &SM, ASTContext &Context);

  llvm::DenseMap<FileID, std::vector<Token>> FileTokens;
  llvm::DenseSet<FileID> ProcessedFiles;
};

} // namespace clang::tidy::readability