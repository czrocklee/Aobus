// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/uimodel/layout/document/LayoutDocument.h>
#include <ao/uimodel/layout/document/LayoutNode.h>
#include <ao/uimodel/layout/document/LayoutTemplateExpansion.h>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string>
#include <utility>

namespace ao::uimodel::test
{
  TEST_CASE("LayoutTemplateExpansion - expands registered templates", "[uimodel][unit][layout][document]")
  {
    auto doc = LayoutDocument{};
    doc.templates["playback.transportGroup"] = LayoutNode{
      .type = "box",
      .props = {{"spacing", LayoutValue{static_cast<std::int64_t>(0)}}},
      .children = {LayoutNode{.type = "playback.playPauseButton"}, LayoutNode{.type = "playback.stopButton"}}};

    doc.root.type = "template";
    doc.root.props["templateId"] = LayoutValue{std::string{"playback.transportGroup"}};

    auto const expanded = expandLayoutTemplates(doc);

    CHECK(expanded.type == "box");
    CHECK(expanded.children.size() == 2);
    CHECK(expanded.children[0].type == "playback.playPauseButton");
    CHECK(expanded.children[1].type == "playback.stopButton");
  }

  TEST_CASE("LayoutTemplateExpansion - lets use sites override node id", "[uimodel][unit][layout][document]")
  {
    auto doc = LayoutDocument{};
    doc.templates["my.template"] = LayoutNode{.type = "spacer"};

    doc.root.type = "template";
    doc.root.id = "my-override-id";
    doc.root.props["templateId"] = LayoutValue{std::string{"my.template"}};

    auto const expanded = expandLayoutTemplates(doc);

    CHECK(expanded.type == "spacer");
    CHECK(expanded.id == "my-override-id");
  }

  TEST_CASE("LayoutTemplateExpansion - merges layout property overrides", "[uimodel][unit][layout][document]")
  {
    auto doc = LayoutDocument{};
    doc.templates["my.template"] = LayoutNode{.type = "box", .layout = {{"hexpand", LayoutValue{true}}}};

    doc.root.type = "template";
    doc.root.props["templateId"] = LayoutValue{std::string{"my.template"}};
    doc.root.layout["vexpand"] = LayoutValue{true};

    auto const expanded = expandLayoutTemplates(doc);

    CHECK(expanded.type == "box");
    CHECK(expanded.layout.at("hexpand").asBool() == true);
    CHECK(expanded.layout.at("vexpand").asBool() == true);
  }

  TEST_CASE("LayoutTemplateExpansion - merges prop overrides except templateId", "[uimodel][unit][layout][document]")
  {
    auto doc = LayoutDocument{};
    doc.templates["my.template"] =
      LayoutNode{.type = "app.actionButton", .props = {{"label", LayoutValue{std::string{"Original"}}}}};

    doc.root.type = "template";
    doc.root.props["templateId"] = LayoutValue{std::string{"my.template"}};
    doc.root.props["label"] = LayoutValue{std::string{"Override"}};
    doc.root.props["icon"] = LayoutValue{std::string{"emblem-system"}};

    auto const expanded = expandLayoutTemplates(doc);

    CHECK(expanded.props.at("label").asString() == "Override");
    CHECK(expanded.props.at("icon").asString() == "emblem-system");
    CHECK(expanded.props.find("templateId") == expanded.props.end());
  }

  TEST_CASE("LayoutTemplateExpansion - appends use-site children after expanded children",
            "[uimodel][unit][layout][document]")
  {
    auto doc = LayoutDocument{};
    doc.templates["my.template"] = LayoutNode{.type = "box", .children = {LayoutNode{.type = "spacer"}}};

    doc.root.type = "template";
    doc.root.props["templateId"] = LayoutValue{std::string{"my.template"}};
    doc.root.children = {LayoutNode{.type = "playback.playPauseButton"}};

    auto const expanded = expandLayoutTemplates(doc);

    REQUIRE(expanded.children.size() == 2);
    CHECK(expanded.children[0].type == "spacer");
    CHECK(expanded.children[1].type == "playback.playPauseButton");
  }

