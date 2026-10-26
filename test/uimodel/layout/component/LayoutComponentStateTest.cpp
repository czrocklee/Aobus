// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors
#include <ao/uimodel/layout/component/LayoutComponentState.h>

#include <ao/uimodel/layout/component/LayoutComponentStateYaml.h>
#include <ao/uimodel/layout/component/LayoutSchema.h>
#include <ao/uimodel/layout/document/LayoutDocument.h>
#include <ao/uimodel/layout/document/LayoutNode.h>
#include <ao/uimodel/layout/document/LayoutPreparation.h>
#include <ao/utility/Xxh3.h>
#include <ao/yaml/RymlAdapter.h>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace ao::uimodel::test
{
  namespace yaml = ao::yaml;

  namespace
  {
    LayoutNode splitNode(std::string id = "main-paned")
    {
      auto node = LayoutNode{};
      node.id = std::move(id);
      node.type = "split";
      node.props["orientation"] = LayoutValue{std::string{"horizontal"}};
      node.props["initialPositionPercent"] = LayoutValue{0.2};
      node.children = {LayoutNode{.type = "spacer"}, LayoutNode{.type = "spacer"}};
      return node;
    }

    LayoutNode collapsibleSplitNode(std::string id = "detail-split")
    {
      auto node = LayoutNode{};
      node.id = std::move(id);
      node.type = "collapsibleSplit";
      node.props["orientation"] = LayoutValue{std::string{"horizontal"}};
      node.props["collapseSide"] = LayoutValue{std::string{"end"}};
      node.props["initialPositionPercent"] = LayoutValue{0.25};
      node.props["position"] = LayoutValue{static_cast<std::int64_t>(150)};
      node.props["revealed"] = LayoutValue{true};
      node.children = {LayoutNode{.type = "spacer"}, LayoutNode{.type = "spacer"}};
      return node;
    }

    LayoutComponentStateDocument stateDocFor(LayoutNode const& node)
    {
      auto doc = LayoutComponentStateDocument{};
      doc.preset = "modern";
      doc.components[node.id] = LayoutComponentStateEntry{
        .type = node.type,
        .stateVersion = kStateEntryVersion,
        .baselineHash = componentBaselineHash(node),
        .state = {{"positionPercent", LayoutValue{0.42}}},
      };
      return doc;
    }

    LayoutSchema persistentLayoutSchema()
    {
      auto schema = LayoutSchema{};
      REQUIRE(schema.tryAddSharedComponent("split"));
      return schema;
    }
  } // namespace

  TEST_CASE("LayoutComponentState - file round-trips entries", "[uimodel][unit][layout][component]")
  {
    auto const node = splitNode();
    auto original = stateDocFor(node);

    auto tree = ryml::Tree{};
    REQUIRE(LayoutComponentStateYamlSchema{}.serialize(tree.rootref(), original));

    auto decodedRes = LayoutComponentStateYamlSchema{}.deserialize(tree.rootref(), LayoutComponentStateDocument{});
    REQUIRE(decodedRes);

    CHECK(decodedRes->version == kStateFileVersion);
    CHECK(decodedRes->preset == "modern");
    REQUIRE(decodedRes->components.contains("main-paned"));
    auto const& decodedEntry = decodedRes->components.at("main-paned");
    CHECK(decodedEntry.type == "split");
    CHECK(decodedEntry.stateVersion == kStateEntryVersion);
    CHECK(decodedEntry.baselineHash == componentBaselineHash(node));
    CHECK(decodedEntry.state.at("positionPercent").asDouble() == 0.42);
  }

  TEST_CASE("LayoutComponentState - resolver validates versions type and baseline",
            "[uimodel][unit][layout][component]")
  {
    auto node = splitNode();
    auto stateDoc = stateDocFor(node);

    SECTION("matching state resolves")
    {
      auto const optResolved = resolveComponentState(stateDoc, node);
      REQUIRE(optResolved);
      CHECK(optResolved->state.at("positionPercent").asDouble() == 0.42);
    }

    SECTION("unknown file version is ignored")
    {
      stateDoc.version = 99;
      CHECK_FALSE(resolveComponentState(stateDoc, node).has_value());
    }

    SECTION("unknown component state version is ignored")
    {
      stateDoc.components.at(node.id).stateVersion = 99;
      CHECK_FALSE(resolveComponentState(stateDoc, node).has_value());
    }

    SECTION("type mismatch is ignored")
    {
      stateDoc.components.at(node.id).type = "collapsibleSplit";
      CHECK_FALSE(resolveComponentState(stateDoc, node).has_value());
    }

    SECTION("baseline mismatch is ignored")
    {
      node.props["resizeStart"] = LayoutValue{false};
      CHECK_FALSE(resolveComponentState(stateDoc, node).has_value());
    }

    SECTION("anonymous nodes are never resolved")
    {
      node.id.clear();
      CHECK_FALSE(resolveComponentState(stateDoc, node).has_value());
    }
  }

  TEST_CASE("LayoutComponentState - layout component baseline hash tracks the owned field inventory",
            "[uimodel][unit][layout][component]")
  {
    SECTION("equivalent numeric spellings hash the same")
    {
      auto first = splitNode();
      first.props["initialPositionPercent"] = LayoutValue{0.2};

      auto second = splitNode();
      second.props["initialPositionPercent"] = LayoutValue{std::string{"0.20"}};

      CHECK(componentBaselineHash(first) == componentBaselineHash(second));
    }

    SECTION("explicit defaults hash like omitted defaults")
    {
      auto implicitSplit = LayoutNode{.type = "split"};
      auto explicitSplit = implicitSplit;
      explicitSplit.props = {{"orientation", LayoutValue{std::string{"vertical"}}},
                             {"initialPositionPercent", LayoutValue{0.0}},
                             {"position", LayoutValue{static_cast<std::int64_t>(-1)}},
                             {"resizeStart", LayoutValue{true}},
                             {"resizeEnd", LayoutValue{true}},
                             {"shrinkStart", LayoutValue{false}},
                             {"shrinkEnd", LayoutValue{false}}};
      CHECK(componentBaselineHash(implicitSplit) == componentBaselineHash(explicitSplit));

      auto implicitCollapsible = LayoutNode{.type = "collapsibleSplit"};
      auto explicitCollapsible = implicitCollapsible;
      explicitCollapsible.props = {{"orientation", LayoutValue{std::string{"horizontal"}}},
                                   {"collapseSide", LayoutValue{std::string{"end"}}},
                                   {"initialPositionPercent", LayoutValue{0.0}},
                                   {"position", LayoutValue{static_cast<std::int64_t>(-1)}},
                                   {"revealed", LayoutValue{true}}};
      CHECK(componentBaselineHash(implicitCollapsible) == componentBaselineHash(explicitCollapsible));
    }

    SECTION("irrelevant children do not change parent hash")
    {
      auto first = splitNode();
      auto second = first;
      second.children.push_back(LayoutNode{.id = "extra-child", .type = "separator"});

      CHECK(componentBaselineHash(first) == componentBaselineHash(second));
    }

    SECTION("every split baseline field changes the hash")
    {
      auto const original = splitNode();
      auto const originalHash = componentBaselineHash(original);
      auto candidates = std::vector<std::pair<std::string, LayoutValue>>{
        {"orientation", LayoutValue{std::string{"vertical"}}},
        {"initialPositionPercent", LayoutValue{0.4}},
        {"position", LayoutValue{static_cast<std::int64_t>(240)}},
        {"resizeStart", LayoutValue{false}},
        {"resizeEnd", LayoutValue{false}},
        {"shrinkStart", LayoutValue{true}},
        {"shrinkEnd", LayoutValue{true}},
      };

      for (auto& [name, value] : candidates)
      {
        auto changed = original;
        changed.props[name] = std::move(value);
        INFO("split baseline field " << name);
        CHECK(componentBaselineHash(changed) != originalHash);
      }
    }

    SECTION("every collapsible split baseline field changes the hash")
    {
      auto const original = collapsibleSplitNode();
      auto const originalHash = componentBaselineHash(original);
      auto candidates = std::vector<std::pair<std::string, LayoutValue>>{
        {"orientation", LayoutValue{std::string{"vertical"}}},
        {"collapseSide", LayoutValue{std::string{"start"}}},
        {"initialPositionPercent", LayoutValue{0.5}},
        {"position", LayoutValue{static_cast<std::int64_t>(220)}},
        {"revealed", LayoutValue{false}},
      };

      for (auto& [name, value] : candidates)
      {
        auto changed = original;
        changed.props[name] = std::move(value);
        INFO("collapsible split baseline field " << name);
        CHECK(componentBaselineHash(changed) != originalHash);
      }
    }

    SECTION("current encoding remains XXH3 over the owned canonical fields")
    {
      auto const canonical = std::string{"type=split\n"
                                         "orientation=horizontal\n"
                                         "initialPositionPercent=0.2\n"
                                         "position=-1\n"
                                         "resizeStart=true\n"
                                         "resizeEnd=true\n"
                                         "shrinkStart=false\n"
                                         "shrinkEnd=false\n"};

      // This is a current-encoding regression tripwire, not a promise that a
      // future version must preserve the same canonical byte representation.
      // Xxh3Test.cpp independently owns the utility's known-answer vectors.
      CHECK(componentBaselineHash(splitNode()) == utility::xxh3Hash64Hex(canonical));
    }
  }

  TEST_CASE("LayoutComponentState - YAML deserialization handles corrupt files", "[uimodel][unit][layout][component]")
  {
    SECTION("malformed state root is rejected")
    {
      auto const* text = "not-a-map";
      auto tree = ryml::Tree{yaml::callbacks()};
      ryml::parse_in_arena(ryml::to_csubstr(text), &tree);

      auto const decodedRes =
        LayoutComponentStateYamlSchema{}.deserialize(tree.rootref(), LayoutComponentStateDocument{});
      REQUIRE_FALSE(decodedRes);
      CHECK(decodedRes.error().code == Error::Code::FormatRejected);
    }

    SECTION("one malformed component rejects the whole candidate")
    {
      auto const* text = R"(
        version: 1
        preset: modern
        components:
          bad-entry:
            type: split
            stateVersion: 1
          valid-entry:
            type: split
            stateVersion: 1
            baselineHash: abc123
            state:
              positionPercent: 0.33
      )";
      auto tree = ryml::Tree{yaml::callbacks()};
      ryml::parse_in_arena(ryml::to_csubstr(text), &tree);

      auto const decodedRes =
        LayoutComponentStateYamlSchema{}.deserialize(tree.rootref(), LayoutComponentStateDocument{});
      REQUIRE_FALSE(decodedRes);
      CHECK(decodedRes.error().code == Error::Code::FormatRejected);
      CHECK(decodedRes.error().message.contains("bad-entry"));
    }

    SECTION("future file version wins over unknown payload structure")
    {
      auto const* text = R"(
        version: 99
        preset: modern
        components: invalid
        futureField: true
      )";
      auto tree = ryml::Tree{yaml::callbacks()};
      ryml::parse_in_arena(ryml::to_csubstr(text), &tree);
      auto const decodedRes =
        LayoutComponentStateYamlSchema{}.deserialize(tree.rootref(), LayoutComponentStateDocument{});

      REQUIRE_FALSE(decodedRes);
      CHECK(decodedRes.error().code == Error::Code::NotSupported);
    }

    SECTION("unknown structural keys are rejected")
    {
      auto const* text = R"(
        version: 1
        preset: modern
        components: {}
        futureField: true
      )";
      auto tree = ryml::Tree{yaml::callbacks()};
      ryml::parse_in_arena(ryml::to_csubstr(text), &tree);
      auto const decodedRes =
        LayoutComponentStateYamlSchema{}.deserialize(tree.rootref(), LayoutComponentStateDocument{});

      REQUIRE_FALSE(decodedRes);
      CHECK(decodedRes.error().code == Error::Code::FormatRejected);
      CHECK(decodedRes.error().message.contains("futureField"));
    }

    SECTION("unknown component-entry keys are rejected")
    {
      auto const* text = R"(
        version: 1
        preset: modern
        components:
          main-paned:
            type: split
            stateVersion: 1
            baselineHash: abc123
            state: {}
            futureField: true
      )";
      auto tree = ryml::Tree{yaml::callbacks()};
      ryml::parse_in_arena(ryml::to_csubstr(text), &tree);
      auto const decodedRes =
        LayoutComponentStateYamlSchema{}.deserialize(tree.rootref(), LayoutComponentStateDocument{});

      REQUIRE_FALSE(decodedRes);
      CHECK(decodedRes.error().code == Error::Code::FormatRejected);
      CHECK(decodedRes.error().message.contains("futureField"));
    }

    SECTION("future entry version wins over malformed entry payload")
    {
      auto const* text = R"(
        version: 1
        preset: modern
        components:
          main-paned:
            type: []
            stateVersion: 99
            baselineHash: []
            state: invalid
      )";
      auto tree = ryml::Tree{yaml::callbacks()};
      ryml::parse_in_arena(ryml::to_csubstr(text), &tree);
      auto const decodedRes =
        LayoutComponentStateYamlSchema{}.deserialize(tree.rootref(), LayoutComponentStateDocument{});

      REQUIRE_FALSE(decodedRes);
      CHECK(decodedRes.error().code == Error::Code::NotSupported);
    }

    SECTION("empty preset identity is rejected")
    {
      auto const* text = "version: 1\npreset: \"\"\ncomponents: {}\n";
      auto tree = ryml::Tree{yaml::callbacks()};
      ryml::parse_in_arena(ryml::to_csubstr(text), &tree);
      auto const decodedRes =
        LayoutComponentStateYamlSchema{}.deserialize(tree.rootref(), LayoutComponentStateDocument{});

      REQUIRE_FALSE(decodedRes);
      CHECK(decodedRes.error().code == Error::Code::FormatRejected);
      CHECK(decodedRes.error().message.contains("preset"));
    }

    SECTION("empty component identity is rejected")
    {
      auto const* text = R"(
        version: 1
        preset: modern
        components:
          "":
            type: split
            stateVersion: 1
            baselineHash: abc123
            state: {}
      )";
      auto tree = ryml::Tree{yaml::callbacks()};
      ryml::parse_in_arena(ryml::to_csubstr(text), &tree);
      auto const decodedRes =
        LayoutComponentStateYamlSchema{}.deserialize(tree.rootref(), LayoutComponentStateDocument{});

      REQUIRE_FALSE(decodedRes);
      CHECK(decodedRes.error().code == Error::Code::FormatRejected);
    }

    SECTION("empty entry type and baseline identities are rejected")
    {
      auto const* text = R"(
        version: 1
        preset: modern
        components:
          main-paned:
            type: ""
            stateVersion: 1
            baselineHash: ""
            state: {}
      )";
      auto tree = ryml::Tree{yaml::callbacks()};
      ryml::parse_in_arena(ryml::to_csubstr(text), &tree);
      auto const decodedRes =
        LayoutComponentStateYamlSchema{}.deserialize(tree.rootref(), LayoutComponentStateDocument{});

      REQUIRE_FALSE(decodedRes);
      CHECK(decodedRes.error().code == Error::Code::FormatRejected);
      CHECK(decodedRes.error().message.contains("type"));
      CHECK(decodedRes.error().message.contains("baselineHash"));
    }

    SECTION("components must be a mapping")
    {
      auto const* text = "version: 1\npreset: modern\ncomponents: []\n";
      auto tree = ryml::Tree{yaml::callbacks()};
      ryml::parse_in_arena(ryml::to_csubstr(text), &tree);
      auto const decodedRes =
        LayoutComponentStateYamlSchema{}.deserialize(tree.rootref(), LayoutComponentStateDocument{});

      REQUIRE_FALSE(decodedRes);
      CHECK(decodedRes.error().code == Error::Code::FormatRejected);
      CHECK(decodedRes.error().message.contains("components"));
    }

    SECTION("component entries must be mappings")
    {
      auto const* text = R"(
        version: 1
        preset: modern
        components:
          main-paned: invalid
      )";
      auto tree = ryml::Tree{yaml::callbacks()};
      ryml::parse_in_arena(ryml::to_csubstr(text), &tree);
      auto const decodedRes =
        LayoutComponentStateYamlSchema{}.deserialize(tree.rootref(), LayoutComponentStateDocument{});

      REQUIRE_FALSE(decodedRes);
      CHECK(decodedRes.error().code == Error::Code::FormatRejected);
      CHECK(decodedRes.error().message.contains("main-paned"));
    }

    SECTION("entry state must be a mapping")
    {
      auto const* text = R"(
        version: 1
        preset: modern
        components:
          main-paned:
            type: split
            stateVersion: 1
            baselineHash: abc123
            state: []
      )";
      auto tree = ryml::Tree{yaml::callbacks()};
      ryml::parse_in_arena(ryml::to_csubstr(text), &tree);
      auto const decodedRes =
        LayoutComponentStateYamlSchema{}.deserialize(tree.rootref(), LayoutComponentStateDocument{});

      REQUIRE_FALSE(decodedRes);
      CHECK(decodedRes.error().code == Error::Code::FormatRejected);
      CHECK(decodedRes.error().message.contains("state"));
    }
  }

  TEST_CASE("LayoutComponentState - pruning removes invalid entries", "[uimodel][unit][layout][component]")
  {
    auto const withExpandedControls = GENERATE(false, true);
    INFO("Expanded pruning controls: " << withExpandedControls);
    auto const liveNode = splitNode("live-split");
    auto const wrongTypeNode = splitNode("wrong-type");
    auto const staleVersionNode = splitNode("stale-version");
    auto const staleBaselineNode = splitNode("stale-baseline");
    auto const templatedNode = splitNode("templated-split");

    auto doc = LayoutDocument{};
    doc.root.type = "box";
    doc.root.children = {liveNode, LayoutNode{.id = "wrong-type", .type = "split"}};

    auto stateDoc = LayoutComponentStateDocument{};
    stateDoc.preset = "classic";
    stateDoc.components["live-split"] = LayoutComponentStateEntry{
      .type = "split",
      .stateVersion = kStateEntryVersion,
      .baselineHash = componentBaselineHash(liveNode),
      .state = {{"positionPercent", LayoutValue{0.25}}},
    };
    stateDoc.components["deleted-split"] = LayoutComponentStateEntry{
      .type = "split",
      .stateVersion = kStateEntryVersion,
      .baselineHash = "orphan",
      .state = {{"positionPercent", LayoutValue{0.50}}},
    };
    stateDoc.components["wrong-type"] = LayoutComponentStateEntry{
      .type = "collapsibleSplit",
      .stateVersion = kStateEntryVersion,
      .baselineHash = componentBaselineHash(liveNode),
      .state = {{"positionPercent", LayoutValue{0.75}}},
    };

    if (withExpandedControls)
    {
      doc.root.children[1] = wrongTypeNode;
      stateDoc.components.at("wrong-type").baselineHash = componentBaselineHash(wrongTypeNode);
      doc.root.children.push_back(staleVersionNode);
      doc.root.children.push_back(staleBaselineNode);
      doc.templates["pane"] = templatedNode;
      doc.root.children.push_back(
        LayoutNode{.type = "template", .props = {{"templateId", LayoutValue{std::string{"pane"}}}}});
      stateDoc.components["templated-split"] = LayoutComponentStateEntry{
        .type = "split",
        .stateVersion = kStateEntryVersion,
        .baselineHash = componentBaselineHash(templatedNode),
        .state = {{"positionPercent", LayoutValue{0.30}}},
      };
      stateDoc.components["stale-version"] = LayoutComponentStateEntry{
        .type = "split",
        .stateVersion = 99,
        .baselineHash = componentBaselineHash(staleVersionNode),
        .state = {{"positionPercent", LayoutValue{0.60}}},
      };
      stateDoc.components["stale-baseline"] = LayoutComponentStateEntry{
        .type = "split",
        .stateVersion = kStateEntryVersion,
        .baselineHash = "stale",
        .state = {{"positionPercent", LayoutValue{0.70}}},
      };
    }

    auto const preparedRes = prepareLayout(doc);
    REQUIRE(preparedRes);

    SECTION("current file version retains only matching expanded nodes")
    {
      pruneComponentState(stateDoc, *preparedRes, persistentLayoutSchema());

      if (withExpandedControls)
      {
        REQUIRE(stateDoc.components.size() == 2);
        CHECK(stateDoc.components.contains("templated-split"));
      }
      else
      {
        CHECK(stateDoc.components.size() == 1);
      }

      CHECK(stateDoc.components.contains("live-split"));
    }

    SECTION("unsupported file version clears every entry")
    {
      stateDoc.version = 99;
      pruneComponentState(stateDoc, *preparedRes, persistentLayoutSchema());

      CHECK(stateDoc.components.empty());
    }
  }
} // namespace ao::uimodel::test
