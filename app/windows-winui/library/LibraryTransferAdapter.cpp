// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include <ao/winui/library/LibraryTransferAdapter.h>

#include <ao/rt/library/LibraryTransfer.h>
#include <ao/uimodel/library/presentation/LibraryTransferPresentation.h>

#include <cstddef>
#include <cstdint>
#include <optional>

namespace ao::winui
{
  std::optional<rt::ExportMode> libraryExportModeForSelection(std::int32_t const selection) noexcept
  {
    if (selection < 0 || static_cast<std::size_t>(selection) >= uimodel::kLibraryExportOptions.size())
    {
      return std::nullopt;
    }

    return uimodel::kLibraryExportOptions[static_cast<std::size_t>(selection)].mode;
  }

  std::optional<rt::ImportMode> libraryImportModeForSelection(std::int32_t const selection) noexcept
  {
    switch (selection)
    {
      case 0: return rt::ImportMode::Merge;
      case 1: return rt::ImportMode::Restore;
      default: return std::nullopt;
    }
  }

  bool needsLibraryImportDestructiveConfirmation(rt::ImportMode const mode) noexcept
  {
    return mode == rt::ImportMode::Restore;
  }
} // namespace ao::winui
