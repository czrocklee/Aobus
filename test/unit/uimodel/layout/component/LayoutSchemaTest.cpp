// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include <ao/uimodel/layout/component/LayoutSchema.h>

#include <ao/uimodel/layout/component/LayoutSurface.h>
#include <ao/uimodel/layout/document/LayoutNode.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace ao::uimodel::test
{
  namespace
  {
    ComponentSchema buttonSchema()
    {
      return {.id = "test.button",
              .displayName = "Test Button",
              .category = ComponentCategory::Generic,
              .optMaxChildren = 0,
              .actionSlots = actionSlotBit(ActionSlot::PrimaryClick) | actionSlotBit(ActionSlot::SecondaryClick),
              .defaultActions = {{ActionSlot::PrimaryClick, "valid.action"}}};
    }

    PropertySchema const* property(ComponentSchema const& schema, std::string_view const name)
    {
      auto const it = std::ranges::find(schema.properties, name, &PropertySchema::name);
      return it == schema.properties.end() ? nullptr : &*it;
    }

    std::string actionSlotName(ActionSlot const slot)
    {
      switch (slot)
      {
        case ActionSlot::PrimaryClick: return "primary click";
        case ActionSlot::PrimaryLongPress: return "primary long press";
        case ActionSlot::SecondaryClick: return "secondary click";
        case ActionSlot::SecondaryLongPress: return "secondary long press";
      }

      return "unknown";
    }

    std::string propertyKindName(PropertyKind const kind)
    {
      switch (kind)
      {
        case PropertyKind::Bool: return "Bool";
        case PropertyKind::Int: return "Int";
        case PropertyKind::Double: return "Double";
        case PropertyKind::String: return "String";
        case PropertyKind::Enum: return "Enum";
        case PropertyKind::StringList: return "StringList";
        case PropertyKind::Size: return "Size";
      }

      return "Unknown";
    }

    std::string defaultValueText(LayoutValue const& value)
    {
      if (value.getIf<std::monostate>() != nullptr)
      {
        return "none";
      }

      if (auto const* const boolean = value.getIf<bool>(); boolean != nullptr)
      {
        return *boolean ? "`true`" : "`false`";
      }

      if (auto const* const integer = value.getIf<std::int64_t>(); integer != nullptr)
      {
        return std::format("`{}`", *integer);
      }

      if (auto const* const number = value.getIf<double>(); number != nullptr)
      {
        return std::format("`{:g}`", *number);
      }

      if (auto const* const text = value.getIf<std::string>(); text != nullptr)
      {
        return text->empty() ? "empty" : std::format("`{}`", *text);
      }

      auto const* const values = value.getIf<std::vector<std::string>>();
      REQUIRE(values != nullptr);
      auto result = std::string{};

      for (auto const& entry : *values)
      {
        result += result.empty() ? std::format("`{}`", entry) : std::format(", `{}`", entry);
      }

      return result.empty() ? "empty" : result;
    }

    std::string propertyNames(ComponentSchema const& schema)
    {
      auto result = std::string{};

      for (auto const& entry : schema.properties)
      {
        result += result.empty() ? std::format("`{}`", entry.name) : std::format(", `{}`", entry.name);
      }

      return result.empty() ? "none" : result;
    }

    std::string enumValues(PropertySchema const& propertySchema)
    {
      auto result = std::string{};

      for (auto const& entry : propertySchema.enumValues)
      {
        result += result.empty() ? std::format("`{}`", entry) : std::format(", `{}`", entry);
      }

      return result.empty() ? "any" : result;
    }

    std::string actionSlots(ComponentSchema const& schema)
    {
      static constexpr auto kSlots = std::array{ActionSlot::PrimaryClick,
                                                ActionSlot::PrimaryLongPress,
                                                ActionSlot::SecondaryClick,
                                                ActionSlot::SecondaryLongPress};
      auto result = std::string{};

      for (auto const slot : kSlots)
      {
        if (schema.allows(slot))
        {
          result += result.empty() ? actionSlotName(slot) : std::format(", {}", actionSlotName(slot));
        }
      }

      return result.empty() ? "none" : result;
    }

    std::string defaultActions(ComponentSchema const& schema)
    {
      auto result = std::string{};

      for (auto const& binding : schema.defaultActions)
      {
        auto const entry = std::format("{} → `{}`", actionSlotName(binding.slot), binding.actionId);
        result += result.empty() ? entry : std::format(", {}", entry);
      }

      return result.empty() ? "none" : result;
    }

    std::string childRange(ComponentSchema const& schema)
    {
      return schema.optMaxChildren ? std::format("{}..{}", schema.minChildren, *schema.optMaxChildren)
                                   : std::format("{}..any", schema.minChildren);
    }

    std::string surfaces(ComponentSchema const& schema)
    {
      auto result = std::string{};

      if (supportsSurface(schema.surfaces, LayoutSurface::Main))
      {
        result = "main";
      }

      if (supportsSurface(schema.surfaces, LayoutSurface::Tooltip))
      {
        result += result.empty() ? "tooltip" : ", tooltip";
      }

      return result.empty() ? "none" : result;
    }

    std::string generatedSharedSchemaReference()
    {
      auto result = std::string{"<!-- BEGIN GENERATED SHARED COMPONENT SCHEMA -->\n"
                                "| Type | Display name | Category | Children | Surfaces | Persistent state | Shared "
                                "properties | Action slots | Default actions |\n"
                                "|---|---|---|---|---|---|---|---|---|\n"};

      for (auto const& component : sharedComponentSchemas())
      {
        result += std::format("| `{}` | {} | {} | {} | {} | {} | {} | {} | {} |\n",
                              component.id,
                              component.displayName,
                              toString(component.category),
                              childRange(component),
                              surfaces(component),
                              component.persistentState ? "yes" : "no",
                              propertyNames(component),
                              actionSlots(component),
                              defaultActions(component));
      }

      result += "\n| Component | Property | Kind | Values | Default | Action slot |\n"
                "|---|---|---|---|---|---|\n";

      for (auto const& component : sharedComponentSchemas())
      {
        for (auto const& propertySchema : component.properties)
        {
          result += std::format("| `{}` | `{}` | {} | {} | {} | {} |\n",
                                component.id,
                                propertySchema.name,
                                propertyKindName(propertySchema.kind),
                                enumValues(propertySchema),
                                defaultValueText(propertySchema.defaultValue),
                                propertySchema.optActionSlot ? actionSlotName(*propertySchema.optActionSlot) : "none");
        }
      }

      result += "<!-- END GENERATED SHARED COMPONENT SCHEMA -->";
      return result;
    }
  } // namespace

  TEST_CASE("LayoutSchema - component and action entries retain registration order", "[uimodel][unit][layout][schema]")
  {
    auto schema = LayoutSchema{};

    REQUIRE(schema.addComponent(buttonSchema()));
    REQUIRE(schema.addComponent({.id = "test.label", .displayName = "Label", .optMaxChildren = 0}));
    REQUIRE(schema.addAction({.id = "valid.action", .label = "Valid", .category = "Test"}));
    REQUIRE(schema.addAction({.id = "other.action", .label = "Other", .category = "Test"}));

    REQUIRE(schema.components().size() == 2);
    CHECK(schema.components()[0].id == "test.button");
    CHECK(schema.components()[1].id == "test.label");
    REQUIRE(schema.actions().size() == 2);
    CHECK(schema.actions()[0].id == "valid.action");
    CHECK(schema.actions()[1].id == "other.action");

    REQUIRE(schema.component("test.button"));
    CHECK(schema.component("test.button")->displayName == "Test Button");
    REQUIRE(schema.action("valid.action"));
    CHECK(schema.action("valid.action")->label == "Valid");
    CHECK_FALSE(schema.component("missing"));
    CHECK_FALSE(schema.action("missing"));
  }

  TEST_CASE("LayoutSchema - duplicate ids preserve the first entry", "[uimodel][unit][layout][schema]")
  {
    auto schema = LayoutSchema{};
    REQUIRE(schema.addComponent(buttonSchema()));
    REQUIRE(schema.addAction({.id = "valid.action", .label = "First", .category = "Test"}));

    CHECK_FALSE(schema.addComponent({.id = "test.button", .displayName = "Replacement"}));
    CHECK_FALSE(schema.addAction({.id = "valid.action", .label = "Replacement", .category = "Other"}));

    REQUIRE(schema.component("test.button"));
    CHECK(schema.component("test.button")->displayName == "Test Button");
    REQUIRE(schema.action("valid.action"));
    CHECK(schema.action("valid.action")->label == "First");
  }

  TEST_CASE("ComponentSchema - action slots resolve authored values, defaults, and explicit unbinding",
            "[uimodel][unit][layout][schema]")
  {
    auto schema = LayoutSchema{};
    REQUIRE(schema.addComponent(buttonSchema()));
    auto const component = *schema.component("test.button");

    auto const* primary = property(component, kPrimaryActionProp);
    REQUIRE(primary != nullptr);
    CHECK(primary->kind == PropertyKind::Enum);
    CHECK(primary->optActionSlot == ActionSlot::PrimaryClick);
    CHECK(property(component, kSecondaryActionProp) != nullptr);
    CHECK(property(component, kPrimaryLongPressActionProp) == nullptr);

    auto node = LayoutNode{.type = "test.button"};
    CHECK(component.actionId(node, ActionSlot::PrimaryClick) == "valid.action");
    CHECK_FALSE(component.actionId(node, ActionSlot::SecondaryClick));
    CHECK_FALSE(component.actionId(node, ActionSlot::PrimaryLongPress));

    node.props[std::string{kPrimaryLongPressActionProp}] = LayoutValue{std::string{"other.action"}};
    CHECK_FALSE(component.actionId(node, ActionSlot::PrimaryLongPress));

    node.props[std::string{kSecondaryActionProp}] = LayoutValue{std::string{"other.action"}};
    CHECK(component.actionId(node, ActionSlot::SecondaryClick) == "other.action");
    CHECK(component.hasBoundAction(node));
    CHECK((component.boundActionSlots(node) & actionSlotBit(ActionSlot::SecondaryClick)) != 0);

    for (auto const& unbound : {std::string{}, std::string{"none"}})
    {
      node.props[std::string{kPrimaryActionProp}] = LayoutValue{unbound};
      CHECK_FALSE(component.actionId(node, ActionSlot::PrimaryClick));
    }
  }

  TEST_CASE("ComponentSchema - copied action ids survive backing value replacement", "[uimodel][unit][layout][schema]")
  {
    auto component = buttonSchema();
    auto authoredNode = LayoutNode{.type = "test.button"};
    authoredNode.props[std::string{kSecondaryActionProp}] = LayoutValue{std::string{"other.action"}};
    auto copiedAuthoredId = std::string{};

    {
      auto const optActionId = component.actionId(authoredNode, ActionSlot::SecondaryClick);
      REQUIRE(optActionId);
      copiedAuthoredId.assign(*optActionId);
    }

    authoredNode.props[std::string{kSecondaryActionProp}] = LayoutValue{std::string{"replacement.action"}};

    auto const defaultNode = LayoutNode{.type = "test.button"};
    auto copiedDefaultId = std::string{};

    {
      auto const optActionId = component.actionId(defaultNode, ActionSlot::PrimaryClick);
      REQUIRE(optActionId);
      copiedDefaultId.assign(*optActionId);
    }

    component.defaultActions.clear();

    CHECK(copiedAuthoredId == "other.action");
    CHECK(copiedDefaultId == "valid.action");
    CHECK(component.actionId(authoredNode, ActionSlot::SecondaryClick) == "replacement.action");
    CHECK_FALSE(component.actionId(defaultNode, ActionSlot::PrimaryClick));
  }

  TEST_CASE("LayoutSchema - generated action properties cannot overwrite authored schema",
            "[uimodel][unit][layout][schema]")
  {
    auto schema = LayoutSchema{};
    auto candidate = buttonSchema();
    candidate.properties.push_back(
      {.name = std::string{kPrimaryActionProp}, .kind = PropertyKind::String, .label = "Conflicting action"});

    CHECK_FALSE(schema.addComponent(candidate));
    CHECK(schema.components().empty());
  }

  TEST_CASE("LayoutSchema - defaults cannot target a disallowed component action slot",
            "[uimodel][unit][layout][schema]")
  {
    auto schema = LayoutSchema{};
    auto candidate = buttonSchema();
    candidate.defaultActions.push_back({ActionSlot::PrimaryLongPress, "valid.action"});

    CHECK_FALSE(schema.addComponent(candidate));
    CHECK(schema.components().empty());
  }

  TEST_CASE("LayoutSchema - shared component inventory is canonical and uniquely keyed",
            "[uimodel][unit][layout][schema]")
  {
    auto const shared = sharedComponentSchemas();
    REQUIRE(shared.size() == 20);

    auto ids = std::vector<std::string_view>{};
    ids.reserve(shared.size());

    for (auto const& component : shared)
    {
      CHECK_FALSE(component.id.empty());
      CHECK_FALSE(component.displayName.empty());
      CHECK_FALSE(std::ranges::contains(ids, component.id));
      ids.push_back(component.id);
    }

    auto const split = std::ranges::find(shared, std::string_view{"split"}, &ComponentSchema::id);
    REQUIRE(split != shared.end());
    CHECK(split->persistentState);
    CHECK(split->minChildren == 2);
    CHECK(split->optMaxChildren == 2);
  }

  TEST_CASE("LayoutSchema - frontend extensions preserve the shared contract", "[uimodel][unit][layout][schema]")
  {
    auto schema = LayoutSchema{};
    REQUIRE(
      schema.addSharedComponent("actionButton",
                                {.properties = {{.name = "glyph", .kind = PropertyKind::String, .label = "Glyph"}},
                                 .actionSlots = actionSlotBit(ActionSlot::SecondaryClick),
                                 .defaultActions = {{ActionSlot::SecondaryClick, "shell.menu"}}}));

    auto const extended = *schema.component("actionButton");
    CHECK(property(extended, kTextProp) != nullptr);
    CHECK(property(extended, "glyph") != nullptr);
    CHECK(extended.allows(ActionSlot::PrimaryClick));
    CHECK(extended.allows(ActionSlot::SecondaryClick));
    CHECK(extended.defaultAction(ActionSlot::SecondaryClick) == "shell.menu");

    auto const canonical =
      std::ranges::find(sharedComponentSchemas(), std::string_view{"actionButton"}, &ComponentSchema::id);
    REQUIRE(canonical != sharedComponentSchemas().end());
    CHECK(property(*canonical, "glyph") == nullptr);
    CHECK_FALSE(canonical->allows(ActionSlot::SecondaryClick));
  }

  TEST_CASE("LayoutSchema - a shared id cannot bypass the canonical vocabulary through addComponent",
            "[uimodel][unit][layout][schema]")
  {
    auto const canonical =
      std::ranges::find(sharedComponentSchemas(), std::string_view{"actionButton"}, &ComponentSchema::id);
    REQUIRE(canonical != sharedComponentSchemas().end());
    auto candidate = *canonical;

    SECTION("display name")
    {
      candidate.displayName = "Different button";
    }

    SECTION("category")
    {
      candidate.category = ComponentCategory::Layout;
    }

    SECTION("child range")
    {
      candidate.optMaxChildren = 1;
    }

    SECTION("persistent-state policy")
    {
      candidate.persistentState = true;
    }

    SECTION("action-slot floor")
    {
      candidate.actionSlots &= ~actionSlotBit(ActionSlot::PrimaryClick);
    }

    SECTION("property shape")
    {
      REQUIRE_FALSE(candidate.properties.empty());
      candidate.properties.front().kind = PropertyKind::Bool;
    }

    auto schema = LayoutSchema{};
    CHECK_FALSE(schema.addComponent(candidate));
    CHECK(schema.components().empty());
  }

  TEST_CASE("LayoutSchema - shared component reference is generated from the canonical table",
            "[uimodel][unit][layout][schema]")
  {
    auto input = std::ifstream{std::filesystem::path{AOBUS_SOURCE_DIR} / "doc/reference/shell/component-vocabulary.md"};
    REQUIRE(input);
    auto const document = std::string{std::istreambuf_iterator{input}, std::istreambuf_iterator<char>{}};
    auto const generated = generatedSharedSchemaReference();
    auto const begin = document.find("<!-- BEGIN GENERATED SHARED COMPONENT SCHEMA -->");
    REQUIRE(begin != std::string::npos);
    auto const endMarker = std::string_view{"<!-- END GENERATED SHARED COMPONENT SCHEMA -->"};
    auto const end = document.find(endMarker, begin);
    REQUIRE(end != std::string::npos);

    CHECK(document.substr(begin, end + endMarker.size() - begin) == generated);
  }

  TEST_CASE("LayoutSchema - invalid shared extensions fail closed", "[uimodel][unit][layout][schema]")
  {
    SECTION("unknown shared id")
    {
      auto schema = LayoutSchema{};
      CHECK_FALSE(schema.addSharedComponent("frontend.only"));
      CHECK(schema.components().empty());
    }

    SECTION("a frontend cannot redeclare a canonical property")
    {
      auto schema = LayoutSchema{};
      CHECK_FALSE(
        schema.addSharedComponent("box", {.properties = {{.name = std::string{kOrientationProp}, .label = "Other"}}}));
      CHECK(schema.components().empty());
    }

    SECTION("a default cannot target a slot the resulting component does not support")
    {
      auto schema = LayoutSchema{};
      CHECK_FALSE(schema.addSharedComponent("label", {.defaultActions = {{ActionSlot::PrimaryClick, "shell.menu"}}}));
      CHECK(schema.components().empty());
    }
  }

  TEST_CASE("ActionSchema - capabilities are explicit bits", "[uimodel][unit][layout][schema]")
  {
    auto const action = ActionSchema{
      .id = "shell.menu",
      .label = "Menu",
      .category = "Shell",
      .capabilities = ActionCapability::RequiresAnchor | ActionCapability::PresentsMenu,
    };

    CHECK(action.supports(ActionCapability::RequiresAnchor));
    CHECK(action.supports(ActionCapability::PresentsMenu));
  }
} // namespace ao::uimodel::test
