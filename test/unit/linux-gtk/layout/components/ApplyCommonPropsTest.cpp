// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "app/linux-gtk/layout/runtime/ComponentRegistry.h"
#include "app/linux-gtk/layout/runtime/LayoutComponent.h"
#include "app/linux-gtk/layout/runtime/LayoutRuntime.h"
#include "test/unit/linux-gtk/layout/LayoutTestSupport.h"
#include <ao/uimodel/layout/document/LayoutDocument.h>
#include <ao/uimodel/layout/document/LayoutNode.h>
#include <ao/uimodel/layout/document/LayoutPreparation.h>

#include <catch2/catch_test_macros.hpp>
#include <gtkmm/box.h>
#include <gtkmm/enums.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace ao::gtk::layout::test
{
  using namespace uimodel;

  namespace
  {
    constexpr std::int32_t kComponentWidthRequest = 73;
    constexpr std::int32_t kComponentHeightRequest = 91;

    class RequestedSizeComponent final : public LayoutComponent
    {
    public:
      RequestedSizeComponent() { _widget.set_size_request(kComponentWidthRequest, kComponentHeightRequest); }

      Gtk::Widget& widget() override { return _widget; }

    private:
      Gtk::Box _widget{};
    };

    std::unique_ptr<LayoutComponent> makeRequestedSizeComponent(LayoutBuildContext& /*context*/,
                                                                LayoutNode const& /*node*/)
    {
      return std::make_unique<RequestedSizeComponent>();
    }

    void registerRequestedSizeComponent(LayoutRuntimeFixture& fixture)
    {
      fixture.components().registerComponent(
        {.id = "test.requestedSize", .displayName = "Requested Size"}, makeRequestedSizeComponent);
    }
  } // namespace

  TEST_CASE("applyCommonProps applies GTK widget layout properties", "[gtk][unit][layout][container]")
  {
    auto fixture = LayoutRuntimeFixture{};
    auto& ctx = fixture.context();
    auto& layoutRuntime = fixture.layoutRuntime();

    SECTION("hexpand/vexpand applied to child")
    {
      auto doc = LayoutDocument{};
      doc.root.type = "box";

      auto child = LayoutNode{};
      child.type = "spacer";
      child.layout["hexpand"] = LayoutValue{true};
      child.layout["vexpand"] = LayoutValue{false};
      doc.root.children.push_back(child);

      auto const compPtr = layoutRuntime.build(ctx, preparedLayout(doc));
      auto* const box = dynamic_cast<Gtk::Box*>(&compPtr->widget());

      REQUIRE(box != nullptr);

      auto* const spacer = box->get_first_child();
      REQUIRE(spacer != nullptr);
      CHECK(spacer->get_hexpand() == true);
      CHECK(spacer->get_vexpand() == false);
    }

    SECTION("halign/valign applied to child")
    {
      auto doc = LayoutDocument{};
      doc.root.type = "box";

      auto child = LayoutNode{};
      child.type = "spacer";
      child.layout["halign"] = LayoutValue{std::string{"center"}};
      child.layout["valign"] = LayoutValue{std::string{"end"}};
      doc.root.children.push_back(child);

      auto const compPtr = layoutRuntime.build(ctx, preparedLayout(doc));
      auto* const box = dynamic_cast<Gtk::Box*>(&compPtr->widget());

      REQUIRE(box != nullptr);

      auto* const spacer = box->get_first_child();
      REQUIRE(spacer != nullptr);
      CHECK(spacer->get_halign() == Gtk::Align::CENTER);
      CHECK(spacer->get_valign() == Gtk::Align::END);
    }

    SECTION("visible=false hides child")
    {
      auto doc = LayoutDocument{};
      doc.root.type = "box";

      auto child = LayoutNode{};
      child.type = "spacer";
      child.layout["visible"] = LayoutValue{false};
      doc.root.children.push_back(child);

      auto const compPtr = layoutRuntime.build(ctx, preparedLayout(doc));
      auto* const box = dynamic_cast<Gtk::Box*>(&compPtr->widget());

      REQUIRE(box != nullptr);

      auto* const spacer = box->get_first_child();
      REQUIRE(spacer != nullptr);
      CHECK(spacer->get_visible() == false);
    }

    SECTION("cssClasses applied from layout")
    {
      auto doc = LayoutDocument{};
      doc.root.type = "box";

      auto child = LayoutNode{};
      child.type = "spacer";
      child.layout["cssClasses"] = LayoutValue{std::vector<std::string>{"my-class", "another"}};
      doc.root.children.push_back(child);

      auto const compPtr = layoutRuntime.build(ctx, preparedLayout(doc));
      auto* const box = dynamic_cast<Gtk::Box*>(&compPtr->widget());

      REQUIRE(box != nullptr);

      auto* const spacer = box->get_first_child();
      REQUIRE(spacer != nullptr);
      CHECK(spacer->has_css_class("my-class"));
      CHECK(spacer->has_css_class("another"));
    }

    SECTION("widthRequest/heightRequest set size request")
    {
      auto doc = LayoutDocument{};
      doc.root.type = "box";

      auto child = LayoutNode{};
      child.type = "spacer";
      child.layout["widthRequest"] = LayoutValue{static_cast<std::int64_t>(200)};
      child.layout["heightRequest"] = LayoutValue{static_cast<std::int64_t>(100)};
      doc.root.children.push_back(child);

      auto const compPtr = layoutRuntime.build(ctx, preparedLayout(doc));
      auto* const box = dynamic_cast<Gtk::Box*>(&compPtr->widget());

      REQUIRE(box != nullptr);

      auto* const spacer = box->get_first_child();
      REQUIRE(spacer != nullptr);

      std::int32_t width = -1;
      std::int32_t height = -1;
      spacer->get_size_request(width, height);

      int const expectedW = 200;
      int const expectedH = 100;
      CHECK(width == expectedW);
      CHECK(height == expectedH);
    }

    SECTION("an authored negative size request clears a component minimum")
    {
      auto doc = LayoutDocument{};
      doc.root.type = "box";

      auto child = LayoutNode{};
      child.type = "spacer";
      child.layout["widthRequest"] = LayoutValue{static_cast<std::int64_t>(-1)};
      child.layout["heightRequest"] = LayoutValue{static_cast<std::int64_t>(-1)};
      doc.root.children.push_back(child);

      auto const compPtr = layoutRuntime.build(ctx, preparedLayout(doc));
      auto* const box = dynamic_cast<Gtk::Box*>(&compPtr->widget());
      REQUIRE(box != nullptr);

      auto* const spacer = box->get_first_child();
      REQUIRE(spacer != nullptr);

      std::int32_t width = 0;
      std::int32_t height = 0;
      spacer->get_size_request(width, height);
      CHECK(width == -1);
      CHECK(height == -1);
    }
  }

  TEST_CASE("applyCommonProps - width-only requests preserve the component height minimum",
            "[gtk][unit][layout][container][geometry]")
  {
    auto fixture = LayoutRuntimeFixture{};
    registerRequestedSizeComponent(fixture);

    auto child = LayoutNode{.type = "test.requestedSize"};

    SECTION("a positive width replaces only the width request")
    {
      child.layout["widthRequest"] = LayoutValue{static_cast<std::int64_t>(200)};

      auto const compPtr = fixture.create(child);
      REQUIRE(compPtr != nullptr);

      std::int32_t width = 0;
      std::int32_t height = 0;
      compPtr->widget().get_size_request(width, height);
      CHECK(width == 200);
      CHECK(height == kComponentHeightRequest);
    }

    SECTION("a negative width clears only the width request")
    {
      child.layout["widthRequest"] = LayoutValue{static_cast<std::int64_t>(-1)};

      auto const compPtr = fixture.create(child);
      REQUIRE(compPtr != nullptr);

      std::int32_t width = 0;
      std::int32_t height = 0;
      compPtr->widget().get_size_request(width, height);
      CHECK(width == -1);
      CHECK(height == kComponentHeightRequest);
    }
  }

  TEST_CASE("applyCommonProps - height-only requests preserve the component width minimum",
            "[gtk][unit][layout][container][geometry]")
  {
    auto fixture = LayoutRuntimeFixture{};
    registerRequestedSizeComponent(fixture);

    auto child = LayoutNode{.type = "test.requestedSize"};

    SECTION("a positive height replaces only the height request")
    {
      child.layout["heightRequest"] = LayoutValue{static_cast<std::int64_t>(100)};

      auto const compPtr = fixture.create(child);
      REQUIRE(compPtr != nullptr);

      std::int32_t width = 0;
      std::int32_t height = 0;
      compPtr->widget().get_size_request(width, height);
      CHECK(width == kComponentWidthRequest);
      CHECK(height == 100);
    }

    SECTION("a negative height clears only the height request")
    {
      child.layout["heightRequest"] = LayoutValue{static_cast<std::int64_t>(-1)};

      auto const compPtr = fixture.create(child);
      REQUIRE(compPtr != nullptr);

      std::int32_t width = 0;
      std::int32_t height = 0;
      compPtr->widget().get_size_request(width, height);
      CHECK(width == kComponentWidthRequest);
      CHECK(height == -1);
    }
  }
} // namespace ao::gtk::layout::test
