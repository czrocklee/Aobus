// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <clang/AST/Decl.h>
#include <clang/Basic/SourceManager.h>

namespace clang::tidy::readability::function_naming
{
  /// Returns whether a function is a named declaration owned by project source.
  bool isProjectOwnedNamedFunction(FunctionDecl const& function, SourceManager const& sourceManager);

  /// Returns the project source declaration that owns an instantiated function.
  FunctionDecl const& sourceFunctionDeclaration(FunctionDecl const& function);

  /// Returns whether a function name is fixed by an external framework contract.
  bool isFrameworkRequiredFunction(FunctionDecl const& function, SourceManager const& sourceManager);
} // namespace clang::tidy::readability::function_naming
