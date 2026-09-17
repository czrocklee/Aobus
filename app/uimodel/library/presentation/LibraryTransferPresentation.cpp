// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include <ao/uimodel/library/presentation/LibraryTransferPresentation.h>

#include <ao/i18n/MessageCatalog.h>
#include <ao/rt/library/LibraryTransfer.h>

#include <string>

namespace ao::uimodel
{
  LibraryRestorePresentation libraryRestorePresentation(i18n::MessageCatalog const& catalog,
                                                        rt::ImportReport const& report)
  {
    using i18n::MessageId;

    auto const isLibraryScope = report.targetScope == rt::ImportTargetScope::Library;
    auto const scope = i18n::requiredText(
      catalog, isLibraryScope ? MessageId::LibraryRestoreScopeLibrary : MessageId::LibraryRestoreScopeLists);

    return {
      .title = std::string{i18n::requiredText(catalog, MessageId::LibraryConfirmRestore)},
      .message = i18n::requiredFormat(catalog,
                                      MessageId::LibraryRestoreConfirmation,
                                      {{"scope", scope},
                                       {"version", report.payloadVersion},
                                       {"mode", rt::exportModeName(report.payloadMode)},
                                       {"tracksCreated", report.tracksCreated},
                                       {"tracksUpdated", report.tracksUpdated},
                                       {"tracksDeleted", report.tracksDeleted},
                                       {"listsCreated", report.listsCreated},
                                       {"listsDeleted", report.listsDeleted},
                                       {"dangling", report.danglingReferencesIgnored}}),
      .action = std::string{i18n::requiredText(
        catalog, isLibraryScope ? MessageId::LibraryRestoreLibrary : MessageId::LibraryRestoreLists)},
    };
  }
} // namespace ao::uimodel
