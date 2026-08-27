// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include <ao/winui/library/LibraryTransferAdapter.h>

#include <ao/i18n/MessageCatalog.h>
#include <ao/rt/library/LibraryYamlExporter.h>
#include <ao/rt/library/LibraryYamlImporter.h>

#include <cstdint>
#include <optional>

namespace ao::winui
{
  std::optional<rt::ExportMode> libraryExportModeForSelection(std::int32_t const selection) noexcept
  {
    switch (selection)
    {
      case 0: return rt::ExportMode::Delta;
      case 1: return rt::ExportMode::Metadata;
      case 2: return rt::ExportMode::Full;
      case 3: return rt::ExportMode::ListOnly;
      default: return std::nullopt;
    }
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

  bool libraryImportRequiresDestructiveConfirmation(rt::ImportMode const mode) noexcept
  {
    return mode == rt::ImportMode::Restore;
  }

  LibraryRestorePreviewState makeLibraryRestorePreviewState(i18n::MessageCatalog const& textCatalog,
                                                            rt::ImportReport const& report)
  {
    using i18n::MessageId;
    auto const libraryScope = report.targetScope == rt::ImportTargetScope::Library;
    auto const scope = i18n::requiredText(
      textCatalog,
      libraryScope ? MessageId::WinUiLibraryRestoreScopeLibrary : MessageId::WinUiLibraryRestoreScopeLists);

    return {
      .title = std::string{i18n::requiredText(textCatalog, MessageId::WinUiLibraryConfirmRestore)},
      .message = i18n::requiredFormat(textCatalog,
                                      MessageId::WinUiLibraryRestoreConfirmation,
                                      {{"scope", scope},
                                       {"version", report.payloadVersion},
                                       {"mode", rt::exportModeName(report.payloadMode)},
                                       {"tracksCreated", report.tracksCreated},
                                       {"tracksUpdated", report.tracksUpdated},
                                       {"tracksDeleted", report.tracksDeleted},
                                       {"listsCreated", report.listsCreated},
                                       {"listsDeleted", report.listsDeleted},
                                       {"dangling", report.danglingReferencesIgnored}}),
      .primaryActionText = std::string{i18n::requiredText(
        textCatalog, libraryScope ? MessageId::WinUiLibraryRestoreLibrary : MessageId::WinUiLibraryRestoreLists)},
    };
  }
} // namespace ao::winui
