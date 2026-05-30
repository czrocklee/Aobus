// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/uimodel/layout/LayoutDocument.h>
#include <ao/uimodel/layout/LayoutNode.h>
#include <ao/uimodel/layout/LayoutTemplateExpander.h>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string>
#include <utility>

namespace ao::uimodel::layout::test
{
  TEST_CASE("LayoutTemplateExpander normal expansion", "[layout][unit][template]")
  {
    auto doc = LayoutDocument{};
    doc.templates["playback.transportGroup"] = LayoutNode{
      .type = "box",
      .props = {{"spacing", LayoutValue{static_cast<std::int64_t>(0)}}},
      .children = {LayoutNode{.type = "playback.playPauseButton"}, LayoutNode{.type = "playback.stopButton"}}};

    doc.root.type = "template";
    doc.root.props["templateId"] = LayoutValue{std::string{"playback.transportGroup"}};

    auto const expanded = LayoutTemplateExpander::expand(doc);

    CHECK(expanded.type == "box");
    CHECK(expanded.children.size() == 2);
    CHECK(expanded.children[0].type == "playback.playPauseButton");
    CHECK(expanded.children[1].type == "playback.stopButton");
  }

  TEST_CASE("LayoutTemplateExpander overrides node id", "[layout][unit][template]")
  {
    auto doc = LayoutDocument{};
    doc.templates["my.template"] = LayoutNode{.type = "spacer"};

    doc.root.type = "template";
    doc.root.id = "my-override-id";
    doc.root.props["templateId"] = LayoutValue{std::string{"my.template"}};

    auto const expanded = LayoutTemplateExpander::expand(doc);

    CHECK(expanded.type == "spacer");
    CHECK(expanded.id == "my-override-id");
  }

  TEST_CASE("LayoutTemplateExpander merges layout property overrides", "[layout][unit][template]")
  {
    auto doc = LayoutDocument{};
    doc.templates["my.template"] = LayoutNode{.type = "box", .layout = {{"hexpand", LayoutValue{true}}}};

    doc.root.type = "template";
    doc.root.props["templateId"] = LayoutValue{std::string{"my.template"}};
    doc.root.layout["vexpand"] = LayoutValue{true};

    auto const expanded = LayoutTemplateExpander::expand(doc);

    CHECK(expanded.type == "box");
    CHECK(expanded.layout.at("hexpand").asBool() == true);
    CHECK(expanded.layout.at("vexpand").asBool() == true);
  }

  TEST_CASE("LayoutTemplateExpander merges prop overrides except templateId", "[layout][unit][template]")
  {
    auto doc = LayoutDocument{};
    doc.templates["my.template"] =
      LayoutNode{.type = "app.actionButton", .props = {{"label", LayoutValue{std::string{"Original"}}}}};

    doc.root.type = "template";
    doc.root.props["templateId"] = LayoutValue{std::string{"my.template"}};
    doc.root.props["label"] = LayoutValue{std::string{"Override"}};
    doc.root.props["icon"] = LayoutValue{std::string{"emblem-system"}};

    auto const expanded = LayoutTemplateExpander::expand(doc);

    CHECK(expanded.props.at("label").asString() == "Override");
    CHECK(expanded.props.at("icon").asString() == "emblem-system");
    CHECK(expanded.props.find("templateId") == expanded.props.end());
  }

  TEST_CASE("LayoutTemplateExpander appends use-site children after expanded children", "[layout][unit][template]")
  {
    auto doc = LayoutDocument{};
    doc.templates["my.template"] = LayoutNode{.type = "box", .children = {LayoutNode{.type = "spacer"}}};

    doc.root.type = "template";
    doc.root.props["templateId"] = LayoutValue{std::string{"my.template"}};
    doc.root.children = {LayoutNode{.type = "playback.playPauseButton"}};

    auto const expanded = LayoutTemplateExpander::expand(doc);

    REQUIRE(expanded.children.size() == 2);
    CHECK(expanded.children[0].type == "spacer");
    CHECK(expanded.children[1].type == "playback.playPauseButton");
  }

