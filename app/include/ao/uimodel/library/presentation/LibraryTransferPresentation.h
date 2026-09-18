// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <ao/i18n/MessageCatalog.h>
#include <ao/rt/library/LibraryTransfer.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <string>

namespace ao::uimodel
{
  struct LibraryExportOptionDescriptor final
  {
    rt::ExportMode mode;
    i18n::MessageId messageId;

    bool operator==(LibraryExportOptionDescriptor const&) const = default;
  };

  inline constexpr auto kLibraryExportOptions = std::to_array<LibraryExportOptionDescriptor>({
    {.mode = rt::ExportMode::Delta, .messageId = i18n::MessageId::LibraryExportModeDelta},
    {.mode = rt::ExportMode::Metadata, .messageId = i18n::MessageId::LibraryExportModeMetadata},
    {.mode = rt::ExportMode::Full, .messageId = i18n::MessageId::LibraryExportModeFull},
    {.mode = rt::ExportMode::ListOnly, .messageId = i18n::MessageId::LibraryExportModeListOnly},
  });

  inline constexpr auto kLibraryExportDefaultIndex = static_cast<std::size_t>(
    std::ranges::find(kLibraryExportOptions, rt::ExportMode::Full, &LibraryExportOptionDescriptor::mode) -
    kLibraryExportOptions.begin());
  static_assert(kLibraryExportDefaultIndex < kLibraryExportOptions.size());

  struct LibraryRestorePresentation final
  {
    std::string title;
    std::string message;
    std::string action;

    bool operator==(LibraryRestorePresentation const&) const = default;
  };

  LibraryRestorePresentation libraryRestorePresentation(i18n::MessageCatalog const& catalog,
                                                        rt::ImportReport const& report);
} // namespace ao::uimodel
