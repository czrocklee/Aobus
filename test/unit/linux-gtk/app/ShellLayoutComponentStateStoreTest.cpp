// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "app/ShellLayoutComponentStateStore.h"

#include "test/unit/FilesystemTestSupport.h"
#include "test/unit/TestFixtureSupport.h"
#include <ao/uimodel/layout/component/LayoutComponentState.h>
#include <ao/uimodel/layout/component/LayoutSchema.h>
#include <ao/uimodel/layout/document/LayoutDocument.h>
#include <ao/uimodel/layout/document/LayoutNode.h>
#include <ao/uimodel/layout/document/LayoutPreparation.h>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <utility>

namespace ao::gtk::test
{
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

    uimodel::LayoutComponentStateEntry stateEntryFor(uimodel::LayoutNode const& node, double const positionPercent)
    {
      return uimodel::LayoutComponentStateEntry{
        .type = node.type,
        .stateVersion = uimodel::kStateEntryVersion,
        .baselineHash = uimodel::componentBaselineHash(node),
        .state = {{"positionPercent", uimodel::LayoutValue{positionPercent}}},
      };
    }

    uimodel::LayoutComponentStateDocument stateDocFor(uimodel::LayoutNode const& node)
    {
      auto doc = uimodel::LayoutComponentStateDocument{};
      doc.preset = "classic";
      doc.components[node.id] = stateEntryFor(node, 0.35);
      return doc;
    }

    uimodel::LayoutSchema persistentStateSchema()
    {
      auto schema = uimodel::LayoutSchema{};
      REQUIRE(schema.tryAddSharedComponent("split"));
      return schema;
    }

    void checkStateEntry(uimodel::LayoutComponentStateEntry const& entry,
                         std::string_view const type,
                         std::string const& baselineHash,
                         double const positionPercent)
    {
      CHECK(entry.type == type);
      CHECK(entry.stateVersion == uimodel::kStateEntryVersion);
      CHECK(entry.baselineHash == baselineHash);
      REQUIRE(entry.state.size() == 1);
      REQUIRE(entry.state.contains("positionPercent"));
      CHECK(entry.state.at("positionPercent").asDouble() == positionPercent);
    }

