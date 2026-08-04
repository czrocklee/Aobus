// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/uimodel/layout/document/LayoutDialect.h>

namespace ao::gtk::layout
{
  /// GTK-specific styling and tooltip rules layered onto shared layout validation.
  uimodel::LayoutDialect const& layoutDialect() noexcept;
} // namespace ao::gtk::layout
