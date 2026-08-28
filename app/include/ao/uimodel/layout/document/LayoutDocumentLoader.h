// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/Error.h>
#include <ao/uimodel/layout/document/LayoutPreparation.h>

#include <string_view>

namespace ao::uimodel
{
  class LayoutSchema;
  struct LayoutDialect;

  /**
   * @brief Turns built-in shell YAML into a validated candidate.
   *
   * Parsing, template expansion, budget limits, and schema validation are one
   * step because a built-in document is one candidate: any defect rejects it
   * entirely rather than degrading part of the shell. @p sourceName only labels
   * diagnostics, and @p dialect names the frontend in them.
   */
  Result<PreparedLayout> prepareShellDocument(std::string_view yaml,
                                              std::string_view sourceName,
                                              LayoutSchema const& schema,
                                              LayoutDialect const& dialect);
} // namespace ao::uimodel
