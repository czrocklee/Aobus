// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/Error.h>
#include <ao/uimodel/layout/document/LayoutDialect.h>

#include <optional>
#include <string>

namespace ao::uimodel
{
  class LayoutActionCatalog;
  class LayoutComponentCatalog;
  class PreparedLayout;

  /// The first defect found in document order.
  struct LayoutRejection final
  {
    LayoutRejectionReason reason = LayoutRejectionReason::UnknownComponentType;
    /// Authored node id, or the component type when the node is anonymous.
    std::string nodeId;
    /// Offending field, property, or action id.
    std::string detail;
    std::string message;
  };

  /**
   * @brief Validates a prepared document against a catalog pair and one frontend's dialect.
   *
   * A built-in document is one candidate: the first defect in document order
   * rejects it entirely, so callers never render per-node diagnostic
   * placeholders. Traversal order is fixed, which makes the reported defect
   * deterministic for a given document.
   *
   * @return The rejection, or nullopt when the whole candidate is acceptable.
   */
  std::optional<LayoutRejection> validateLayout(PreparedLayout const& layout,
                                                LayoutComponentCatalog const& components,
                                                LayoutActionCatalog const& actions,
                                                LayoutDialect const& dialect);

  /// The same validation, expressed as the error a rejected candidate reports.
  Result<> requireValidLayout(PreparedLayout const& layout,
                              LayoutComponentCatalog const& components,
                              LayoutActionCatalog const& actions,
                              LayoutDialect const& dialect);

  std::string describeLayoutRejection(LayoutDialect const& dialect, LayoutRejection const& rejection);
} // namespace ao::uimodel