  TEST_CASE("LayoutTemplateExpansion - recurses into non-template children", "[uimodel][unit][layout][document]")
  {
    auto doc = LayoutDocument{};
    doc.templates["inner.template"] = LayoutNode{.type = "spacer"};

    doc.root.type = "box";
    doc.root.children = {
      LayoutNode{.type = "template", .props = {{"templateId", LayoutValue{std::string{"inner.template"}}}}}};

    auto const expanded = expandLayoutTemplates(doc);

    REQUIRE(expanded.children.size() == 1);
    CHECK(expanded.children[0].type == "spacer");
  }

  TEST_CASE("LayoutTemplateExpansion - integrates tooltip overrides", "[uimodel][unit][layout][document]")
  {
    auto doc = LayoutDocument{};
    auto templateTooltip = LayoutNode{.type = "my.templateTooltip"};
    doc.templates["my.template"] =
      LayoutNode{.type = "app.actionButton", .optTooltip = BoxedLayoutNode{std::move(templateTooltip)}};

    doc.root.type = "template";
    doc.root.props["templateId"] = LayoutValue{std::string{"my.template"}};

    auto useSiteTooltip = LayoutNode{.type = "my.useSiteTooltip"};
    doc.root.optTooltip = BoxedLayoutNode{std::move(useSiteTooltip)};

    auto const expanded = expandLayoutTemplates(doc);

    CHECK(expanded.type == "app.actionButton");
    REQUIRE(expanded.optTooltip);
    REQUIRE(expanded.optTooltip->nodePtr != nullptr);
    CHECK(expanded.optTooltip->nodePtr->type == "my.useSiteTooltip");
  }

  TEST_CASE("LayoutTemplateExpansion - recurses into non-template tooltip", "[uimodel][unit][layout][document]")
  {
    auto doc = LayoutDocument{};
    doc.templates["inner.template"] = LayoutNode{.type = "spacer"};

    doc.root.type = "box";
    auto tooltipNode =
      LayoutNode{.type = "template", .props = {{"templateId", LayoutValue{std::string{"inner.template"}}}}};
    doc.root.optTooltip = BoxedLayoutNode{std::move(tooltipNode)};

    auto const expanded = expandLayoutTemplates(doc);

    CHECK(expanded.type == "box");
    REQUIRE(expanded.optTooltip);
    REQUIRE(expanded.optTooltip->nodePtr != nullptr);
    CHECK(expanded.optTooltip->nodePtr->type == "spacer");
  }

  TEST_CASE("LayoutTemplateExpansion - returns an error node for missing templateId",
            "[uimodel][unit][layout][document]")
  {
    auto doc = LayoutDocument{};
    doc.root.type = "template";
    // No templateId prop

    auto const expanded = expandLayoutTemplates(doc);

    CHECK(expanded.type == "[TemplateError] Missing templateId");
  }

  TEST_CASE("LayoutTemplateExpansion - returns an error node for unknown template id",
            "[uimodel][unit][layout][document]")
  {
    auto doc = LayoutDocument{};
    doc.root.type = "template";
    doc.root.props["templateId"] = LayoutValue{std::string{"nonexistent"}};

    auto const expanded = expandLayoutTemplates(doc);

    CHECK(expanded.type == "[TemplateError] Unknown template: nonexistent");
  }

  TEST_CASE("LayoutTemplateExpansion - returns an error node for recursive template loops",
            "[uimodel][unit][layout][document]")
  {
    auto doc = LayoutDocument{};
    doc.templates["selfRef"] =
      LayoutNode{.type = "template", .props = {{"templateId", LayoutValue{std::string{"selfRef"}}}}};

    doc.root.type = "template";
    doc.root.props["templateId"] = LayoutValue{std::string{"selfRef"}};

    auto const expanded = expandLayoutTemplates(doc);

    CHECK(expanded.type == "[TemplateError] Recursive template loop: selfRef -> selfRef");
  }
} // namespace ao::uimodel::test
