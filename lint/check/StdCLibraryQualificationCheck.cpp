// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "check/StdCLibraryQualificationCheck.h"

#include <clang/AST/ASTContext.h>
#include <clang/AST/Decl.h>
#include <clang/AST/Expr.h>
#include <clang/ASTMatchers/ASTMatchFinder.h>
#include <clang/ASTMatchers/ASTMatchers.h>
#include <clang/Basic/LLVM.h>
#include <clang/Basic/SourceLocation.h>
#include <clang/Lex/Lexer.h>
#include <llvm/ADT/StringSwitch.h>

using namespace clang::ast_matchers;

namespace clang::tidy::readability
{
  namespace
  {
    bool isCStandardLibraryFunction(StringRef name)
    {
      return llvm::StringSwitch<bool>{name}
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

  void StdCLibraryQualificationCheck::registerMatchers(MatchFinder* finder)
  {
    // The callee of a function call is an ImplicitCastExpr (FunctionToPointerDecay)
    // wrapping the actual DeclRefExpr to the FunctionDecl.
    finder->addMatcher(
      callExpr(callee(implicitCastExpr(hasSourceExpression(declRefExpr(to(functionDecl(isExternC(),
                                                                                       hasAnyName("memcpy",
                                                                                                  "memmove",
                                                                                                  "memcmp",
                                                                                                  "memset",
                                                                                                  "strlen",
                                                                                                  "strcmp",
                                                                                                  "strncmp",
                                                                                                  "strcpy",
                                                                                                  "strncpy",
                                                                                                  "strcat",
                                                                                                  "strncat",
                                                                                                  "strchr",
                                                                                                  "strrchr",
                                                                                                  "strstr",
                                                                                                  "abs",
                                                                                                  "fabs",
                                                                                                  "malloc",
                                                                                                  "free",
                                                                                                  "isalpha",
                                                                                                  "isdigit",
                                                                                                  "tolower",
                                                                                                  "toupper",
                                                                                                  "sin",
                                                                                                  "cos",
                                                                                                  "sqrt",
                                                                                                  "pow"))
                                                                            .bind("func")))))))
        .bind("call"),
      this);
  }

  void StdCLibraryQualificationCheck::check(MatchFinder::MatchResult const& result)
  {
    auto const& sm = *result.SourceManager;
    auto const* func = result.Nodes.getNodeAs<FunctionDecl>("func");
    auto const* call = result.Nodes.getNodeAs<CallExpr>("call");

    if ((func == nullptr) || (call == nullptr))
    {
      return;
    }

    SourceLocation const loc = call->getBeginLoc();

    if (loc.isInvalid() || loc.isMacroID() || sm.isInSystemHeader(loc))
    {
      return;
    }

    // Skip functions defined in the main file (project-local extern "C" wrappers).
    // Everything else — system headers, third-party libs, project headers — gets flagged.
    if (sm.isInMainFile(func->getLocation()))
    {
      return;
    }

    // Full name check (the matcher only lists the most common ones for performance)
    if (!isCStandardLibraryFunction(func->getName()))
    {
      return;
    }

    auto calleeRange = CharSourceRange::getTokenRange(call->getCallee()->getSourceRange());
    StringRef const calleeText = Lexer::getSourceText(calleeRange, sm, result.Context->getLangOpts());

    if (calleeText.starts_with("std::"))
    {
      return;
    }

    diag(loc, "C standard library function '%0' should use 'std::%0' via <c...> header") << func->getName();
  }
} // namespace clang::tidy::readability
