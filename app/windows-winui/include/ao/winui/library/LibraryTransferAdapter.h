// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <ao/i18n/MessageCatalog.h>
#include <ao/rt/library/LibraryYamlExporter.h>
#include <ao/rt/library/LibraryYamlImporter.h>

#include <cstdint>
#include <optional>
#include <string>

namespace ao::winui
{
  struct LibraryRestorePreviewState final
  {
    std::string title{};
    std::string message{};
    std::string primaryActionText{};

    bool operator==(LibraryRestorePreviewState const&) const = default;
  };

  /// Maps the native export-mode selector's stable row order to runtime policy.
  std::optional<rt::ExportMode> libraryExportModeForSelection(std::int32_t selection) noexcept;

  /// Maps the native import-policy selector's stable row order to runtime policy.
  std::optional<rt::ImportMode> libraryImportModeForSelection(std::int32_t selection) noexcept;

  bool libraryImportRequiresDestructiveConfirmation(rt::ImportMode mode) noexcept;

  /// Turns the shared dry-run report into the destructive native confirmation.
  LibraryRestorePreviewState makeLibraryRestorePreviewState(i18n::MessageCatalog const& textCatalog,
                                                            rt::ImportReport const& report);
} // namespace ao::winui
