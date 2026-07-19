// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "app/ShellLayoutStore.h"

#include "test/unit/TestUtils.h"
#include <ao/Error.h>
#include <ao/Exception.h>
#include <ao/uimodel/layout/document/LayoutDocument.h>
#include <ao/uimodel/layout/document/LayoutPreparation.h>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <ios>
#include <string>
#include <string_view>

namespace ao::gtk::test
{
  TEST_CASE("ShellLayoutStore - persists layout documents and default selection", "[gtk][unit][app][layout]")
  {
    auto const tempDir = ao::test::TempDir{};
    auto const layoutsDir = std::filesystem::path{tempDir.path()} / "layouts";

    SECTION("load on a missing file returns nullopt")
    {
      auto const store = ShellLayoutStore{layoutsDir};
      auto const result = store.load("classic");

      REQUIRE(result);
      CHECK_FALSE(*result);
    }

    SECTION("save creates directory and layout file, and load retrieves it")
    {
      auto store = ShellLayoutStore{layoutsDir};
      auto doc = uimodel::LayoutDocument{};
      doc.root.type = "box";
      doc.root.id = "my-root";

      REQUIRE(store.save(doc, "classic"));

      auto const store2 = ShellLayoutStore{layoutsDir};
      auto const loaded = store2.load("classic");

      REQUIRE(loaded);
      REQUIRE(*loaded);
      CHECK((*loaded)->version == uimodel::kLayoutDocumentVersion);
      CHECK((*loaded)->root.type == "box");
      CHECK((*loaded)->root.id == "my-root");
    }

    SECTION("load reports a corrupted file")
    {
      std::filesystem::create_directories(layoutsDir);
      auto const filePath = layoutsDir / "corrupted.yaml";
      {
        auto out = std::ofstream{filePath};
        out << "layout: \"corrupted_data_not_a_map\"\n";
      }

      auto const store = ShellLayoutStore{layoutsDir};
      auto const result = store.load("corrupted");

      REQUIRE_FALSE(result);
      CHECK(result.error().code == Error::Code::FormatRejected);
    }

    SECTION("load rejects an existing file without the layout group")
    {
      std::filesystem::create_directories(layoutsDir);
      std::ofstream{layoutsDir / "missing-group.yaml"} << "other: {}\n";

      auto const store = ShellLayoutStore{layoutsDir};
      auto const result = store.load("missing-group");

      REQUIRE_FALSE(result);
      CHECK(result.error().code == Error::Code::FormatRejected);
    }

    SECTION("load rejects an oversized file before parsing and preserves it")
    {
      std::filesystem::create_directories(layoutsDir);
      auto const filePath = layoutsDir / "classic.yaml";
      auto const original = std::string(128, 'x');
      std::ofstream{filePath, std::ios::binary} << original;

      auto limits = uimodel::LayoutDocumentLimits{};
      limits.maxFileBytes = 64;
      auto const store = ShellLayoutStore{layoutsDir, limits};
      auto const result = store.load("classic");

      REQUIRE_FALSE(result);
      CHECK(result.error().code == Error::Code::ValueTooLarge);
      CHECK(ao::test::readFile(filePath) == original);
    }

    SECTION("save rejects an over-budget candidate and preserves the existing layout")
    {
      auto initialStore = ShellLayoutStore{layoutsDir};
      auto initial = uimodel::LayoutDocument{};
      initial.root.type = "spacer";
      REQUIRE(initialStore.save(initial, "classic"));

      auto const filePath = layoutsDir / "classic.yaml";
      auto const original = ao::test::readFile(filePath);
      auto limits = uimodel::LayoutDocumentLimits{};
      limits.authored.maxEntries = 1;
      auto store = ShellLayoutStore{layoutsDir, limits};
      auto replacement = initial;
      replacement.root.children.push_back(uimodel::LayoutNode{.type = "spacer"});
      auto const result = store.save(replacement, "classic");

      REQUIRE_FALSE(result);
      CHECK(result.error().code == Error::Code::ValueTooLarge);
      CHECK(ao::test::readFile(filePath) == original);
    }

    SECTION("save preserves an existing document from an unsupported version")
    {
      std::filesystem::create_directories(layoutsDir);
      auto const filePath = layoutsDir / "classic.yaml";
      auto const original = std::string{"layout:\n  version: 99\n  root: future-layout\n"};
      std::ofstream{filePath, std::ios::binary} << original;

      auto store = ShellLayoutStore{layoutsDir};
      auto replacement = uimodel::LayoutDocument{};
      replacement.root.type = "spacer";
      auto const result = store.save(replacement, "classic");

      REQUIRE_FALSE(result);
      CHECK(result.error().code == Error::Code::NotSupported);
      CHECK(ao::test::readFile(filePath) == original);
    }

    SECTION("remove deletes the file, remove on a missing file is a no-op")
    {
      auto store = ShellLayoutStore{layoutsDir};
      auto doc = uimodel::LayoutDocument{};
      doc.root.type = "box";
      REQUIRE(store.save(doc, "classic"));

      CHECK(std::filesystem::exists(layoutsDir / "classic.yaml"));

      REQUIRE(store.remove("classic"));
      CHECK(!std::filesystem::exists(layoutsDir / "classic.yaml"));

      REQUIRE(store.remove("classic"));
    }

    SECTION("filePath throws Exception on path traversal or empty presetId")
    {
      auto const store = ShellLayoutStore{layoutsDir};

      CHECK_THROWS_AS(store.load("../traversal"), Exception);
      CHECK_THROWS_AS(store.load(""), Exception);
    }
  }
} // namespace ao::gtk::test
