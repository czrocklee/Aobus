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

    CHECK(presentation.title == "Confirm Restore");
    CHECK(presentation.action == "Restore Library");
    CHECK(presentation.message ==
          "This restore will replace the current library tracks and Lists.\n\nPayload: YAML v5, mode 'full'.\nPreview: "
          "2 created, 3 updated, 4 deleted; 5 Lists created, 6 deleted; 7 dangling references ignored.\n\nContinue "
          "only if this matches the selected backup.");
  }

  TEST_CASE("Library restore presentation - every mode preserves its token and explicit zero counts",
            "[uimodel][unit][library-transfer]")
  {
    auto const modes = std::array{
      std::pair{rt::ExportMode::Delta, std::string_view{"delta"}},
      std::pair{rt::ExportMode::Metadata, std::string_view{"metadata"}},
      std::pair{rt::ExportMode::Full, std::string_view{"full"}},
      std::pair{rt::ExportMode::ListOnly, std::string_view{"listOnly"}},
    };

    for (auto const& [mode, token] : modes)
    {
      auto const presentation = libraryRestorePresentation(ao::test::englishMessageCatalog(),
                                                           rt::ImportReport{
                                                             .payloadVersion = 5,
                                                             .payloadMode = mode,
                                                             .targetScope = rt::ImportTargetScope::Lists,
                                                           });

      CHECK(presentation.title == "Confirm Restore");
      CHECK(presentation.action == "Restore Lists");
      CHECK(presentation.message.starts_with("This restore will replace the current Lists."));
      CHECK(presentation.message.contains(std::string{"mode '"} + std::string{token} + "'"));
      CHECK(presentation.message.contains(
        "Preview: 0 created, 0 updated, 0 deleted; 0 Lists created, 0 deleted; 0 dangling references ignored."));
    }
  }
} // namespace ao::uimodel::test
