// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "layout/runtime/ActionValidator.h"

#include "layout/document/LayoutDocument.h"
#include "layout/runtime/ActionRegistry.h"
#include "layout/runtime/ComponentRegistry.h"

#include <catch2/catch_test_macros.hpp>

namespace ao::gtk::layout::test
{
  TEST_CASE("ActionValidator", "[layout][unit][runtime]")
  {
    auto registry = ActionRegistry{};
    auto compRegistry = ComponentRegistry{};

    compRegistry.registerComponent(
      {.type = "app.actionButton",
       .displayName = "Action Button",
       .category = "Generic",
       .props = {{.name = "primaryAction",
                  .kind = PropertyKind::Enum,
                  .label = "Primary Action",
                  .optActionBinding = ActionBindingProperty{.slot = ActionSlot::PrimaryClick}}}},
      nullptr);

    registry.registerAction(
      ActionDescriptor{
        .id = "test.action", .label = "Test Action", .category = "Test", .capabilities = ActionCapability::None},
      [](auto&) {});
    registry.registerAction(ActionDescriptor{.id = "test.anchored",
                                             .label = "Anchored",
                                             .category = "Test",
                                             .capabilities = ActionCapability::RequiresAnchor},
                            [](auto&) {});

    SECTION("Validates good document")
    {
      auto doc = LayoutDocument{};
      doc.root.type = "app.actionButton";
      doc.root.props["primaryAction"] = LayoutValue{std::string{"test.action"}};

      auto diagnostics =
        validateActions(doc, compRegistry.catalog(), registry.catalog(), resolveGtkLayoutActionBindingContext);
      CHECK(diagnostics.empty());
    }

    SECTION("Detects unknown action ID")
    {
      auto doc = LayoutDocument{};
      doc.root.type = "app.actionButton";
      doc.root.id = "my-btn";
      doc.root.props["primaryAction"] = LayoutValue{std::string{"unknown.id"}};

      auto diagnostics =
        validateActions(doc, compRegistry.catalog(), registry.catalog(), resolveGtkLayoutActionBindingContext);
      REQUIRE(diagnostics.size() == 1);
      CHECK(diagnostics[0].componentId == "my-btn");
      CHECK(diagnostics[0].propertyName == "primaryAction");
      CHECK(diagnostics[0].actionId == "unknown.id");
    }

    SECTION("Tolerates 'none' action ID")
    {
      auto doc = LayoutDocument{};
      doc.root.type = "app.actionButton";
      doc.root.props["primaryAction"] = LayoutValue{std::string{"none"}};

      auto diagnostics =
        validateActions(doc, compRegistry.catalog(), registry.catalog(), resolveGtkLayoutActionBindingContext);
      CHECK(diagnostics.empty());
    }
  }
}
