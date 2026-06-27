// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "app/ShellLayoutStore.h"

#include "test/unit/TestUtils.h"
#include <ao/Exception.h>
#include <ao/uimodel/layout/LayoutDocument.h>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <string_view>

namespace ao::gtk::test
{
  TEST_CASE("ShellLayoutStore persists layout documents and default selection", "[gtk][unit][app][layout]")
  {
    auto const tempDir = ao::test::TempDir{};
    auto const layoutsDir = std::filesystem::path{tempDir.path()} / "layouts";

    SECTION("load on a missing file returns nullopt")
    {
      auto const store = ShellLayoutStore{layoutsDir};
      auto const optDoc = store.load("classic");

      CHECK(!optDoc.has_value());
    }

    SECTION("save creates directory and layout file, and load retrieves it")
    {
      auto store = ShellLayoutStore{layoutsDir};
      auto doc = uimodel::layout::LayoutDocument{};
      doc.version = 42;
      doc.root.type = "box";
      doc.root.id = "my-root";

      store.save(doc, "classic");

      auto const store2 = ShellLayoutStore{layoutsDir};
      auto const optLoaded = store2.load("classic");

      REQUIRE(optLoaded.has_value());
      CHECK(optLoaded->version == 42);
      CHECK(optLoaded->root.type == "box");
      CHECK(optLoaded->root.id == "my-root");
    }

    SECTION("load on a corrupted file returns nullopt without throwing")
    {
      std::filesystem::create_directories(layoutsDir);
      auto const filePath = layoutsDir / "corrupted.yaml";
      {
        auto out = std::ofstream{filePath};
        out << "layout: \"corrupted_data_not_a_map\"\n";
      }

      auto const store = ShellLayoutStore{layoutsDir};
      auto const optResult = store.load("corrupted");

      CHECK(!optResult.has_value());
    }

    SECTION("remove deletes the file, remove on a missing file is a no-op")
    {
      auto store = ShellLayoutStore{layoutsDir};
      auto doc = uimodel::layout::LayoutDocument{};
      store.save(doc, "classic");

      CHECK(std::filesystem::exists(layoutsDir / "classic.yaml"));

      store.remove("classic");
      CHECK(!std::filesystem::exists(layoutsDir / "classic.yaml"));

      // Remove again should not throw or fail
      CHECK_NOTHROW(store.remove("classic"));
    }

    SECTION("filePath throws Exception on path traversal or empty presetId")
    {
      auto const store = ShellLayoutStore{layoutsDir};

      CHECK_THROWS_AS(store.load("../traversal"), Exception);
      CHECK_THROWS_AS(store.load(""), Exception);
    }
  }
} // namespace ao::gtk::test
