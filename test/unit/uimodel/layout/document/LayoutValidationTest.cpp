// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/uimodel/layout/document/LayoutValidation.h>

#include <ao/uimodel/layout/action/LayoutActionCatalog.h>
#include <ao/uimodel/layout/component/LayoutComponentCatalog.h>
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
     * @brief A catalog with no frontend behind it.
     *
     * The shared traversal is supposed to work off the catalog and the dialect
     * alone. Testing it against an invented vocabulary is what shows that: a
     * rule that only holds for the real Windows catalog would not survive here.
     */
    LayoutComponentCatalog fakeCatalog()
    {
      auto catalog = LayoutComponentCatalog{};
      catalog.registerComponentDescriptor(LayoutComponentDescriptor{
        .type = "frame",
        .displayName = "Frame",
        .category = LayoutComponentCategory::Container,
        .props = {LayoutPropertyDescriptor{.name = "mode", .kind = LayoutPropertyKind::String, .label = "Mode"}},
        .layoutProps = {LayoutPropertyDescriptor{
          .name = "weight", .kind = LayoutPropertyKind::Double, .label = "Weight"}},
        .minChildren = 1,
        .optMaxChildren = 2,
      });
      catalog.registerComponentDescriptor(LayoutComponentDescriptor{
        .type = "readout",
        .displayName = "Readout",
        .category = LayoutComponentCategory::Status,
        .props = {LayoutPropertyDescriptor{.name = "caption", .kind = LayoutPropertyKind::String, .label = "Caption"}},
        .surfaces = static_cast<LayoutSurfaceCapabilityMask>(LayoutSurfaceCapability::Main) |
                    static_cast<LayoutSurfaceCapabilityMask>(LayoutSurfaceCapability::Tooltip),
      });
      return catalog;
    }

    LayoutActionCatalog fakeActions()
    {
      return {};
    }

    /// A field only this dialect knows, so a shared rule cannot be what accepts it.
    constexpr auto kPaintProp = std::string_view{"paint"};
    /// A field this dialect refuses even though the descriptor never mentions it.
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
      return validateLayout(*preparedRes, fakeCatalog(), fakeActions(), dialect);
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
    CHECK(describeLayoutRejection(fakeDialect(), unknown).contains("Fake catalog"));
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

  TEST_CASE("validateLayout - a dialect rules on a layout field before the descriptor does",
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

    // A field the descriptor never declares is rejected as the dialect's defect,
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
