#pragma once
#include "clang-tidy/ClangTidyCheck.h"
#include "clang-tidy/ClangTidyOptions.h"
#include "clang-tidy/utils/IncludeInserter.h"
#include "clang/Basic/LLVM.h"
#include "clang/Basic/SourceLocation.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/StringSet.h"

namespace clang
{
  class FileID;
  class Preprocessor;
  class SourceManager;
  namespace ast_matchers
  {
    class MatchFinder;
  }
} // namespace clang

namespace clang::tidy
{
  class ClangTidyContext;
} // namespace clang::tidy

namespace clang::tidy::readability
{
  /// Enforces explicit sized integer types (std::int32_t) over plain C types (int, short).
  /// Excludes types in main() and system headers, and traces external C API usage.
  class UseStdNumbersCheck : public ClangTidyCheck
  {
  public:
    UseStdNumbersCheck(StringRef name, ClangTidyContext* context);

    void registerPPCallbacks(SourceManager const& sm, Preprocessor* pp, Preprocessor* moduleExpanderPP) override;
    void registerMatchers(ast_matchers::MatchFinder* finder) override;
    void check(ast_matchers::MatchFinder::MatchResult const& result) override;
    void storeOptions(ClangTidyOptions::OptionMap& opts) override;

  private:
    utils::IncludeInserter _includeInserter;
    llvm::DenseMap<FileID, llvm::StringSet<>> _insertedHeaders;
  };
} // namespace clang::tidy::readability
