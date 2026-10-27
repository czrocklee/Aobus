// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include <ao/winui/library/LibraryTransferAdapter.h>

#include <ao/rt/library/LibraryTransfer.h>

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
    CHECK_FALSE(needsLibraryImportDestructiveConfirmation(rt::ImportMode::Merge));
    CHECK(needsLibraryImportDestructiveConfirmation(rt::ImportMode::Restore));
  }
} // namespace ao::winui::test
