// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "layout/runtime/ActionValidator.h"

#include "layout/runtime/ActionRegistry.h"
#include "layout/runtime/ComponentRegistry.h"
#include <ao/uimodel/layout/action/LayoutActionTypes.h>
#include <ao/uimodel/layout/action/LayoutActionValidator.h>
#include <ao/uimodel/layout/component/LayoutComponentCatalog.h>
#include <ao/uimodel/layout/document/LayoutDocument.h>

#include <catch2/catch_test_macros.hpp>

namespace ao::gtk::layout::test
{
  using namespace uimodel;

  TEST_CASE("LayoutActionValidator validates component action bindings against the GTK action registry",
            "[gtk][unit][layout][runtime]")
  {
    auto registry = ActionRegistry{};
    auto compRegistry = ComponentRegistry{};

    compRegistry.registerComponent(
      {.type = "app.actionButton",
       .displayName = "Action Button",
       .category = LayoutComponentCategory::Generic,
       .props = {{.name = "primaryAction",
                  .kind = LayoutPropertyKind::Enum,
                  .label = "Primary Action",
                  .optActionBinding = LayoutActionBindingProperty{.slot = LayoutActionSlot::PrimaryClick}}}},
      nullptr);

    registry.registerAction(
      LayoutActionDescriptor{
        .id = "test.action", .label = "Test Action", .category = "Test", .capabilities = LayoutActionCapability::None},
      [](auto&) {});
    registry.registerAction(LayoutActionDescriptor{.id = "test.anchored",
                                                   .label = "Anchored",
                                                   .category = "Test",
                                                   .capabilities = LayoutActionCapability::RequiresAnchor},
                            [](auto&) {});

    SECTION("Validates good document")
    {
      auto doc = LayoutDocument{};
      doc.root.type = "app.actionButton";
      doc.root.props["primaryAction"] = LayoutValue{std::string{"test.action"}};

      auto diagnostics = uimodel::validateLayoutActions(
        doc, compRegistry.catalog(), registry.catalog(), resolveGtkLayoutActionBindingContext);
      CHECK(diagnostics.empty());
    }

    SECTION("Detects unknown action ID")
    {
      auto doc = LayoutDocument{};
      doc.root.type = "app.actionButton";
      doc.root.id = "my-btn";
      doc.root.props["primaryAction"] = LayoutValue{std::string{"unknown.id"}};

      auto diagnostics = uimodel::validateLayoutActions(
        doc, compRegistry.catalog(), registry.catalog(), resolveGtkLayoutActionBindingContext);
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

      auto diagnostics = uimodel::validateLayoutActions(
        doc, compRegistry.catalog(), registry.catalog(), resolveGtkLayoutActionBindingContext);
      CHECK(diagnostics.empty());
    }
  }
} // namespace ao::gtk::layout::test
