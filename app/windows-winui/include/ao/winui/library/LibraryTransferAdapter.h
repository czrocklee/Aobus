// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <ao/rt/library/LibraryTransfer.h>

#include <cstdint>
#include <optional>

namespace ao::winui
{
  /// Maps the native export-mode selector's stable row order to runtime policy.
  std::optional<rt::ExportMode> libraryExportModeForSelection(std::int32_t selection) noexcept;

  /// Maps the native import-policy selector's stable row order to runtime policy.
  std::optional<rt::ImportMode> libraryImportModeForSelection(std::int32_t selection) noexcept;

  bool needsLibraryImportDestructiveConfirmation(rt::ImportMode mode) noexcept;
} // namespace ao::winui
