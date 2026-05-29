// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/uimodel/layout/ActionCatalog.h>
#include <ao/uimodel/layout/ActionTypes.h>
#include <ao/uimodel/layout/ActionValidator.h>
#include <ao/uimodel/layout/ComponentCatalog.h>
#include <ao/uimodel/layout/LayoutDocument.h>
#include <ao/uimodel/layout/LayoutNode.h>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <optional>
#include <string>

namespace ao::uimodel::layout::test
{
  namespace
  {
    ActionCatalog makeTestCatalog()
    {
      auto catalog = ActionCatalog{};
      catalog.registerActionDescriptor(ActionDescriptor{
        .id = "valid.action", .label = "Valid Action", .category = "Test", .capabilities = ActionCapability::None});
      catalog.registerActionDescriptor(ActionDescriptor{.id = "needsAnchor",
                                                        .label = "Needs Anchor",
                                                        .category = "Test",
                                                        .capabilities = ActionCapability::RequiresAnchor});
      return catalog;
    }

    ComponentCatalog makeCompCatalog()
    {
      auto catalog = ComponentCatalog{};
      catalog.registerComponentDescriptor(
        ComponentDescriptor{.type = "test.button",
                            .displayName = "Test Button",
                            .category = "Test",
                            .props = {{.name = "primaryAction",
                                       .kind = PropertyKind::Enum,
                                       .label = "Primary Action",
                                       .optActionBinding = ActionBindingProperty{.slot = ActionSlot::PrimaryClick}}}});
      return catalog;
    }

    auto const permissiveResolver = [](LayoutNode const&,
                                       PropertyDescriptor const& prop) -> std::optional<ActionBindingContext>
    {
      if (!prop.optActionBinding)
      {
        return std::nullopt;
      }

      return ActionBindingContext{
        .slot = prop.optActionBinding->slot, .hasAnchor = true, .hasFocusedView = true, .componentType = "test.button"};
    };
  } // namespace

  TEST_CASE("ActionValidator", "[layout][unit][validator]")
  {
    auto const actions = makeTestCatalog();
    auto const components = makeCompCatalog();

    SECTION("valid document produces no diagnostics")
    {
      auto doc = LayoutDocument{};
      doc.root.type = "test.button";
      doc.root.props["primaryAction"] = LayoutValue{std::string{"valid.action"}};

      auto const diagnostics = validateActions(doc, components, actions, permissiveResolver);
      CHECK(diagnostics.empty());
    }

    SECTION("unknown action id produces diagnostic")
    {
      auto doc = LayoutDocument{};
      doc.root.type = "test.button";
      doc.root.props["primaryAction"] = LayoutValue{std::string{"unknown.action"}};

      auto const diagnostics = validateActions(doc, components, actions, permissiveResolver);
      REQUIRE(diagnostics.size() == 1);
      CHECK(diagnostics[0].actionId == "unknown.action");
      CHECK(diagnostics[0].propertyName == "primaryAction");
    }

    SECTION("none action id produces no diagnostic")
    {
      auto doc = LayoutDocument{};
      doc.root.type = "test.button";
      doc.root.props["primaryAction"] = LayoutValue{std::string{"none"}};

      auto const diagnostics = validateActions(doc, components, actions, permissiveResolver);
      CHECK(diagnostics.empty());
    }

    SECTION("empty action id produces no diagnostic")
    {
      auto doc = LayoutDocument{};
      doc.root.type = "test.button";
      doc.root.props["primaryAction"] = LayoutValue{std::string{""}};

      auto const diagnostics = validateActions(doc, components, actions, permissiveResolver);
      CHECK(diagnostics.empty());
    }

    SECTION("missing action prop produces no diagnostic")
    {
      auto doc = LayoutDocument{};
      doc.root.type = "test.button";

      auto const diagnostics = validateActions(doc, components, actions, permissiveResolver);
      CHECK(diagnostics.empty());
    }

    SECTION("non-string action value produces diagnostic")
    {
      auto doc = LayoutDocument{};
      doc.root.type = "test.button";
      doc.root.props["primaryAction"] = LayoutValue{static_cast<std::int64_t>(42)};

      auto const diagnostics = validateActions(doc, components, actions, permissiveResolver);
      REQUIRE(diagnostics.size() == 1);
      CHECK(diagnostics[0].actionId == "(invalid type)");
    }

    SECTION("incompatible anchor requirement produces diagnostic")
    {
      auto doc = LayoutDocument{};
      doc.root.type = "test.button";
      doc.root.props["primaryAction"] = LayoutValue{std::string{"needsAnchor"}};

      auto const noAnchorResolver = [](LayoutNode const&,
                                       PropertyDescriptor const& prop) -> std::optional<ActionBindingContext>
      {
        if (!prop.optActionBinding)
        {
          return std::nullopt;
        }

        return ActionBindingContext{.slot = prop.optActionBinding->slot,
                                    .hasAnchor = false,
                                    .hasFocusedView = false,
                                    .componentType = "test.button"};
      };

      auto const diagnostics = validateActions(doc, components, actions, noAnchorResolver);
      REQUIRE(diagnostics.size() == 1);
      CHECK(diagnostics[0].actionId == "needsAnchor");
    }

    SECTION("missing resolver returns no diagnostic (conservative)")
    {
      auto doc = LayoutDocument{};
      doc.root.type = "test.button";
      doc.root.props["primaryAction"] = LayoutValue{std::string{"needsAnchor"}};

      auto const nullResolver = [](LayoutNode const&, PropertyDescriptor const&) -> std::optional<ActionBindingContext>
      { return std::nullopt; };

      auto const diagnostics = validateActions(doc, components, actions, nullResolver);
      CHECK(diagnostics.empty());
    }
  }
}
