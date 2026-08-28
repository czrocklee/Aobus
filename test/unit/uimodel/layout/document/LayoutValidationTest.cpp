// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/uimodel/layout/document/LayoutValidation.h>

#include <ao/uimodel/layout/component/LayoutSchema.h>
#include <ao/uimodel/layout/component/LayoutSurface.h>
#include <ao/uimodel/layout/document/LayoutDialect.h>
#include <ao/uimodel/layout/document/LayoutDocument.h>
#include <ao/uimodel/layout/document/LayoutNode.h>
#include <ao/uimodel/layout/document/LayoutPreparation.h>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace ao::uimodel::test
{
  namespace
  {
    /**
     * @brief A schema with no frontend behind it.
     *
     * The shared traversal is supposed to work off the schema and the dialect
     * alone. Testing it against an invented vocabulary is what shows that: a
     * rule that only holds for the real Windows schema would not survive here.
     */
    LayoutSchema fakeSchema()
    {
      auto schema = LayoutSchema{};
      schema.addComponent(ComponentSchema{
        .id = "frame",
        .displayName = "Frame",
        .category = ComponentCategory::Container,
        .properties = {PropertySchema{
          .name = "mode", .kind = PropertyKind::Enum, .label = "Mode", .enumValues = {"row", "leaf"}}},
        .layoutProperties = {PropertySchema{.name = "weight", .kind = PropertyKind::Double, .label = "Weight"}},
        .minChildren = 1,
        .optMaxChildren = 2,
      });
      schema.addComponent(ComponentSchema{
        .id = "readout",
        .displayName = "Readout",
        .category = ComponentCategory::Status,
        .properties = {PropertySchema{.name = "caption", .kind = PropertyKind::String, .label = "Caption"}},
        .surfaces = static_cast<LayoutSurfaceCapabilityMask>(LayoutSurfaceCapability::Main) |
                    static_cast<LayoutSurfaceCapabilityMask>(LayoutSurfaceCapability::Tooltip),
      });
      schema.addComponent(ComponentSchema{
        .id = "trigger",
        .displayName = "Trigger",
        .category = ComponentCategory::Generic,
        .optMaxChildren = 0,
        .actionSlots = actionSlotBit(ActionSlot::PrimaryClick),
        .defaultActions = {{ActionSlot::PrimaryClick, "valid.action"}},
      });
      schema.addAction({.id = "valid.action", .label = "Valid", .category = "Test"});
      return schema;
    }

    /// A field only this dialect knows, so a shared rule cannot be what accepts it.
    constexpr auto kPaintProp = std::string_view{"paint"};
    /// A field this dialect refuses even though the schema entry never mentions it.
    constexpr auto kRivalProp = std::string_view{"rivalStyling"};

    LayoutFieldVerdict fakeLayoutField(LayoutNode const& /*node*/,
                                       std::string_view const name,
                                       LayoutValue const& value)
    {
      if (name == kRivalProp)
      {
        return LayoutFieldVerdict::rejected(
          LayoutRejectionReason::UnsupportedLayoutField, "this shell paints through its own vocabulary");
      }

      if (name == kPaintProp)
      {
        if (auto const* const spelling = value.getIf<std::string>(); spelling == nullptr || spelling->empty())
        {
          return LayoutFieldVerdict::rejected(
            LayoutRejectionReason::InvalidLayoutFieldValue, "paint must name a non-empty colour");
        }

        return LayoutFieldVerdict::accepted();
      }

      return LayoutFieldVerdict::unclaimed();
    }

    std::optional<std::size_t> fakePresentationChildCount(LayoutNode const& node)
    {
      return node.propertyOr<std::string>("mode", "") == "leaf" ? std::optional<std::size_t>{0} : std::nullopt;
    }

    bool fakeRequiresStableId(std::string_view const type)
    {
      return type == "readout";
    }

    LayoutDialect fakeDialect()
    {
      return LayoutDialect{
        .name = "Fake",
        .layoutField = &fakeLayoutField,
        .presentationChildCount = &fakePresentationChildCount,
        .requiresStableId = &fakeRequiresStableId,
        .authorsTooltips = false,
      };
    }

    LayoutNode readout(std::string id)
    {
      return LayoutNode{.id = std::move(id), .type = "readout"};
    }

    std::optional<LayoutRejection> validate(LayoutNode root, LayoutDialect const& dialect)
    {
      auto const document = LayoutDocument{.root = std::move(root)};
      auto const preparedRes = prepareLayout(document);
      REQUIRE(preparedRes.has_value());
      return validateLayout(*preparedRes, fakeSchema(), dialect);
    }

    LayoutRejection rejectionOf(LayoutNode root, LayoutDialect const& dialect)
    {
      auto optRejection = validate(std::move(root), dialect);
      REQUIRE(optRejection);
      return *optRejection;
    }
  } // namespace

  TEST_CASE("validateLayout - the shared rules hold for a vocabulary no frontend ships",
            "[uimodel][unit][layout][validation]")
  {
    auto const optAccepted = validate(LayoutNode{.type = "frame", .children = {readout("a")}}, fakeDialect());
    CHECK_FALSE(optAccepted);

    auto const unknown = rejectionOf(LayoutNode{.type = "nosuchtype"}, fakeDialect());
    CHECK(unknown.reason == LayoutRejectionReason::UnknownComponentType);
    // The rejection names the dialect rather than any one frontend.
    CHECK(describeLayoutRejection(fakeDialect(), unknown).contains("Fake schema"));
  }

  TEST_CASE("validateLayout - component child bounds hold without a dialect override",
            "[uimodel][unit][layout][validation]")
  {
    auto const bare = LayoutDialect{.name = "Bare"};

    auto const belowMinimum = rejectionOf(LayoutNode{.type = "frame"}, bare);
    CHECK(belowMinimum.reason == LayoutRejectionReason::ChildCountBelowMinimum);

    auto const aboveMaximum =
      rejectionOf(LayoutNode{.type = "frame", .children = {readout("a"), readout("b"), readout("c")}}, bare);
    CHECK(aboveMaximum.reason == LayoutRejectionReason::ChildCountAboveMaximum);
  }

  TEST_CASE("validateLayout - enum properties reject values outside their schema",
            "[uimodel][unit][layout][validation]")
  {
    auto const invalid = rejectionOf(
      LayoutNode{
        .type = "frame", .props = {{"mode", LayoutValue{std::string{"super-large"}}}}, .children = {readout("a")}},
      LayoutDialect{.name = "Bare"});

    CHECK(invalid.reason == LayoutRejectionReason::InvalidPropertyValue);
    CHECK(invalid.detail == "mode");
  }

  TEST_CASE("validateLayout - action bindings use the document validator's single error channel",
            "[uimodel][unit][layout][validation]")
  {
    auto node = LayoutNode{.type = "trigger"};
    node.props[std::string{kPrimaryActionProp}] = LayoutValue{std::string{"valid.action"}};
    CHECK_FALSE(validate(node, LayoutDialect{.name = "Bare"}));

    node.props[std::string{kPrimaryActionProp}] = LayoutValue{std::string{"missing.action"}};
    CHECK(rejectionOf(node, LayoutDialect{.name = "Bare"}).reason == LayoutRejectionReason::UnknownAction);

    node.props[std::string{kPrimaryActionProp}] = LayoutValue{std::int64_t{42}};
    CHECK(rejectionOf(node, LayoutDialect{.name = "Bare"}).reason == LayoutRejectionReason::InvalidPropertyValue);

    node.props.erase(std::string{kPrimaryActionProp});
    node.props[std::string{kSecondaryActionProp}] = LayoutValue{std::string{"valid.action"}};
    CHECK(rejectionOf(node, LayoutDialect{.name = "Bare"}).reason == LayoutRejectionReason::UnsupportedActionSlot);
  }

  TEST_CASE("validateLayout - component defaults must resolve to a registered action",
            "[uimodel][unit][layout][validation]")
  {
    auto schema = fakeSchema();
    REQUIRE(schema.addComponent(ComponentSchema{
      .id = "bad-default",
      .displayName = "Bad Default",
      .optMaxChildren = 0,
      .actionSlots = actionSlotBit(ActionSlot::PrimaryClick),
      .defaultActions = {{ActionSlot::PrimaryClick, "missing.action"}},
    }));
    auto const preparedRes = prepareLayout(LayoutDocument{.root = LayoutNode{.type = "bad-default"}});
    REQUIRE(preparedRes);

    auto const optRejection = validateLayout(*preparedRes, schema, LayoutDialect{.name = "Bare"});
    REQUIRE(optRejection);
    CHECK(optRejection->reason == LayoutRejectionReason::UnknownAction);
  }

  TEST_CASE("validateLayout - a parent layout field is accepted on its direct child",
            "[uimodel][unit][layout][validation]")
  {
    auto child = readout("child");
    child.layout["weight"] = LayoutValue{1.5};

    auto const optAccepted = validate(LayoutNode{.type = "frame", .children = {std::move(child)}}, fakeDialect());
    CHECK_FALSE(optAccepted);

    auto invalidChild = readout("child");
    invalidChild.layout["weight"] = LayoutValue{true};

    auto const rejection =
      rejectionOf(LayoutNode{.type = "frame", .children = {std::move(invalidChild)}}, fakeDialect());
    CHECK(rejection.reason == LayoutRejectionReason::InvalidLayoutFieldValue);
    CHECK(rejection.nodeId == "child");
    CHECK(rejection.detail == "weight");
  }

  TEST_CASE("validateLayout - a dialect rules on a layout field before the schema entry does",
            "[uimodel][unit][layout][validation]")
  {
    auto const optClaimed = validate(LayoutNode{.type = "frame",
                                                .layout = {{std::string{kPaintProp}, LayoutValue{std::string{"teal"}}}},
                                                .children = {readout("a")}},
                                     fakeDialect());
    CHECK_FALSE(optClaimed);

    auto const malformed = rejectionOf(
      LayoutNode{.type = "frame", .layout = {{std::string{kPaintProp}, LayoutValue{true}}}, .children = {readout("a")}},
      fakeDialect());
    CHECK(malformed.reason == LayoutRejectionReason::InvalidLayoutFieldValue);

    // A field the schema entry never declares is rejected as the dialect's defect,
    // not as an unsupported field, which is what running first buys.
    auto const rival = rejectionOf(LayoutNode{.type = "frame",
                                              .layout = {{std::string{kRivalProp}, LayoutValue{std::string{"x"}}}},
                                              .children = {readout("a")}},
                                   fakeDialect());
    CHECK(rival.reason == LayoutRejectionReason::UnsupportedLayoutField);
    CHECK(rival.message.contains("its own vocabulary"));
  }

  TEST_CASE("validateLayout - the common layout fields are shared and need no dialect",
            "[uimodel][unit][layout][validation]")
  {
    auto const bare = LayoutDialect{.name = "Bare"};

    auto const optAccepted = validate(LayoutNode{.type = "frame",
                                                 .layout = {{"hexpand", LayoutValue{true}},
                                                            {"halign", LayoutValue{std::string{"center"}}},
                                                            {"widthRequest", LayoutValue{std::int64_t{80}}},
                                                            {"visible", LayoutValue{false}}},
                                                 .children = {readout("a")}},
                                      bare);
    CHECK_FALSE(optAccepted);

    auto const badAlign = rejectionOf(
      LayoutNode{
        .type = "frame", .layout = {{"halign", LayoutValue{std::string{"stretch"}}}}, .children = {readout("a")}},
      bare);
    CHECK(badAlign.reason == LayoutRejectionReason::InvalidLayoutFieldValue);

    auto const badSize = rejectionOf(
      LayoutNode{
        .type = "frame", .layout = {{"widthRequest", LayoutValue{std::string{"wide"}}}}, .children = {readout("a")}},
      bare);
    CHECK(badSize.reason == LayoutRejectionReason::InvalidLayoutFieldValue);

    auto const nonFiniteSize =
      rejectionOf(LayoutNode{.type = "frame",
                             .layout = {{"widthRequest", LayoutValue{std::numeric_limits<double>::infinity()}}},
                             .children = {readout("a")}},
                  bare);
    CHECK(nonFiniteSize.reason == LayoutRejectionReason::InvalidLayoutFieldValue);

    auto const oversized = rejectionOf(
      LayoutNode{
        .type = "frame",
        .layout = {{"heightRequest", LayoutValue{static_cast<double>(std::numeric_limits<std::int32_t>::max()) + 1.0}}},
        .children = {readout("a")}},
      bare);
    CHECK(oversized.reason == LayoutRejectionReason::InvalidLayoutFieldValue);
  }

  TEST_CASE("validateLayout - an absent hook is no opinion rather than a refusal",
            "[uimodel][unit][layout][validation]")
  {
    auto const bare = LayoutDialect{.name = "Bare"};

    // fakeRequiresStableId demands one; a dialect that names no hook does not.
    auto const anonymous = LayoutNode{.type = "frame", .children = {LayoutNode{.type = "readout"}}};
    CHECK_FALSE(validate(anonymous, bare).has_value());
    CHECK(rejectionOf(anonymous, fakeDialect()).reason == LayoutRejectionReason::MissingRequiredId);

    // Likewise for a presentation that fixes its own child count.
    auto const leaf =
      LayoutNode{.type = "frame", .props = {{"mode", LayoutValue{std::string{"leaf"}}}}, .children = {readout("a")}};
    CHECK(rejectionOf(leaf, fakeDialect()).reason == LayoutRejectionReason::ChildCountAboveMaximum);
  }

  TEST_CASE("validateLayout - a dialect that authors tooltips accepts the subtree the others reject",
            "[uimodel][unit][layout][validation]")
  {
    auto tooltipped = LayoutNode{.type = "frame", .children = {readout("a")}};
    tooltipped.optTooltip = BoxedLayoutNode{readout("tip")};

    CHECK(rejectionOf(tooltipped, fakeDialect()).reason == LayoutRejectionReason::UnsupportedSurface);

    auto withTooltips = fakeDialect();
    withTooltips.authorsTooltips = true;
    CHECK_FALSE(validate(tooltipped, withTooltips).has_value());

    auto unsupported = LayoutNode{.type = "frame", .children = {readout("a")}};
    unsupported.optTooltip = BoxedLayoutNode{LayoutNode{.type = "frame", .children = {readout("tip")}}};
    CHECK(rejectionOf(unsupported, withTooltips).reason == LayoutRejectionReason::UnsupportedSurface);
  }

  TEST_CASE("validateLayout - node ids stay unique across the whole document", "[uimodel][unit][layout][validation]")
  {
    auto const duplicated =
      rejectionOf(LayoutNode{.type = "frame", .children = {readout("same"), readout("same")}}, fakeDialect());

    CHECK(duplicated.reason == LayoutRejectionReason::DuplicateNodeId);
    CHECK(duplicated.nodeId == "same");
  }
} // namespace ao::uimodel::test
