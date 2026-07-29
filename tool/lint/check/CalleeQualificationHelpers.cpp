// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "CalleeQualificationHelpers.h"

#include <clang/AST/Expr.h>
#include <clang/Basic/SourceLocation.h>
#include <clang/Basic/SourceManager.h>
#include <clang/Lex/Lexer.h>
#include <llvm/ADT/StringRef.h>
#include <llvm/ADT/StringSwitch.h>

#include <optional>

namespace clang::tidy::aobus
{
  bool isCStandardLibraryFunction(llvm::StringRef name)
  {
    return llvm::StringSwitch<bool>{name}
      .Cases({"memcpy", "memmove", "memcmp", "memset"}, true)
      .Cases({"strlen", "strcmp", "strncmp", "strcpy", "strncpy"}, true)
      .Cases({"strcat", "strncat", "strchr", "strrchr", "strstr"}, true)
      .Cases({"abs", "labs", "llabs", "fabs", "fabsf", "fabsl"}, true)
      .Cases({"div", "ldiv", "lldiv"}, true)
      .Cases({"isalnum", "isalpha", "isblank", "iscntrl", "isdigit"}, true)
      .Cases({"isgraph", "islower", "isprint", "ispunct", "isspace"}, true)
      .Cases({"isupper", "isxdigit", "tolower", "toupper"}, true)
      .Cases({"sin", "cos", "tan", "sqrt", "pow", "log", "exp"}, true)
      .Cases({"floor", "ceil", "round", "trunc", "fmod"}, true)
      .Cases({"malloc", "calloc", "realloc", "free"}, true)
      .Cases({"qsort", "bsearch"}, true)
      .Cases({"atoi", "atol", "atoll", "atof"}, true)
      .Cases({"strtol", "strtoul", "strtoll", "strtoull"}, true)
      .Cases({"strtof", "strtod", "strtold"}, true)
      .Cases({"sprintf", "snprintf", "vsprintf", "vsnprintf"}, true)
      .Cases({"fopen", "fclose", "fread", "fwrite", "fseek", "ftell"}, true)
      .Cases({"fprintf", "fscanf", "fgets", "fputs"}, true)
      .Cases({"remove", "rename", "tmpfile", "tmpnam"}, true)
      .Cases({"rand", "srand"}, true)
      .Cases({"clock", "time", "difftime", "mktime", "strftime"}, true)
      .Default(false);
  }

  std::optional<UnqualifiedCallee> getCalleeForQualification(CallExpr const& call,
                                                             SourceManager const& sm,
                                                             LangOptions const& langOpts)
  {
    Expr const* callee = call.getCallee();

    if (callee == nullptr)
    {
      return std::nullopt;
    }

    callee = callee->IgnoreParenCasts();
    SourceLocation const calleeLoc = callee->getBeginLoc();

    if (calleeLoc.isInvalid() || calleeLoc.isMacroID())
    {
      return std::nullopt;
    }

    auto const calleeRange = CharSourceRange::getTokenRange(callee->getSourceRange());
    llvm::StringRef const calleeText = Lexer::getSourceText(calleeRange, sm, langOpts);

    return UnqualifiedCallee{.loc = calleeLoc, .text = calleeText};
  }
} // namespace clang::tidy::aobus
