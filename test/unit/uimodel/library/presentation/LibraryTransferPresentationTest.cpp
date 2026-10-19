// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include <ao/uimodel/library/presentation/LibraryTransferPresentation.h>

#include "test/unit/MessageCatalogTestSupport.h"
#include <ao/i18n/MessageCatalog.h>
#include <ao/rt/library/LibraryTransfer.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <string>
#include <string_view>
#include <utility>

namespace ao::uimodel::test
{
  TEST_CASE("Library export options - fixed order covers every native choice", "[uimodel][unit][library-transfer]")
  {
    REQUIRE(kLibraryExportOptions.size() == 4);
    CHECK(kLibraryExportOptions[0] ==
          LibraryExportOptionDescriptor{
            .mode = rt::ExportMode::Delta, .messageId = i18n::MessageId::LibraryExportModeDelta});
    CHECK(kLibraryExportOptions[1] ==
          LibraryExportOptionDescriptor{
            .mode = rt::ExportMode::Metadata, .messageId = i18n::MessageId::LibraryExportModeMetadata});
    CHECK(
      kLibraryExportOptions[2] ==
      LibraryExportOptionDescriptor{.mode = rt::ExportMode::Full, .messageId = i18n::MessageId::LibraryExportModeFull});
    CHECK(kLibraryExportOptions[3] ==
          LibraryExportOptionDescriptor{
            .mode = rt::ExportMode::ListOnly, .messageId = i18n::MessageId::LibraryExportModeListOnly});
  }

  TEST_CASE("Library export options - default selects Full", "[uimodel][unit][library-transfer]")
  {
    REQUIRE(kLibraryExportDefaultIndex < kLibraryExportOptions.size());
    CHECK(kLibraryExportOptions[kLibraryExportDefaultIndex].mode == rt::ExportMode::Full);
  }

  TEST_CASE("Library restore presentation - owns every library-scope report field", "[uimodel][unit][library-transfer]")
  {
    auto const presentation = libraryRestorePresentation(ao::test::englishMessageCatalog(),
                                                         rt::ImportReport{
                                                           .payloadVersion = 1,
                                                           .payloadMode = rt::ExportMode::Full,
                                                           .targetScope = rt::ImportTargetScope::Library,
                                                           .tracksCreated = 2,
                                                           .tracksUpdated = 3,
                                                           .tracksDeleted = 4,
                                                           .listsCreated = 5,
                                                           .listsDeleted = 6,
                                                           .danglingReferencesIgnored = 7,
                                                           .invalidFiltersKept = 8,
                                                         });

    CHECK(presentation.title == "Confirm Restore");
    CHECK(presentation.action == "Restore Library");
    CHECK(
      presentation.message ==
      "This restore will replace the current library tracks and Lists.\n\nPayload: YAML v1, mode 'full'.\nPreview — "
      "tracks: 2 created, 3 updated, 4 deleted; Lists: 5 created, 6 deleted; dangling references ignored: 7; "
      "invalid List filters kept as written: 8.\n\nContinue only if this matches the selected backup.");
  }

  TEST_CASE("Library restore presentation - list-only payload names the narrower destructive scope",
            "[uimodel][unit][library-transfer]")
  {
    // Deliberately distinct report fields exercise projection, not the importer's valid list-only counts.
    auto const presentation = libraryRestorePresentation(ao::test::englishMessageCatalog(),
                                                         rt::ImportReport{
                                                           .payloadVersion = 1,
                                                           .payloadMode = rt::ExportMode::ListOnly,
                                                           .targetScope = rt::ImportTargetScope::Lists,
                                                           .tracksCreated = 2,
                                                           .tracksUpdated = 3,
                                                           .tracksDeleted = 4,
                                                           .listsCreated = 5,
                                                           .listsDeleted = 6,
                                                           .danglingReferencesIgnored = 7,
                                                           .invalidFiltersKept = 8,
                                                         });

    CHECK(presentation.title == "Confirm Restore");
    CHECK(presentation.action == "Restore Lists");
    CHECK(presentation.message ==
          "This restore will replace the current Lists.\n\nPayload: YAML v1, mode 'list-only'.\nPreview — "
          "tracks: 2 created, 3 updated, 4 deleted; Lists: 5 created, 6 deleted; dangling references ignored: 7; "
          "invalid List filters kept as written: 8.\n\nContinue only if this matches the selected backup.");
  }

  TEST_CASE("Library restore presentation - every mode preserves its token and explicit zero counts",
            "[uimodel][unit][library-transfer]")
  {
    auto const modes = std::array{
      std::pair{rt::ExportMode::Delta, std::string_view{"delta"}},
      std::pair{rt::ExportMode::Metadata, std::string_view{"metadata"}},
      std::pair{rt::ExportMode::Full, std::string_view{"full"}},
      std::pair{rt::ExportMode::ListOnly, std::string_view{"list-only"}},
    };

    for (auto const& [mode, token] : modes)
    {
      auto const presentation = libraryRestorePresentation(ao::test::englishMessageCatalog(),
                                                           rt::ImportReport{
                                                             .payloadVersion = 1,
                                                             .payloadMode = mode,
                                                             .targetScope = rt::ImportTargetScope::Lists,
                                                           });

      CHECK(presentation.title == "Confirm Restore");
      CHECK(presentation.action == "Restore Lists");
      CHECK(presentation.message ==
            std::string{"This restore will replace the current Lists.\n\nPayload: YAML v1, mode '"} +
              std::string{token} +
              "'.\nPreview — tracks: 0 created, 0 updated, 0 deleted; Lists: 0 created, 0 deleted; dangling "
              "references ignored: 0; invalid List filters kept as written: 0.\n\nContinue only if this matches the "
              "selected backup.");
    }
  }

  TEST_CASE("Library restore presentation - a count of one reads as grammatically as any other",
            "[uimodel][unit][library-transfer]")
  {
    auto const presentation = libraryRestorePresentation(ao::test::englishMessageCatalog(),
                                                         rt::ImportReport{
                                                           .payloadVersion = 1,
                                                           .payloadMode = rt::ExportMode::Full,
                                                           .targetScope = rt::ImportTargetScope::Library,
                                                           .tracksCreated = 1,
                                                           .tracksUpdated = 1,
                                                           .tracksDeleted = 1,
                                                           .listsCreated = 1,
                                                           .listsDeleted = 1,
                                                           .danglingReferencesIgnored = 1,
                                                           .invalidFiltersKept = 1,
                                                         });

    CHECK(
      presentation.message ==
      "This restore will replace the current library tracks and Lists.\n\nPayload: YAML v1, mode 'full'.\nPreview — "
      "tracks: 1 created, 1 updated, 1 deleted; Lists: 1 created, 1 deleted; dangling references ignored: 1; "
      "invalid List filters kept as written: 1.\n\nContinue only if this matches the selected backup.");
  }
} // namespace ao::uimodel::test
