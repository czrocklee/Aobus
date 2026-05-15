#include "check/CApiGlobalQualificationCheck.h"
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

void CApiGlobalQualificationCheck::registerMatchers(MatchFinder *Finder)
{
  Finder->addMatcher(
    callExpr(
      callee(declRefExpr(to(functionDecl(isExternC()).bind("func"))))
    ).bind("call"),
    this);
}

void CApiGlobalQualificationCheck::check(
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

  // Only flag functions declared in system headers (not project C-linkage wrappers)
  if (!SM.isInSystemHeader(Func->getLocation()))
    return;

  // Skip C standard library functions — they are Check 2's domain (need std::)
  if (isCStandardLibraryFunction(Func->getName()))
    return;

  // Get the source text of the callee expression
  auto CalleeRange = CharSourceRange::getTokenRange(
    Call->getCallee()->getSourceRange());
  StringRef CalleeText = Lexer::getSourceText(
    CalleeRange, SM, Result.Context->getLangOpts());

  if (CalleeText.starts_with("::"))
    return;

  // If qualified with std:: (incorrect for non-stdlib C API), flag it anyway
  // as it should use :: not std::
  if (CalleeText.starts_with("std::"))
    return;

  diag(Loc,
       "external C library function '%0' must use global qualification '::%0'")
    << Func->getName();
}

} // namespace clang::tidy::readability
