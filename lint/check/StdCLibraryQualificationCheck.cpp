#include "check/StdCLibraryQualificationCheck.h"
#include "clang/AST/ASTContext.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"
#include "clang/Lex/Lexer.h"

using namespace clang::ast_matchers;

namespace clang::tidy::readability {

namespace {

bool isCStandardLibraryFunction(StringRef name)
{
  return llvm::StringSwitch<bool>(name)
    .Cases("memcpy", "memmove", "memcmp", "memset", true)
    .Cases("strlen", "strcmp", "strncmp", "strcpy", "strncpy", true)
    .Cases("strcat", "strncat", "strchr", "strrchr", "strstr", true)
    .Cases("abs", "labs", "llabs", "fabs", "fabsf", "fabsl", true)
    .Cases("div", "ldiv", "lldiv", true)
    .Cases("isalnum", "isalpha", "isblank", "iscntrl", "isdigit", true)
    .Cases("isgraph", "islower", "isprint", "ispunct", "isspace", true)
    .Cases("isupper", "isxdigit", "tolower", "toupper", true)
    .Cases("sin", "cos", "tan", "sqrt", "pow", "log", "exp", true)
    .Cases("floor", "ceil", "round", "trunc", "fmod", true)
    .Cases("malloc", "calloc", "realloc", "free", true)
    .Cases("qsort", "bsearch", true)
    .Cases("atoi", "atol", "atoll", "atof", true)
    .Cases("strtol", "strtoul", "strtoll", "strtoull", true)
    .Cases("strtof", "strtod", "strtold", true)
    .Cases("sprintf", "snprintf", "vsprintf", "vsnprintf", true)
    .Cases("fopen", "fclose", "fread", "fwrite", "fseek", "ftell", true)
    .Cases("fprintf", "fscanf", "fgets", "fputs", true)
    .Cases("remove", "rename", "tmpfile", "tmpnam", true)
    .Cases("rand", "srand", true)
    .Cases("clock", "time", "difftime", "mktime", "strftime", true)
    .Default(false);
}

} // namespace

void StdCLibraryQualificationCheck::registerMatchers(MatchFinder *Finder)
{
  // Match calls to known C standard library function names.
  // The isExternC() filter narrows matches to C-linkage declarations.
  // In check() we verify the declaration is from a system header.
  Finder->addMatcher(
    callExpr(
      callee(declRefExpr(to(functionDecl(
        isExternC(),
        hasAnyName("memcpy", "memmove", "memcmp", "memset",
                   "strlen", "strcmp", "strncmp", "strcpy", "strncpy",
                   "strcat", "strncat", "strchr", "strrchr", "strstr",
                   "abs", "fabs", "malloc", "free", "isalpha", "isdigit",
                   "tolower", "toupper", "sin", "cos", "sqrt", "pow")
      ).bind("func"))))
    ).bind("call"),
    this);
}

void StdCLibraryQualificationCheck::check(
  const MatchFinder::MatchResult &Result)
{
  const auto &SM = *Result.SourceManager;
  const auto *Func = Result.Nodes.getNodeAs<FunctionDecl>("func");
  const auto *Call = Result.Nodes.getNodeAs<CallExpr>("call");

  if (!Func || !Call)
    return;

  SourceLocation Loc = Call->getBeginLoc();
  if (Loc.isInvalid() || Loc.isMacroID() || SM.isInSystemHeader(Loc))
    return;

  // Verify the function is declared in a system header (not a project function
  // that happens to share the same name).
  if (!SM.isInSystemHeader(Func->getLocation()))
    return;

  // Full name check (the matcher only lists the most common ones for performance)
  if (!isCStandardLibraryFunction(Func->getName()))
    return;

  auto CalleeRange = CharSourceRange::getTokenRange(
    Call->getCallee()->getSourceRange());
  StringRef CalleeText = Lexer::getSourceText(
    CalleeRange, SM, Result.Context->getLangOpts());

  if (CalleeText.starts_with("std::"))
    return;

  diag(Loc,
       "C standard library function '%0' should use 'std::%0' via <c...> header")
    << Func->getName();
}

} // namespace clang::tidy::readability
