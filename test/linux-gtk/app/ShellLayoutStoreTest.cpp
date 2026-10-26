// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "app/ShellLayoutStore.h"

#include "test/TestFixtureSupport.h"
#include <ao/Error.h>
#include <ao/uimodel/layout/document/LayoutDocument.h>
#include <ao/uimodel/layout/document/LayoutPreparation.h>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <ios>
#include <string>

namespace ao::gtk::test
{
  TEST_CASE("ShellLayoutStore - a session with no configuration directory uses the built-in presets")
  {
    // An empty directory would resolve preset files relative to the working
    // directory, so this answers "not customized" instead.
    auto store = ShellLayoutStore{rt::ConfigStore::NoLocation{}};

    auto const loadedRes = store.load("classic");

    REQUIRE(loadedRes);
    CHECK_FALSE(loadedRes->has_value());

    auto doc = uimodel::LayoutDocument{};
    doc.root.type = "box";

    CHECK(store.save(doc, "classic"));
    CHECK(store.remove("classic"));
    CHECK_FALSE(std::filesystem::exists("classic.yaml"));
  }

  TEST_CASE("ShellLayoutStore - loads and saves layout documents")
  {
    auto const tempDir = ao::test::TempDir{};
    auto const layoutsDir = std::filesystem::path{tempDir.path()} / "layouts";

    SECTION("load on a missing file returns nullopt")
    {
      auto const store = ShellLayoutStore{layoutsDir};
      auto const res = store.load("classic");

      REQUIRE(res);
      CHECK_FALSE(*res);
    }

    SECTION("save creates directory and layout file, and load retrieves it")
    {
      auto store = ShellLayoutStore{layoutsDir};
      auto doc = uimodel::LayoutDocument{};
      doc.root.type = "box";
      doc.root.id = "my-root";

      REQUIRE(store.save(doc, "classic"));

      auto const freshStore = ShellLayoutStore{layoutsDir};
      auto const loadedRes = freshStore.load("classic");

      REQUIRE(loadedRes);
      REQUIRE(*loadedRes);
      CHECK((*loadedRes)->version == uimodel::kLayoutDocumentVersion);
      CHECK((*loadedRes)->root.type == "box");
      CHECK((*loadedRes)->root.id == "my-root");
      CHECK((*loadedRes)->root.props.empty());
      CHECK((*loadedRes)->root.layout.empty());
      CHECK((*loadedRes)->root.children.empty());
      CHECK_FALSE((*loadedRes)->root.optTooltip);
      CHECK((*loadedRes)->templates.empty());
    }
  }

  TEST_CASE("ShellLayoutStore - rejected reads report exact errors and preserve bytes")
  {
    auto const tempDir = ao::test::TempDir{};
    auto const layoutsDir = std::filesystem::path{tempDir.path()} / "layouts";

    SECTION("load reports a corrupted file")
    {
      std::filesystem::create_directories(layoutsDir);
      auto const filePath = layoutsDir / "corrupted.yaml";
      {
        auto out = std::ofstream{filePath};
        out << "layout: \"corrupted_data_not_a_map\"\n";
      }

      auto const store = ShellLayoutStore{layoutsDir};
      auto const res = store.load("corrupted");

      REQUIRE_FALSE(res);
      CHECK(res.error().code == Error::Code::FormatRejected);
    }

    SECTION("load rejects an existing file without the layout group")
    {
      std::filesystem::create_directories(layoutsDir);
      std::ofstream{layoutsDir / "missing-group.yaml"} << "other: {}\n";

      auto const store = ShellLayoutStore{layoutsDir};
      auto const res = store.load("missing-group");

      REQUIRE_FALSE(res);
      CHECK(res.error().code == Error::Code::FormatRejected);
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
      auto const res = store.load("classic");

      REQUIRE_FALSE(res);
      CHECK(res.error().code == Error::Code::ValueTooLarge);
      CHECK(ao::test::readFile(filePath) == original);
    }
  }

  TEST_CASE("ShellLayoutStore - failed saves preserve the existing layout")
  {
    auto const tempDir = ao::test::TempDir{};
    auto const layoutsDir = std::filesystem::path{tempDir.path()} / "layouts";

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
      auto const res = store.save(replacement, "classic");

      REQUIRE_FALSE(res);
      CHECK(res.error().code == Error::Code::ValueTooLarge);
      CHECK(ao::test::readFile(filePath) == original);
    }

