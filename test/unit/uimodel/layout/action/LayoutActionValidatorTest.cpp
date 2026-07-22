// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/uimodel/layout/action/LayoutActionBinding.h>
#include <ao/uimodel/layout/action/LayoutActionCapabilities.h>
#include <ao/uimodel/layout/action/LayoutActionCatalog.h>
#include <ao/uimodel/layout/action/LayoutActionDescriptor.h>
#include <ao/uimodel/layout/action/LayoutActionSlot.h>
#include <ao/uimodel/layout/action/LayoutActionValidator.h>
#include <ao/uimodel/layout/component/LayoutComponentCatalog.h>
#include <ao/uimodel/layout/document/LayoutDocument.h>
#include <ao/uimodel/layout/document/LayoutNode.h>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string>

namespace ao::uimodel::test
{
  namespace
  {
    LayoutActionCatalog makeTestCatalog()
    {
      auto catalog = LayoutActionCatalog{};
      catalog.registerActionDescriptor(LayoutActionDescriptor{.id = "valid.action",
                                                              .label = "Valid Action",
                                                              .category = "Test",
                                                              .capabilities = LayoutActionCapability::None});
      return catalog;
    }

    LayoutComponentCatalog makeCompCatalog()
    {
      auto catalog = LayoutComponentCatalog{};
      catalog.registerComponentDescriptor(LayoutComponentDescriptor{
        .type = "test.button",
        .displayName = "Test Button",
        .category = LayoutComponentCategory::Generic,
        .props = {{.name = "primaryAction",
                   .kind = LayoutPropertyKind::Enum,
                   .label = "Primary Action",
                   .optActionBinding = LayoutActionBindingProperty{.slot = LayoutActionSlot::PrimaryClick}}}});
      return catalog;
    }
  } // namespace

  TEST_CASE("LayoutActionValidator - reports invalid action bindings", "[uimodel][unit][layout][action]")
  {
    auto const actions = makeTestCatalog();
    auto const components = makeCompCatalog();

    SECTION("valid document produces no diagnostics")
    {
      auto doc = LayoutDocument{};
      doc.root.type = "test.button";
      doc.root.props["primaryAction"] = LayoutValue{std::string{"valid.action"}};

      auto const diagnostics = validateLayoutActions(doc, components, actions);
      CHECK(diagnostics.empty());
    }

    SECTION("unknown action id produces diagnostic")
    {
      auto doc = LayoutDocument{};
      doc.root.type = "test.button";
      doc.root.id = "unknown-button";
      doc.root.props["primaryAction"] = LayoutValue{std::string{"unknown.action"}};

      auto const diagnostics = validateLayoutActions(doc, components, actions);
      REQUIRE(diagnostics.size() == 1);
      CHECK(diagnostics[0].componentId == "unknown-button");
      CHECK(diagnostics[0].actionId == "unknown.action");
      CHECK(diagnostics[0].propertyName == "primaryAction");
      CHECK(diagnostics[0].message == "Unknown action ID: unknown.action");
    }

    SECTION("none action id produces no diagnostic")
    {
      auto doc = LayoutDocument{};
      doc.root.type = "test.button";
      doc.root.props["primaryAction"] = LayoutValue{std::string{"none"}};

      auto const diagnostics = validateLayoutActions(doc, components, actions);
      CHECK(diagnostics.empty());
    }

    SECTION("empty action id produces no diagnostic")
    {
      auto doc = LayoutDocument{};
      doc.root.type = "test.button";
      doc.root.props["primaryAction"] = LayoutValue{std::string{""}};

      auto const diagnostics = validateLayoutActions(doc, components, actions);
      CHECK(diagnostics.empty());
    }

    SECTION("missing action prop produces no diagnostic")
    {
      auto doc = LayoutDocument{};
      doc.root.type = "test.button";

      auto const diagnostics = validateLayoutActions(doc, components, actions);
      CHECK(diagnostics.empty());
    }

    SECTION("non-string action value produces diagnostic")
    {
      auto doc = LayoutDocument{};
      doc.root.type = "test.button";
      doc.root.id = "non-string-button";
      doc.root.props["primaryAction"] = LayoutValue{static_cast<std::int64_t>(42)};

      auto const diagnostics = validateLayoutActions(doc, components, actions);
      REQUIRE(diagnostics.size() == 1);
      CHECK(diagnostics[0].componentId == "non-string-button");
      CHECK(diagnostics[0].propertyName == "primaryAction");
      CHECK(diagnostics[0].actionId == "(invalid type)");
      CHECK(diagnostics[0].message == "Action ID must be a string");
    }

    SECTION("detects unsupported action slots (disallowed by policy)")
    {
      auto doc = LayoutDocument{};
      doc.root.type = "test.button";
      doc.root.id = "my-btn";
      // test.button only allows PrimaryClick in makeCompCatalog (via explicit props for now)
      // Actually makeCompCatalog doesn't set actionPolicy, so it defaults to kNoExternalActions.
      // But it has an explicit primaryAction prop.
      doc.root.props["secondaryAction"] = LayoutValue{std::string{"valid.action"}};

      auto const diagnostics = validateLayoutActions(doc, components, actions);
      REQUIRE(diagnostics.size() == 1);
      CHECK(diagnostics[0].componentId == "my-btn");
      CHECK(diagnostics[0].propertyName == "secondaryAction");
      CHECK(diagnostics[0].actionId.empty());
      CHECK(diagnostics[0].message == "Action slot is not supported by this component policy");
    }
  }
} // namespace ao::uimodel::test