    void checkSingleStateDocument(uimodel::LayoutComponentStateDocument const& doc,
                                  uimodel::LayoutNode const& node,
                                  double const positionPercent)
    {
      CHECK(doc.version == uimodel::kStateFileVersion);
      CHECK(doc.preset == "classic");
      REQUIRE(doc.components.size() == 1);
      REQUIRE(doc.components.contains(node.id));
      checkStateEntry(doc.components.at(node.id), node.type, uimodel::componentBaselineHash(node), positionPercent);
    }
  } // namespace

  TEST_CASE("ShellLayoutComponentStateStore - persists and rejects state documents", "[gtk][unit][app][layout-state]")
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
      auto const node = splitNode();
      {
        auto store = ShellLayoutComponentStateStore{stateDir};
        store.save("classic", stateDocFor(node));
      }

      auto const freshStore = ShellLayoutComponentStateStore{stateDir};
      auto const optLoaded = freshStore.load("classic");
      REQUIRE(optLoaded);
      checkSingleStateDocument(*optLoaded, node, 0.35);
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
  }

  TEST_CASE("ShellLayoutComponentStateStore - prune removes only entries outside the prepared baseline",
            "[gtk][unit][app][layout-state]")
  {
    auto const tempDir = ao::test::TempDir{};
    auto const stateDir = std::filesystem::path{tempDir.path()} / "layout-state";

    SECTION("prune removes orphan, type-mismatched, and stale-baseline entries")
    {
      auto store = ShellLayoutComponentStateStore{stateDir};
      auto const liveNode = splitNode("live-split");
      auto const wrongTypeNode = splitNode("wrong-type");
      auto const staleNode = splitNode("stale-split");
      auto doc = stateDocFor(liveNode);
      doc.components["orphan-split"] = uimodel::LayoutComponentStateEntry{
        .type = "split",
        .stateVersion = uimodel::kStateEntryVersion,
        .baselineHash = "orphan",
        .state = {{"positionPercent", uimodel::LayoutValue{0.10}}},
      };
      auto wrongTypeEntry = stateEntryFor(wrongTypeNode, 0.20);
      wrongTypeEntry.type = "collapsibleSplit";
      doc.components[wrongTypeNode.id] = std::move(wrongTypeEntry);
      doc.components[staleNode.id] = uimodel::LayoutComponentStateEntry{
        .type = "split",
        .stateVersion = uimodel::kStateEntryVersion,
        .baselineHash = "stale",
        .state = {{"positionPercent", uimodel::LayoutValue{0.30}}},
      };
      store.save("classic", doc);

      auto layoutDoc = uimodel::LayoutDocument{};
      layoutDoc.root.type = "box";
      layoutDoc.root.children = {liveNode, wrongTypeNode, staleNode};
      auto const preparedRes = uimodel::prepareLayout(layoutDoc);
      REQUIRE(preparedRes);
      auto const schema = persistentStateSchema();

      CHECK(store.tryPrune("classic", *preparedRes, schema));

      auto const freshStore = ShellLayoutComponentStateStore{stateDir};
      auto const optLoaded = freshStore.load("classic");
      REQUIRE(optLoaded);
      checkSingleStateDocument(*optLoaded, liveNode, 0.35);
    }

    SECTION("prune returns whether anything changed")
    {
      auto store = ShellLayoutComponentStateStore{stateDir};
      auto const node = splitNode("live-split");
      store.save("classic", stateDocFor(node));

      auto layoutDoc = uimodel::LayoutDocument{};
      layoutDoc.root.type = "box";
      layoutDoc.root.children = {node};
      auto preparedRes = uimodel::prepareLayout(layoutDoc);
      REQUIRE(preparedRes);
      auto const schema = persistentStateSchema();

      CHECK_FALSE(store.tryPrune("classic", *preparedRes, schema));
      {
        auto const freshStore = ShellLayoutComponentStateStore{stateDir};
        auto const optLoaded = freshStore.load("classic");
        REQUIRE(optLoaded);
        checkSingleStateDocument(*optLoaded, node, 0.35);
      }

      layoutDoc.root.children.clear();
      preparedRes = uimodel::prepareLayout(layoutDoc);
      REQUIRE(preparedRes);
      CHECK(store.tryPrune("classic", *preparedRes, schema));
      CHECK_FALSE(std::filesystem::exists(stateDir / "classic.yaml"));

      auto const freshStore = ShellLayoutComponentStateStore{stateDir};
      CHECK_FALSE(freshStore.load("classic").has_value());
    }
  }

  TEST_CASE("ShellLayoutComponentStateStore - removePreset deletes state idempotently",
            "[gtk][unit][app][layout-state]")
  {
    auto const tempDir = ao::test::TempDir{};
    auto const stateDir = std::filesystem::path{tempDir.path()} / "layout-state";
    auto store = ShellLayoutComponentStateStore{stateDir};
    store.save("classic", stateDocFor(splitNode()));

    CHECK(std::filesystem::exists(stateDir / "classic.yaml"));

    CHECK(store.tryRemovePreset("classic"));
    CHECK_FALSE(std::filesystem::exists(stateDir / "classic.yaml"));
    CHECK(store.tryRemovePreset("classic"));
  }

  TEST_CASE("ShellLayoutComponentStateStore - saved state file is readable only by owner",
            "[gtk][unit][app][layout-state]")
  {
    auto const tempDir = ao::test::TempDir{};
    auto const stateDir = std::filesystem::path{tempDir.path()} / "layout-state";
    auto store = ShellLayoutComponentStateStore{stateDir};
    store.save("classic", stateDocFor(splitNode()));

    CHECK(ao::test::hasPrivateManagedFileAccess(stateDir / "classic.yaml"));
  }
} // namespace ao::gtk::test