    SECTION("save keeps a preset a newer version saved")
    {
      std::filesystem::create_directories(layoutsDir);
      auto const filePath = layoutsDir / "classic.yaml";
      auto const original = std::string{"layout:\n  version: 2\n  root: future-layout\n"};
      std::ofstream{filePath, std::ios::binary} << original;

      auto store = ShellLayoutStore{layoutsDir};
      auto replacement = uimodel::LayoutDocument{};
      replacement.root.type = "spacer";
      auto const res = store.save(replacement, "classic");

      REQUIRE_FALSE(res);
      CHECK(res.error().code == Error::Code::NotSupported);
      CHECK(ao::test::readFile(filePath) == original);
    }
  }

  TEST_CASE("ShellLayoutStore - remove deletes a layout idempotently")
  {
    auto const tempDir = ao::test::TempDir{};
    auto const layoutsDir = std::filesystem::path{tempDir.path()} / "layouts";
    auto store = ShellLayoutStore{layoutsDir};
    auto doc = uimodel::LayoutDocument{};
    doc.root.type = "box";
    REQUIRE(store.save(doc, "classic"));

    CHECK(std::filesystem::exists(layoutsDir / "classic.yaml"));

    REQUIRE(store.remove("classic"));
    CHECK_FALSE(std::filesystem::exists(layoutsDir / "classic.yaml"));

    REQUIRE(store.remove("classic"));
  }

  TEST_CASE("ShellLayoutStore - isNewer reports only a preset a newer version saved")
  {
    auto const tempDir = ao::test::TempDir{};
    auto const layoutsDir = std::filesystem::path{tempDir.path()} / "layouts";
    std::filesystem::create_directories(layoutsDir);
    auto store = ShellLayoutStore{layoutsDir};

    SECTION("a newer preset")
    {
      std::ofstream{layoutsDir / "classic.yaml", std::ios::binary} << "layout:\n  version: 2\n  root: future-layout\n";

      CHECK_FALSE(store.load("classic"));
      CHECK(store.isNewer("classic"));
    }

    SECTION("a preset of the current version")
    {
      auto doc = uimodel::LayoutDocument{};
      doc.root.type = "box";
      REQUIRE(store.save(doc, "classic"));

      CHECK_FALSE(store.isNewer("classic"));
    }

    SECTION("a preset rejected for another reason")
    {
      std::ofstream{layoutsDir / "classic.yaml", std::ios::binary} << "layout:\n  version: 0\n  root: old-layout\n";

      CHECK_FALSE(store.load("classic"));
      CHECK_FALSE(store.isNewer("classic"));
    }

    SECTION("a preset too large to read far enough to tell")
    {
      std::ofstream{layoutsDir / "classic.yaml", std::ios::binary}
        << "layout:\n  version: 2\n  root: future-layout\n# "
        << std::string(uimodel::LayoutDocumentLimits::kDefaultMaxFileBytes, 'x') << "\n";

      CHECK_FALSE(store.load("classic"));
      CHECK_FALSE(store.isNewer("classic"));
    }

    SECTION("a missing preset")
    {
      CHECK_FALSE(store.isNewer("classic"));
    }
  }

  TEST_CASE("ShellLayoutStore - remove keeps a preset a newer version saved")
  {
    auto const tempDir = ao::test::TempDir{};
    auto const layoutsDir = std::filesystem::path{tempDir.path()} / "layouts";
    std::filesystem::create_directories(layoutsDir);
    auto store = ShellLayoutStore{layoutsDir};

    SECTION("a newer preset stays in place")
    {
      auto const filePath = layoutsDir / "classic.yaml";
      auto const original = std::string{"layout:\n  version: 2\n  root: future-layout\n"};
      std::ofstream{filePath, std::ios::binary} << original;

      auto const res = store.remove("classic");

      REQUIRE_FALSE(res);
      CHECK(res.error().code == Error::Code::NotSupported);
      CHECK(ao::test::readFile(filePath) == original);
    }

    SECTION("a newer preset above the size ceiling stays in place")
    {
      // The ceiling stops the read before the version is seen, so nothing
      // shows the preset is not newer.
      auto const filePath = layoutsDir / "classic.yaml";
      auto const original = std::string{"layout:\n  version: 2\n  root: future-layout\n# "} +
                            std::string(uimodel::LayoutDocumentLimits::kDefaultMaxFileBytes, 'x') + "\n";
      std::ofstream{filePath, std::ios::binary} << original;

      auto const res = store.remove("classic");

      REQUIRE_FALSE(res);
      CHECK(res.error().code == Error::Code::ValueTooLarge);
      CHECK(ao::test::readFile(filePath) == original);
    }

    SECTION("a preset that is malformed for another reason is still removed")
    {
      auto const filePath = layoutsDir / "broken.yaml";
      std::ofstream{filePath} << "layout: \"corrupted_data_not_a_map\"\n";

      REQUIRE(store.remove("broken"));
      CHECK_FALSE(std::filesystem::exists(filePath));
    }
  }

  TEST_CASE("ShellLayoutStore - remove keeps a preset it cannot open")
  {
    auto const tempDir = ao::test::TempDir{};
    auto const layoutsDir = std::filesystem::path{tempDir.path()} / "layouts";
    std::filesystem::create_directories(layoutsDir);
    auto store = ShellLayoutStore{layoutsDir};
    auto const filePath = layoutsDir / "classic.yaml";
    auto const original = std::string{"layout:\n  version: 2\n  root: future-layout\n"};
    std::ofstream{filePath, std::ios::binary} << original;
    std::filesystem::permissions(filePath, std::filesystem::perms::none);

    if (std::ifstream{filePath}.is_open())
    {
      SKIP("this process reads files regardless of their permission bits");
    }

    auto const res = store.remove("classic");

    REQUIRE_FALSE(res);
    CHECK(res.error().code == Error::Code::IoError);
    std::filesystem::permissions(filePath, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write);
    CHECK(ao::test::readFile(filePath) == original);
  }
} // namespace ao::gtk::test
