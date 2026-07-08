// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "app/ShellLayoutComponentStateStore.h"

#include "test/unit/TestUtils.h"
#include <ao/Exception.h>
#include <ao/uimodel/layout/component/LayoutComponentState.h>
#include <ao/uimodel/layout/document/LayoutDocument.h>
#include <ao/uimodel/layout/document/LayoutNode.h>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <string>
#include <utility>

namespace ao::gtk::test
{
  using namespace uimodel;

  namespace
  {
    uimodel::LayoutNode splitNode(std::string id = "main-paned")
    {
      auto node = uimodel::LayoutNode{};
      node.id = std::move(id);
      node.type = "split";
      node.props["orientation"] = uimodel::LayoutValue{std::string{"horizontal"}};
      node.props["initialPositionPercent"] = uimodel::LayoutValue{0.2};
      node.children = {uimodel::LayoutNode{.type = "spacer"}, uimodel::LayoutNode{.type = "spacer"}};
      return node;
    }

    uimodel::LayoutComponentStateDocument stateDocFor(uimodel::LayoutNode const& node)
    {
      auto doc = uimodel::LayoutComponentStateDocument{};
      doc.preset = "classic";
      doc.components[node.id] = uimodel::LayoutComponentStateEntry{
        .type = node.type,
        .stateVersion = uimodel::kStateEntryVersion,
        .baselineHash = uimodel::componentBaselineHash(node),
        .state = {{"positionPercent", uimodel::LayoutValue{0.35}}},
      };
      return doc;
    }
  } // namespace

  TEST_CASE("ShellLayoutComponentStateStore - persists component state by layout id", "[gtk][unit][app][layout-state]")
  {
    auto const tempDir = ao::test::TempDir{};
    auto const stateDir = std::filesystem::path{tempDir.path()} / "layout-state";

    SECTION("load on a missing file returns nullopt")
    {
      auto const store = ShellLayoutComponentStateStore{stateDir};
      CHECK_FALSE(store.load("classic").has_value());
    }

    SECTION("save creates a state file and load retrieves it")
    {
      auto store = ShellLayoutComponentStateStore{stateDir};
      auto const node = splitNode();
      auto doc = stateDocFor(node);

      store.save("classic", doc);

      auto const optLoaded = store.load("classic");
      REQUIRE(optLoaded);
      CHECK(optLoaded->preset == "classic");
      REQUIRE(optLoaded->components.contains("main-paned"));
      CHECK(optLoaded->components.at("main-paned").state.at("positionPercent").asDouble() == 0.35);
    }

    SECTION("load rejects corrupted and mismatched files without throwing")
    {
      std::filesystem::create_directories(stateDir);
      {
        auto out = std::ofstream{stateDir / "corrupted.yaml"};
        out << "not-a-state-file\n";
      }
      {
        auto out = std::ofstream{stateDir / "modern.yaml"};
        out << "version: 1\npreset: classic\ncomponents: {}\n";
      }

      auto const store = ShellLayoutComponentStateStore{stateDir};
      CHECK_FALSE(store.load("corrupted").has_value());
      CHECK_FALSE(store.load("modern").has_value());
    }

    SECTION("prune removes orphan, type-mismatched, and stale-baseline entries")
    {
      auto store = ShellLayoutComponentStateStore{stateDir};
      auto liveNode = splitNode("live-split");
      auto staleNode = splitNode("stale-split");
      auto doc = stateDocFor(liveNode);
      doc.components["orphan-split"] = uimodel::LayoutComponentStateEntry{
        .type = "split",
        .stateVersion = uimodel::kStateEntryVersion,
        .baselineHash = "orphan",
        .state = {{"positionPercent", uimodel::LayoutValue{0.10}}},
      };
      doc.components["wrong-type"] = uimodel::LayoutComponentStateEntry{
        .type = "collapsibleSplit",
        .stateVersion = uimodel::kStateEntryVersion,
        .baselineHash = uimodel::componentBaselineHash(liveNode),
        .state = {{"positionPercent", uimodel::LayoutValue{0.20}}},
      };
      doc.components["stale-split"] = uimodel::LayoutComponentStateEntry{
        .type = "split",
        .stateVersion = uimodel::kStateEntryVersion,
        .baselineHash = "stale",
        .state = {{"positionPercent", uimodel::LayoutValue{0.30}}},
      };
      store.save("classic", doc);

      auto layoutDoc = uimodel::LayoutDocument{};
      layoutDoc.root.type = "box";
      layoutDoc.root.children = {liveNode, uimodel::LayoutNode{.id = "wrong-type", .type = "split"}, staleNode};

      store.prune("classic", layoutDoc);

      auto const optLoaded = store.load("classic");
      REQUIRE(optLoaded);
      REQUIRE(optLoaded->components.size() == 1);
      CHECK(optLoaded->components.contains("live-split"));
    }

    SECTION("removePreset deletes the file and reports success")
    {
      auto store = ShellLayoutComponentStateStore{stateDir};
      store.save("classic", stateDocFor(splitNode()));

      CHECK(std::filesystem::exists(stateDir / "classic.yaml"));

      CHECK(store.removePreset("classic"));
      CHECK_FALSE(std::filesystem::exists(stateDir / "classic.yaml"));
      CHECK(store.removePreset("classic"));
    }

    SECTION("prune returns whether anything changed")
    {
      auto store = ShellLayoutComponentStateStore{stateDir};
      auto const node = splitNode("live-split");
      auto doc = stateDocFor(node);
      store.save("classic", doc);

      auto layoutDoc = uimodel::LayoutDocument{};
      layoutDoc.root.type = "box";
      layoutDoc.root.children = {node};

      CHECK_FALSE(store.prune("classic", layoutDoc));

      layoutDoc.root.children.clear();
      CHECK(store.prune("classic", layoutDoc));
    }

    SECTION("preset validation rejects path traversal, empty ids, and null bytes")
    {
      auto const store = ShellLayoutComponentStateStore{stateDir};

      CHECK_THROWS_AS(store.load("../traversal"), Exception);
      CHECK_THROWS_AS(store.load(""), Exception);
      CHECK_THROWS_AS(store.load(std::string{"class\0ic", 9}), Exception);
    }

    SECTION("saved state file is readable only by owner")
    {
      auto store = ShellLayoutComponentStateStore{stateDir};
      store.save("classic", stateDocFor(splitNode()));

      auto const perms = std::filesystem::status(stateDir / "classic.yaml").permissions();
      CHECK((perms & std::filesystem::perms::owner_read) != std::filesystem::perms::none);
      CHECK((perms & std::filesystem::perms::owner_write) != std::filesystem::perms::none);
      CHECK((perms & std::filesystem::perms::group_read) == std::filesystem::perms::none);
      CHECK((perms & std::filesystem::perms::others_read) == std::filesystem::perms::none);
    }
  }
} // namespace ao::gtk::test
