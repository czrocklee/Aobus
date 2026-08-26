// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include <ao/winui/library/LibraryTransferAdapter.h>

#include "test/unit/PresentationTextCatalogTestSupport.h"
#include <ao/rt/library/LibraryYamlExporter.h>
#include <ao/rt/library/LibraryYamlImporter.h>

#include <catch2/catch_test_macros.hpp>

namespace ao::winui::test
{
  TEST_CASE("Library transfer selector - rows map to every runtime policy", "[winui][unit][library-transfer]")
  {
    CHECK(libraryExportModeForSelection(0) == rt::ExportMode::Delta);
    CHECK(libraryExportModeForSelection(1) == rt::ExportMode::Metadata);
    CHECK(libraryExportModeForSelection(2) == rt::ExportMode::Full);
    CHECK(libraryExportModeForSelection(3) == rt::ExportMode::ListOnly);
    CHECK_FALSE(libraryExportModeForSelection(-1));
    CHECK_FALSE(libraryExportModeForSelection(4));

    CHECK(libraryImportModeForSelection(0) == rt::ImportMode::Merge);
    CHECK(libraryImportModeForSelection(1) == rt::ImportMode::Restore);
    CHECK_FALSE(libraryImportModeForSelection(-1));
    CHECK_FALSE(libraryImportModeForSelection(2));
  }

  TEST_CASE("Library import confirmation - only restore requires destructive confirmation",
            "[winui][unit][library-transfer]")
  {
    CHECK_FALSE(libraryImportRequiresDestructiveConfirmation(rt::ImportMode::Merge));
    CHECK(libraryImportRequiresDestructiveConfirmation(rt::ImportMode::Restore));
  }

  TEST_CASE("Library restore preview - preserves every shared dry-run field", "[winui][unit][library-transfer]")
  {
    auto const state = makeLibraryRestorePreviewState(ao::test::englishPresentationTextCatalog(),
                                                      rt::ImportReport{
                                                        .payloadVersion = 5,
                                                        .payloadMode = rt::ExportMode::Full,
                                                        .targetScope = rt::ImportTargetScope::Library,
                                                        .tracksCreated = 2,
                                                        .tracksUpdated = 3,
                                                        .tracksDeleted = 4,
                                                        .listsCreated = 5,
                                                        .listsDeleted = 6,
                                                        .danglingReferencesIgnored = 7,
                                                      });

    CHECK(state.title == "Confirm Restore");
    CHECK(state.primaryActionText == "Restore Library");
    CHECK(state.message ==
          "This restore will replace the current library.\n\nPayload: YAML v5, mode 'full'.\nPreview: 2 created, "
          "3 updated, 4 deleted; 5 lists created, 6 deleted; 7 dangling references ignored.\n\nContinue only if "
          "this matches the selected backup.");
  }

  TEST_CASE("Library restore preview - list-only payload names the narrower destructive scope",
            "[winui][unit][library-transfer]")
  {
    auto const state = makeLibraryRestorePreviewState(ao::test::englishPresentationTextCatalog(),
                                                      rt::ImportReport{
                                                        .payloadVersion = 5,
                                                        .payloadMode = rt::ExportMode::ListOnly,
                                                        .targetScope = rt::ImportTargetScope::Lists,
                                                      });

    CHECK(state.primaryActionText == "Restore Lists");
    CHECK(state.message.starts_with("This restore will replace the current Lists."));
    CHECK(state.message.contains("mode 'listOnly'"));
  }
} // namespace ao::winui::test