  TEST_CASE("LayoutTemplateExpander recurses into non-template children", "[layout][unit][template]")
  {
    auto doc = LayoutDocument{};
    doc.templates["inner.template"] = LayoutNode{.type = "spacer"};

    doc.root.type = "box";
    doc.root.children = {
      LayoutNode{.type = "template", .props = {{"templateId", LayoutValue{std::string{"inner.template"}}}}}};

    auto const expanded = LayoutTemplateExpander::expand(doc);

    REQUIRE(expanded.children.size() == 1);
    CHECK(expanded.children[0].type == "spacer");
  }

  TEST_CASE("LayoutTemplateExpander integrates tooltip overrides", "[layout][unit][template]")
  {
    auto doc = LayoutDocument{};
    auto templateTooltip = LayoutNode{.type = "my.templateTooltip"};
    doc.templates["my.template"] =
      LayoutNode{.type = "app.actionButton", .optTooltip = BoxedLayoutNode{std::move(templateTooltip)}};

    doc.root.type = "template";
    doc.root.props["templateId"] = LayoutValue{std::string{"my.template"}};

    auto useSiteTooltip = LayoutNode{.type = "my.useSiteTooltip"};
    doc.root.optTooltip = BoxedLayoutNode{std::move(useSiteTooltip)};

    auto const expanded = LayoutTemplateExpander::expand(doc);

    CHECK(expanded.type == "app.actionButton");
    REQUIRE(expanded.optTooltip.has_value());
    REQUIRE(expanded.optTooltip->nodePtr != nullptr);
    CHECK(expanded.optTooltip->nodePtr->type == "my.useSiteTooltip");
  }

  TEST_CASE("LayoutTemplateExpander recurses into non-template tooltip", "[layout][unit][template]")
  {
    auto doc = LayoutDocument{};
    doc.templates["inner.template"] = LayoutNode{.type = "spacer"};

    doc.root.type = "box";
    auto tooltipNode =
      LayoutNode{.type = "template", .props = {{"templateId", LayoutValue{std::string{"inner.template"}}}}};
    doc.root.optTooltip = BoxedLayoutNode{std::move(tooltipNode)};

    auto const expanded = LayoutTemplateExpander::expand(doc);

    CHECK(expanded.type == "box");
    REQUIRE(expanded.optTooltip.has_value());
    REQUIRE(expanded.optTooltip->nodePtr != nullptr);
    CHECK(expanded.optTooltip->nodePtr->type == "spacer");
  }

  TEST_CASE("LayoutTemplateExpander error on missing templateId", "[layout][unit][template]")
  {
    auto doc = LayoutDocument{};
    doc.root.type = "template";
    // No templateId prop

    auto const expanded = LayoutTemplateExpander::expand(doc);

    CHECK(expanded.type.find("[TemplateError]") != std::string::npos);
    CHECK(expanded.type.find("Missing templateId") != std::string::npos);
  }

  TEST_CASE("LayoutTemplateExpander error on unknown template id", "[layout][unit][template]")
  {
    auto doc = LayoutDocument{};
    doc.root.type = "template";
    doc.root.props["templateId"] = LayoutValue{std::string{"nonexistent"}};

    auto const expanded = LayoutTemplateExpander::expand(doc);

    CHECK(expanded.type.find("[TemplateError]") != std::string::npos);
    CHECK(expanded.type.find("Unknown template") != std::string::npos);
  }

  TEST_CASE("LayoutTemplateExpander error on recursive template loop", "[layout][unit][template]")
  {
    auto doc = LayoutDocument{};
    doc.templates["selfRef"] =
      LayoutNode{.type = "template", .props = {{"templateId", LayoutValue{std::string{"selfRef"}}}}};

    doc.root.type = "template";
    doc.root.props["templateId"] = LayoutValue{std::string{"selfRef"}};

    auto const expanded = LayoutTemplateExpander::expand(doc);

    CHECK(expanded.type.find("[TemplateError]") != std::string::npos);
    CHECK(expanded.type.find("Recursive template loop") != std::string::npos);
  }
}
