// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <clang/Basic/SourceLocation.h>
#include <llvm/ADT/StringRef.h>

#include <optional>

namespace clang
{
  class CallExpr;
  class LangOptions;
  class SourceManager;
} // namespace clang

namespace clang::tidy::aobus
{
  bool isCStandardLibraryFunction(llvm::StringRef name);

  struct UnqualifiedCallee final
  {
    SourceLocation loc;
    llvm::StringRef text;
  };

  // Unwraps parens/casts around a call's callee and yields the location where a
  // namespace qualifier can be inserted plus the callee's source text. Returns
  // nullopt when the callee sits inside a macro expansion (a FixIt there would
  // edit the macro definition, not the call site).
  std::optional<UnqualifiedCallee> getCalleeForQualification(CallExpr const& call,
                                                             SourceManager const& sm,
                                                             LangOptions const& langOpts);
} // namespace clang::tidy::aobus
