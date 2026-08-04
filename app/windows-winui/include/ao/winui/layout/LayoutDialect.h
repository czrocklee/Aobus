// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/uimodel/layout/document/LayoutDialect.h>

namespace ao::winui
{
  /**
   * @brief What the Windows shell adds to the shared document rules.
   *
   * Everything the two shells decide the same way - the common layout fields,
   * component cardinality, property kinds, action bindings, id uniqueness - is
   * decided by the shared traversal. This names only what is the Windows
   * shell's own: it styles through XAML resources rather than CSS classes, it
   * paints themed surfaces, its components own their tooltips, and a few of its
   * component types tighten their own rules through an authored presentation.
   */
  uimodel::LayoutDialect const& layoutDialect() noexcept;
} // namespace ao::winui
