// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/winui/layout/LayoutDialect.h>

#include <ao/Error.h>
#include <ao/uimodel/layout/document/LayoutDialect.h>
#include <ao/uimodel/layout/document/LayoutDocument.h>
#include <ao/uimodel/layout/document/LayoutNode.h>
#include <ao/uimodel/layout/document/LayoutPreparation.h>
#include <ao/uimodel/layout/document/LayoutValidation.h>
#include <ao/winui/layout/LayoutSchema.h>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace ao::winui::test
{
  using uimodel::LayoutNode;
  using uimodel::LayoutValue;

  namespace
  {
    LayoutNode shellRoot(std::vector<LayoutNode> children)
    {
      return LayoutNode{.id = "windows-root",
                        .type = "box",
                        .props = {{"orientation", LayoutValue{std::string{"vertical"}}}},
                        .children = std::move(children)};
    }

    LayoutNode trackTable()
    {
      return LayoutNode{.id = "track-table", .type = "track.table"};
    }

    std::optional<uimodel::LayoutRejection> validate(LayoutNode root)
    {
      auto const document = uimodel::LayoutDocument{.root = std::move(root)};
      auto const preparedRes = uimodel::prepareLayout(document);
      REQUIRE(preparedRes.has_value());
      return uimodel::validateLayout(*preparedRes, layoutSchema(), layoutDialect());
    }

    uimodel::LayoutRejection rejectionOf(LayoutNode root)
    {
      auto optRejection = validate(std::move(root));
      REQUIRE(optRejection);
      return *optRejection;
    }
  } // namespace

  TEST_CASE("validateLayout under the Windows dialect - a document using only registered components is accepted",
            "[winui][unit][layout]")
  {
    auto root = shellRoot(
      {trackTable(),
       LayoutNode{.id = "status", .type = "windows.statusBar"},
       LayoutNode{.type = "playback.transportButton", .props = {{"command", LayoutValue{std::string{"stop"}}}}}});

    auto const optRejection = validate(std::move(root));
    CHECK_FALSE(optRejection);
  }

  TEST_CASE("validateLayout under the Windows dialect - an unregistered component type rejects the candidate",
            "[winui][unit][layout]")
  {
    auto const rejection = rejectionOf(shellRoot({LayoutNode{.id = "tabs", .type = "tabs"}}));

    CHECK(rejection.reason == uimodel::LayoutRejectionReason::UnknownComponentType);
    CHECK(rejection.nodeId == "tabs");
    CHECK(rejection.detail == "tabs");
  }

  TEST_CASE("validateLayout under the Windows dialect - a failed template expansion cannot reach a constructed element",
            "[winui][unit][layout]")
  {
    auto root = shellRoot({LayoutNode{
      .id = "missing-template", .type = "template", .props = {{"templateId", LayoutValue{std::string{"absent"}}}}}});

    auto const optRejection = validate(std::move(root));
    REQUIRE(optRejection);
    CHECK(optRejection->reason == uimodel::LayoutRejectionReason::UnknownComponentType);
  }

  TEST_CASE("validateLayout under the Windows dialect - the GTK styling field is rejected rather than ignored",
            "[winui][unit][layout]")
  {
    auto const rejection = rejectionOf(shellRoot(
      {LayoutNode{.id = "styled", .type = "label", .layout = {{"cssClasses", LayoutValue{std::string{"ao-pane"}}}}}}));

    CHECK(rejection.reason == uimodel::LayoutRejectionReason::UnsupportedLayoutField);
    CHECK(rejection.detail == "cssClasses");
  }

  TEST_CASE("validateLayout under the Windows dialect - a themed surface must name a slot the element can paint",
            "[winui][unit][layout]")
  {
    SECTION("a slot the shell does not paint")
    {
      auto const rejection = rejectionOf(shellRoot({LayoutNode{
        .id = "surfaced", .type = "box", .layout = {{"surface", LayoutValue{std::string{"modern.nowhere"}}}}}}));

      CHECK(rejection.reason == uimodel::LayoutRejectionReason::InvalidLayoutFieldValue);
      CHECK(rejection.detail == "surface");
    }

    SECTION("an element that owns no background")
    {
      auto const rejection = rejectionOf(shellRoot({LayoutNode{
        .id = "surfaced", .type = "label", .layout = {{"surface", LayoutValue{std::string{"modern.inspector"}}}}}}));

      CHECK(rejection.reason == uimodel::LayoutRejectionReason::UnsupportedLayoutField);
      CHECK(rejection.detail == "surface");
    }

    SECTION("an element that does own one")
    {
      CHECK_FALSE(validate(shellRoot({LayoutNode{
                             .id = "surfaced",
                             .type = "box",
                             .layout = {{"surface", LayoutValue{std::string{"classic.toolbar"}}}},
                           }}))
                    .has_value());
    }
  }

  TEST_CASE("validateLayout under the Windows dialect - unsupported and malformed layout fields reject the candidate",
            "[winui][unit][layout]")
  {
    SECTION("a field the component does not declare")
    {
      auto const rejection = rejectionOf(shellRoot(
        {LayoutNode{.id = "slotted", .type = "label", .layout = {{"slot", LayoutValue{std::string{"end"}}}}}}));

      CHECK(rejection.reason == uimodel::LayoutRejectionReason::UnsupportedLayoutField);
      CHECK(rejection.detail == "slot");
    }

    SECTION("an alignment outside the version 1 vocabulary")
    {
      auto const rejection = rejectionOf(shellRoot(
        {LayoutNode{.id = "aligned", .type = "label", .layout = {{"halign", LayoutValue{std::string{"trailing"}}}}}}));

      CHECK(rejection.reason == uimodel::LayoutRejectionReason::InvalidLayoutFieldValue);
      CHECK(rejection.detail == "halign");
    }

    SECTION("a non-numeric size request")
    {
      auto const rejection = rejectionOf(shellRoot(
        {LayoutNode{.id = "sized", .type = "label", .layout = {{"widthRequest", LayoutValue{std::string{"wide"}}}}}}));

      CHECK(rejection.reason == uimodel::LayoutRejectionReason::InvalidLayoutFieldValue);
      CHECK(rejection.detail == "widthRequest");
    }

    SECTION("an empty style key")
    {
      auto const rejection = rejectionOf(
        shellRoot({LayoutNode{.id = "styled", .type = "label", .layout = {{"styleKey", LayoutValue{std::string{}}}}}}));

      CHECK(rejection.reason == uimodel::LayoutRejectionReason::InvalidLayoutFieldValue);
      CHECK(rejection.detail == "styleKey");
    }
  }

  TEST_CASE("validateLayout under the Windows dialect - properties must exist and carry the declared kind",
            "[winui][unit][layout]")
  {
    SECTION("an undeclared property")
    {
      auto const rejection = rejectionOf(
        shellRoot({LayoutNode{.id = "spaced", .type = "label", .props = {{"padding", LayoutValue{std::int64_t{4}}}}}}));

      CHECK(rejection.reason == uimodel::LayoutRejectionReason::UnknownProperty);
      CHECK(rejection.detail == "padding");
    }

    SECTION("a property with the wrong value type")
    {
      auto const rejection = rejectionOf(
        shellRoot({LayoutNode{.id = "row", .type = "box", .props = {{"spacing", LayoutValue{std::string{"wide"}}}}}}));

      CHECK(rejection.reason == uimodel::LayoutRejectionReason::InvalidPropertyValue);
      CHECK(rejection.detail == "spacing");
    }

    SECTION("an enum value outside the declared list")
    {
      auto const rejection = rejectionOf(shellRoot(
        {LayoutNode{.id = "row", .type = "box", .props = {{"orientation", LayoutValue{std::string{"diagonal"}}}}}}));

      CHECK(rejection.reason == uimodel::LayoutRejectionReason::InvalidPropertyValue);
      CHECK(rejection.detail == "orientation");
    }
  }

  TEST_CASE("validateLayout under the Windows dialect - component cardinality is enforced in both directions",
            "[winui][unit][layout]")
  {
    SECTION("a split needs both of its children")
    {
      auto const rejection =
        rejectionOf(shellRoot({LayoutNode{.id = "workspace", .type = "split", .children = {trackTable()}}}));

      CHECK(rejection.reason == uimodel::LayoutRejectionReason::ChildCountBelowMinimum);
      CHECK(rejection.nodeId == "workspace");
    }

    SECTION("a leaf accepts no children")
    {
      auto const rejection =
        rejectionOf(shellRoot({LayoutNode{.id = "gap", .type = "label", .children = {LayoutNode{.type = "label"}}}}));

      CHECK(rejection.reason == uimodel::LayoutRejectionReason::ChildCountAboveMaximum);
      CHECK(rejection.nodeId == "gap");
    }
  }

  TEST_CASE("validateLayout under the Windows dialect - the navigation presentation fixes the pane child count",
            "[winui][unit][layout]")
  {
    SECTION("the navigation view presentation requires a content child")
    {
      auto const rejection =
        rejectionOf(shellRoot({LayoutNode{.id = "navigation",
                                          .type = "windows.navigationPane",
                                          .props = {{"presentation", LayoutValue{std::string{"navigationView"}}}}}}));

      CHECK(rejection.reason == uimodel::LayoutRejectionReason::ChildCountBelowMinimum);
      CHECK(rejection.nodeId == "navigation");
    }

    SECTION("the tree presentation is a sibling column and hosts nothing")
    {
      auto const rejection =
        rejectionOf(shellRoot({LayoutNode{.id = "navigation",
                                          .type = "windows.navigationPane",
                                          .props = {{"presentation", LayoutValue{std::string{"tree"}}}},
                                          .children = {trackTable()}}}));

      CHECK(rejection.reason == uimodel::LayoutRejectionReason::ChildCountAboveMaximum);
      CHECK(rejection.nodeId == "navigation");
    }

    SECTION("both presentations are accepted when their child count matches")
    {
      auto root = shellRoot({LayoutNode{.id = "navigation",
                                        .type = "windows.navigationPane",
                                        .props = {{"presentation", LayoutValue{std::string{"navigationView"}}}},
                                        .children = {trackTable()}}});

      auto const optRejection = validate(std::move(root));
      CHECK_FALSE(optRejection);
    }
  }

  TEST_CASE("validateLayout under the Windows dialect - reconciled components need stable unique ids",
            "[winui][unit][layout]")
  {
    SECTION("an anonymous track table cannot be located after a rebuild")
    {
      auto const rejection = rejectionOf(shellRoot({LayoutNode{.type = "track.table"}}));

      CHECK(rejection.reason == uimodel::LayoutRejectionReason::MissingRequiredId);
      CHECK(rejection.detail == "id");
    }

    SECTION("a repeated id makes reconciliation ambiguous")
    {
      auto const rejection = rejectionOf(shellRoot({trackTable(), LayoutNode{.id = "track-table", .type = "label"}}));

      CHECK(rejection.reason == uimodel::LayoutRejectionReason::DuplicateNodeId);
      CHECK(rejection.detail == "track-table");
    }

    SECTION("an anonymous structural node is fine")
    {
      CHECK_FALSE(validate(shellRoot({LayoutNode{.type = "label"}, LayoutNode{.type = "label"}})).has_value());
    }
  }

  TEST_CASE("validateLayout under the Windows dialect - action bindings honor the policy and the schema",
            "[winui][unit][layout]")
  {
    SECTION("a component without an action policy rejects a bound slot")
    {
      auto const rejection = rejectionOf(shellRoot({LayoutNode{
        .id = "caption", .type = "label", .props = {{"primaryAction", LayoutValue{std::string{"library.open"}}}}}}));

      CHECK(rejection.reason == uimodel::LayoutRejectionReason::UnsupportedActionSlot);
      CHECK(rejection.detail == "primaryAction");
    }

    SECTION("an unregistered action id rejects the candidate")
    {
      auto const rejection =
        rejectionOf(shellRoot({LayoutNode{.id = "command",
                                          .type = "actionButton",
                                          .props = {{"primaryAction", LayoutValue{std::string{"library.destroy"}}}}}}));

      CHECK(rejection.reason == uimodel::LayoutRejectionReason::UnknownAction);
      CHECK(rejection.detail == "library.destroy");
    }

    SECTION("an explicitly unbound slot is accepted")
    {
      auto root = shellRoot({LayoutNode{
        .id = "command", .type = "actionButton", .props = {{"primaryAction", LayoutValue{std::string{"none"}}}}}});

      auto const optRejection = validate(std::move(root));
      CHECK_FALSE(optRejection);
    }
  }

  TEST_CASE("validateLayout under the Windows dialect - an authored tooltip subtree has no Windows surface",
            "[winui][unit][layout]")
  {
    auto tooltipped = LayoutNode{.id = "soul", .type = "playback.soulButton"};
    tooltipped.optTooltip = uimodel::BoxedLayoutNode{LayoutNode{.type = "label"}};

    auto const rejection = rejectionOf(shellRoot({std::move(tooltipped)}));

    CHECK(rejection.reason == uimodel::LayoutRejectionReason::UnsupportedSurface);
    CHECK(rejection.detail == "tooltip");
  }

  TEST_CASE("requireValidLayout under the Windows dialect - a rejection becomes the candidate's reported error",
            "[winui][unit][layout]")
  {
    auto const document = uimodel::LayoutDocument{.root = shellRoot({LayoutNode{.id = "tabs", .type = "tabs"}})};
    auto const preparedRes = uimodel::prepareLayout(document);
    REQUIRE(preparedRes.has_value());

    auto const validatedRes = uimodel::requireValidLayout(*preparedRes, layoutSchema(), layoutDialect());

    REQUIRE_FALSE(validatedRes.has_value());
    CHECK(validatedRes.error().code == Error::Code::FormatRejected);
    CHECK(validatedRes.error().message.contains("tabs"));
    CHECK(validatedRes.error().message.contains("unknown component type"));
  }
} // namespace ao::winui::test
