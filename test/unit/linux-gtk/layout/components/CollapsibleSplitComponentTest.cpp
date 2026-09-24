// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "ContainerTestHelpers.h"
#include "app/linux-gtk/layout/runtime/LayoutRuntime.h"
#include "test/unit/linux-gtk/GtkApplicationTestSupport.h"
#include "test/unit/linux-gtk/GtkLayoutTestSupport.h"
#include "test/unit/linux-gtk/GtkWidgetTestSupport.h"
#include "test/unit/linux-gtk/layout/LayoutTestSupport.h"
#include "test/unit/linux-gtk/layout/state/FakeLayoutComponentStateStore.h"
#include <ao/uimodel/layout/component/LayoutComponentState.h>
#include <ao/uimodel/layout/document/LayoutDocument.h>
#include <ao/uimodel/layout/document/LayoutNode.h>
#include <ao/uimodel/layout/document/LayoutPreparation.h>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <gtkmm/box.h>
#include <gtkmm/button.h>
#include <gtkmm/enums.h>
#include <gtkmm/gesturedrag.h>
#include <gtkmm/revealer.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace ao::gtk::layout::test
{
  using namespace uimodel;
  using ao::gtk::test::AllocationHost;
  using ao::gtk::test::emitClicked;
  using ao::gtk::test::hasAccessibleLabel;
  using ao::gtk::test::measureWidget;

  TEST_CASE("CollapsibleSplitComponent - applies reveal sizing and drag adaptation",
            "[gtk][unit][layout-component][geometry]")
  {
    auto stateStore = FakeLayoutComponentStateStore{};
    auto fixture = LayoutRuntimeFixture{"io.github.aobus.layout_test", {}, "en", nullptr, &stateStore};
    auto& ctx = fixture.context();
    auto& layoutRuntime = fixture.layoutRuntime();

    SECTION("collapsibleSplit wraps the collapsible child in a revealer sizing pane")
    {
      auto doc = LayoutDocument{};
      doc.root.type = "collapsibleSplit";
      doc.root.props["orientation"] = LayoutValue{std::string{"horizontal"}};
      doc.root.props["position"] = LayoutValue{static_cast<std::int64_t>(420)};
      doc.root.props["revealed"] = LayoutValue{false};

      doc.root.children.push_back(LayoutNode{.type = "spacer"});
      doc.root.children.push_back(LayoutNode{.type = "spacer"});

      auto const compPtr = layoutRuntime.build(ctx, preparedLayout(doc));
      auto* const box = collapsibleSplitBox(*compPtr);

      REQUIRE(box != nullptr);
      CHECK(box->get_orientation() == Gtk::Orientation::HORIZONTAL);

      auto* const workspace = box->get_first_child();
      REQUIRE(workspace != nullptr);
      CHECK(workspace->get_hexpand() == true);

      auto* const gutterBox = dynamic_cast<Gtk::Box*>(workspace->get_next_sibling());
      REQUIRE(gutterBox != nullptr);
      CHECK(gutterBox->has_css_class("ao-detail-resize-grip"));

      auto* const handleWidget = gutterBox->get_first_child();
      REQUIRE(handleWidget != nullptr);
      auto* const handleButton = dynamic_cast<Gtk::Button*>(handleWidget);
      REQUIRE(handleButton != nullptr);
      auto windowFixture = ao::gtk::test::GtkWindowFixture{};
      windowFixture.mount(compPtr->widget());
      windowFixture.present();
      CHECK(handleButton->get_valign() == Gtk::Align::CENTER);
      CHECK_FALSE(handleButton->get_vexpand());
      CHECK(hasAccessibleLabel(*handleButton, "Expand panel"));

      auto* const revealer = dynamic_cast<Gtk::Revealer*>(gutterBox->get_next_sibling());
      REQUIRE(revealer != nullptr);
      CHECK(revealer->get_reveal_child() == false);
      CHECK(revealer->get_transition_type() == Gtk::RevealerTransitionType::SLIDE_LEFT);

      emitClicked(*handleButton);
      CHECK(revealer->get_reveal_child() == true);
      CHECK(hasAccessibleLabel(*handleButton, "Collapse panel"));

      auto* const paneSizer = revealer->get_child();
      REQUIRE(paneSizer != nullptr);

      int const expectedWidth = 420;
      auto const horizontalMeasure = measureWidget(*paneSizer, Gtk::Orientation::HORIZONTAL);
      CHECK(horizontalMeasure.minimum == expectedWidth);
      CHECK(horizontalMeasure.natural == expectedWidth);
    }

    SECTION("collapsibleSplit pane size is not expanded by child minimum width")
    {
      auto doc = LayoutDocument{};
      doc.root.type = "collapsibleSplit";
      doc.root.props["orientation"] = LayoutValue{std::string{"horizontal"}};
      doc.root.props["position"] = LayoutValue{static_cast<std::int64_t>(120)};

      auto workspaceNode = LayoutNode{.type = "spacer"};
      auto detailNode = LayoutNode{.type = "spacer"};
      detailNode.layout["widthRequest"] = LayoutValue{static_cast<std::int64_t>(900)};
      doc.root.children.push_back(workspaceNode);
      doc.root.children.push_back(detailNode);

      auto const compPtr = layoutRuntime.build(ctx, preparedLayout(doc));
      auto* const box = collapsibleSplitBox(*compPtr);

      REQUIRE(box != nullptr);

      auto* const workspace = box->get_first_child();
      REQUIRE(workspace != nullptr);

      auto* const gutterBox = workspace->get_next_sibling();
      REQUIRE(gutterBox != nullptr);

      auto* const revealer = dynamic_cast<Gtk::Revealer*>(gutterBox->get_next_sibling());
      REQUIRE(revealer != nullptr);

      auto* const paneSizer = revealer->get_child();
      REQUIRE(paneSizer != nullptr);

      int const expectedWidth = 120;
      auto const horizontalMeasure = measureWidget(*paneSizer, Gtk::Orientation::HORIZONTAL);
      CHECK(horizontalMeasure.minimum == expectedWidth);
      CHECK(horizontalMeasure.natural == expectedWidth);
      CHECK(paneSizer->get_overflow() == Gtk::Overflow::HIDDEN);
    }

    SECTION("collapsibleSplit start side places the revealer before the handle")
    {
      auto doc = LayoutDocument{};
      doc.root.type = "collapsibleSplit";
      doc.root.props["orientation"] = LayoutValue{std::string{"vertical"}};
      doc.root.props["collapseSide"] = LayoutValue{std::string{"start"}};
      doc.root.props["position"] = LayoutValue{static_cast<std::int64_t>(180)};
      doc.root.props["revealed"] = LayoutValue{true};

      doc.root.children.push_back(LayoutNode{.type = "spacer"});
      doc.root.children.push_back(LayoutNode{.type = "spacer"});

      auto const compPtr = layoutRuntime.build(ctx, preparedLayout(doc));
      auto* const box = collapsibleSplitBox(*compPtr);

      REQUIRE(box != nullptr);
      CHECK(box->get_orientation() == Gtk::Orientation::VERTICAL);

      auto* const revealer = dynamic_cast<Gtk::Revealer*>(box->get_first_child());
      REQUIRE(revealer != nullptr);
      CHECK(revealer->get_reveal_child() == true);
      CHECK(revealer->get_transition_type() == Gtk::RevealerTransitionType::SLIDE_DOWN);

      auto* const gutterBox = dynamic_cast<Gtk::Box*>(revealer->get_next_sibling());
      REQUIRE(gutterBox != nullptr);
      CHECK(gutterBox->has_css_class("ao-detail-resize-grip"));

      auto* const handleWidget = gutterBox->get_first_child();
      REQUIRE(handleWidget != nullptr);
      auto* const handleButton = dynamic_cast<Gtk::Button*>(handleWidget);
      REQUIRE(handleButton != nullptr);

      emitClicked(*handleButton);
      CHECK(revealer->get_reveal_child() == false);

      auto* const paneSizer = revealer->get_child();
      REQUIRE(paneSizer != nullptr);

      int const expectedHeight = 180;
      auto const verticalMeasure = measureWidget(*paneSizer, Gtk::Orientation::VERTICAL);
      CHECK(verticalMeasure.minimum == expectedHeight);
      CHECK(verticalMeasure.natural == expectedHeight);
    }

    SECTION("collapsibleSplit invalid position falls back to default detail size")
    {
      auto doc = LayoutDocument{};
      doc.root.type = "collapsibleSplit";
      doc.root.props["orientation"] = LayoutValue{std::string{"horizontal"}};
      doc.root.props["position"] = LayoutValue{static_cast<std::int64_t>(-1)};

      doc.root.children.push_back(LayoutNode{.type = "spacer"});
      doc.root.children.push_back(LayoutNode{.type = "spacer"});

      auto const compPtr = layoutRuntime.build(ctx, preparedLayout(doc));
      auto* const box = collapsibleSplitBox(*compPtr);

      REQUIRE(box != nullptr);

      auto* const workspace = box->get_first_child();
      REQUIRE(workspace != nullptr);

      auto* const gutterBox = workspace->get_next_sibling();
      REQUIRE(gutterBox != nullptr);

      auto* const revealer = dynamic_cast<Gtk::Revealer*>(gutterBox->get_next_sibling());
      REQUIRE(revealer != nullptr);

      auto* const paneSizer = revealer->get_child();
      REQUIRE(paneSizer != nullptr);

      int const expectedDefaultWidth = 50;
      auto const horizontalMeasure = measureWidget(*paneSizer, Gtk::Orientation::HORIZONTAL);
      CHECK(horizontalMeasure.minimum == expectedDefaultWidth);
      CHECK(horizontalMeasure.natural == expectedDefaultWidth);
    }

    SECTION("collapsibleSplit percent size follows later container allocations before manual resize")
    {
      auto doc = LayoutDocument{};
      doc.root.type = "collapsibleSplit";
      doc.root.props["orientation"] = LayoutValue{std::string{"horizontal"}};
      doc.root.props["initialPositionPercent"] = LayoutValue{0.3};

      doc.root.children.push_back(LayoutNode{.type = "spacer"});
      doc.root.children.push_back(LayoutNode{.type = "spacer"});

      auto const compPtr = layoutRuntime.build(ctx, preparedLayout(doc));
      auto* const box = collapsibleSplitBox(*compPtr);

      REQUIRE(box != nullptr);

      auto* const workspace = box->get_first_child();
      REQUIRE(workspace != nullptr);

      auto* const gutterBox = workspace->get_next_sibling();
      REQUIRE(gutterBox != nullptr);

      auto* const revealer = dynamic_cast<Gtk::Revealer*>(gutterBox->get_next_sibling());
      REQUIRE(revealer != nullptr);

      auto* const paneSizer = revealer->get_child();
      REQUIRE(paneSizer != nullptr);

      auto allocationHost = AllocationHost{compPtr->widget()};

      allocationHost.allocateChild(834, 300);
      auto firstMeasure = measureWidget(*paneSizer, Gtk::Orientation::HORIZONTAL);
      CHECK(firstMeasure.minimum == 250);
      CHECK(firstMeasure.natural == 250);

      allocationHost.allocateChild(2000, 300);
      auto secondMeasure = measureWidget(*paneSizer, Gtk::Orientation::HORIZONTAL);
      CHECK(secondMeasure.minimum == 600);
      CHECK(secondMeasure.natural == 600);
    }

    SECTION("collapsibleSplit drag callbacks apply threshold side axis floor and persistence")
    {
      struct DragVariant final
      {
        std::string_view id;
        std::string_view orientation;
        std::string_view collapseSide;
        double offsetX;
        double offsetY;
        std::int32_t expectedSize;
        bool persists;
      };

      auto constexpr kVariants = std::to_array<DragVariant>({
        {.id = "below-threshold",
         .orientation = "horizontal",
         .collapseSide = "end",
         .offsetX = 2.0,
         .offsetY = 200.0,
         .expectedSize = 180,
         .persists = false},
        {.id = "horizontal-end",
         .orientation = "horizontal",
         .collapseSide = "end",
         .offsetX = 40.0,
         .offsetY = 200.0,
         .expectedSize = 140,
         .persists = true},
        {.id = "horizontal-start",
         .orientation = "horizontal",
         .collapseSide = "start",
         .offsetX = 40.0,
         .offsetY = 200.0,
         .expectedSize = 220,
         .persists = true},
        {.id = "vertical-end",
         .orientation = "vertical",
         .collapseSide = "end",
         .offsetX = 200.0,
         .offsetY = 40.0,
         .expectedSize = 140,
         .persists = true},
        {.id = "vertical-start",
         .orientation = "vertical",
         .collapseSide = "start",
         .offsetX = 200.0,
         .offsetY = 40.0,
         .expectedSize = 220,
         .persists = true},
        {.id = "minimum-floor",
         .orientation = "horizontal",
         .collapseSide = "end",
         .offsetX = 500.0,
         .offsetY = 200.0,
         .expectedSize = 50,
         .persists = true},
      });

      fixture.setComponentState("classic", LayoutComponentStateDocument{.preset = "classic"});

      for (auto const& variant : kVariants)
      {
        INFO(variant.id);
        auto doc = LayoutDocument{};
        doc.root.id = std::string{variant.id};
        doc.root.type = "collapsibleSplit";
        doc.root.props["orientation"] = LayoutValue{std::string{variant.orientation}};
        doc.root.props["collapseSide"] = LayoutValue{std::string{variant.collapseSide}};
        doc.root.props["position"] = LayoutValue{static_cast<std::int64_t>(180)};
        doc.root.props["revealed"] = LayoutValue{true};
        doc.root.children.push_back(LayoutNode{.type = "spacer"});
        doc.root.children.push_back(LayoutNode{.type = "spacer"});

        auto const compPtr = layoutRuntime.build(ctx, preparedLayout(doc));
        auto* const box = collapsibleSplitBox(*compPtr);
        REQUIRE(box != nullptr);
        auto* const gutterBox = ao::gtk::test::findWidgetByClass<Gtk::Box>(*box, "ao-detail-resize-grip");
        REQUIRE(gutterBox != nullptr);
        auto* const revealer = ao::gtk::test::findWidget<Gtk::Revealer>(*box);
        REQUIRE(revealer != nullptr);
        auto* const paneSizer = revealer->get_child();
        REQUIRE(paneSizer != nullptr);

        revealer->set_transition_duration(0);
        auto allocationHost = AllocationHost{compPtr->widget()};
        allocationHost.allocateChild(800, 600);
        auto windowFixture = ao::gtk::test::GtkWindowFixture{};
        windowFixture.mount(allocationHost);
        windowFixture.present();
        REQUIRE(ao::gtk::test::tryPumpGtkEventsUntil([&] { return gutterBox->get_mapped(); }));
        REQUIRE(gutterBox->get_width() > 0);
        REQUIRE(gutterBox->get_height() > 0);
        REQUIRE(gutterBox->contains(
          static_cast<double>(gutterBox->get_width()) / 2.0, static_cast<double>(gutterBox->get_height()) / 2.0));
        REQUIRE((variant.orientation == "horizontal" ? paneSizer->get_width() : paneSizer->get_height()) == 180);

        double dragStartX = 0.0;
        double dragStartY = 0.0;
        REQUIRE(gutterBox->translate_coordinates(*box,
                                                 static_cast<double>(gutterBox->get_width()) / 2.0,
                                                 static_cast<double>(gutterBox->get_height()) / 2.0,
                                                 dragStartX,
                                                 dragStartY));
        auto const dragPtr = ao::gtk::test::findController<Gtk::GestureDrag>(*box);
        REQUIRE(dragPtr);
        auto const saveCountBeforeDrag = stateStore.saveCount();

        ::g_signal_emit_by_name(dragPtr->gobj(), "drag-begin", dragStartX, dragStartY);
        ::g_signal_emit_by_name(dragPtr->gobj(), "drag-update", variant.offsetX, variant.offsetY);
        ::g_signal_emit_by_name(dragPtr->gobj(), "drag-end", variant.offsetX, variant.offsetY);

        auto const axis =
          variant.orientation == "horizontal" ? Gtk::Orientation::HORIZONTAL : Gtk::Orientation::VERTICAL;
        auto const measure = measureWidget(*paneSizer, axis);
        CHECK(measure.minimum == variant.expectedSize);
        CHECK(measure.natural == variant.expectedSize);
        CHECK(stateStore.saveCount() == saveCountBeforeDrag + (variant.persists ? 1 : 0));

        if (variant.persists)
        {
          REQUIRE(stateStore.document().components.contains(variant.id));
          auto const& entry = stateStore.document().components.at(doc.root.id);
          CHECK(entry.state.at("size").asInt() == variant.expectedSize);
          CHECK(entry.state.at("revealed").asBool(false));
        }
      }
    }
  }

  TEST_CASE("CollapsibleSplitComponent - allocated revealed pane enforces the minimum on both axes",
            "[gtk][unit][layout-component][geometry]")
  {
    auto fixture = LayoutRuntimeFixture{};

    for (auto const& orientation : {std::string{"horizontal"}, std::string{"vertical"}})
    {
      for (auto const optPosition : std::array<std::optional<std::int64_t>, 5>{std::nullopt, -1, 0, 20, 180})
      {
        INFO("axis=" << orientation << " position=" << optPosition.value_or(-999));
        auto doc = LayoutDocument{};
        doc.root.type = "collapsibleSplit";
        doc.root.props["orientation"] = LayoutValue{orientation};
        doc.root.props["revealed"] = LayoutValue{true};

        if (optPosition)
        {
          doc.root.props["position"] = LayoutValue{*optPosition};
        }

        doc.root.children.push_back(LayoutNode{.type = "spacer"});
        doc.root.children.push_back(LayoutNode{.type = "spacer"});

        auto const compPtr = fixture.layoutRuntime().build(fixture.context(), preparedLayout(doc));
        auto* const box = collapsibleSplitBox(*compPtr);
        REQUIRE(box != nullptr);
        auto* const revealer = ao::gtk::test::findWidget<Gtk::Revealer>(*box);
        REQUIRE(revealer != nullptr);
        revealer->set_transition_duration(0);
        auto* const paneSizer = revealer->get_child();
        REQUIRE(paneSizer != nullptr);

        auto windowFixture = ao::gtk::test::GtkWindowFixture{};
        windowFixture.window().set_default_size(800, 600);
        windowFixture.mount(compPtr->widget());
        windowFixture.present();
        REQUIRE(ao::gtk::test::tryPumpGtkEventsUntil(
          [&] { return box->get_mapped() && box->get_width() >= 800 && box->get_height() >= 600; }));
        REQUIRE(revealer->get_reveal_child());
        REQUIRE(revealer->get_child_revealed());

        auto const extent = orientation == "horizontal" ? paneSizer->get_width() : paneSizer->get_height();
        auto const expectedExtent = static_cast<std::int32_t>(std::max<std::int64_t>(50, optPosition.value_or(0)));
        CHECK(extent == expectedExtent);
      }
    }
  }

  TEST_CASE("CollapsibleSplitComponent - restores guarded panel state", "[gtk][unit][layout-component][geometry]")
  {
    auto stateStore = FakeLayoutComponentStateStore{};
    auto fixture = LayoutRuntimeFixture{"io.github.aobus.layout_test", {}, "en", nullptr, &stateStore};
    auto& ctx = fixture.context();
    auto& layoutRuntime = fixture.layoutRuntime();

    SECTION("collapsibleSplit persisted size overrides layout defaults and clamps to narrow allocation")
    {
      auto doc = LayoutDocument{};
      doc.root.id = "detail-split";
      doc.root.type = "collapsibleSplit";
      doc.root.props["orientation"] = LayoutValue{std::string{"horizontal"}};
      doc.root.props["position"] = LayoutValue{static_cast<std::int64_t>(420)};
      doc.root.props["initialPositionPercent"] = LayoutValue{0.3};
      doc.root.props["revealed"] = LayoutValue{true};

      doc.root.children.push_back(LayoutNode{.type = "spacer"});
      doc.root.children.push_back(LayoutNode{.type = "spacer"});

      auto state = LayoutComponentStateDocument{.preset = "classic"};
      state.components["detail-split"] = LayoutComponentStateEntry{
        .type = "collapsibleSplit",
        .stateVersion = kStateEntryVersion,
        .baselineHash = componentBaselineHash(doc.root),
        .state = {{"size", LayoutValue{static_cast<std::int64_t>(900)}}, {"revealed", LayoutValue{true}}},
      };
      fixture.setComponentState("classic", state);

      auto const compPtr = layoutRuntime.build(ctx, preparedLayout(doc));
      auto* const box = collapsibleSplitBox(*compPtr);

      REQUIRE(box != nullptr);

      auto* const revealer = endSideCollapsibleRevealer(*box);
      REQUIRE(revealer != nullptr);

      auto* const paneSizer = revealer->get_child();
      REQUIRE(paneSizer != nullptr);

      auto allocationHost = AllocationHost{compPtr->widget()};
      allocationHost.allocateChild(300, 200);

      auto const horizontalMeasure = measureWidget(*paneSizer, Gtk::Orientation::HORIZONTAL);
      CHECK(horizontalMeasure.minimum == 238);
      CHECK(horizontalMeasure.natural == 238);
    }

    SECTION("collapsibleSplit persisted revealed restores only with matching baseline")
    {
      auto doc = LayoutDocument{};
      doc.root.id = "detail-split";
      doc.root.type = "collapsibleSplit";
      doc.root.props["orientation"] = LayoutValue{std::string{"horizontal"}};
      doc.root.props["position"] = LayoutValue{static_cast<std::int64_t>(180)};
      doc.root.props["revealed"] = LayoutValue{true};

      doc.root.children.push_back(LayoutNode{.type = "spacer"});
      doc.root.children.push_back(LayoutNode{.type = "spacer"});

      auto state = LayoutComponentStateDocument{.preset = "classic"};
      state.components["detail-split"] = LayoutComponentStateEntry{
        .type = "collapsibleSplit",
        .stateVersion = kStateEntryVersion,
        .baselineHash = componentBaselineHash(doc.root),
        .state = {{"size", LayoutValue{static_cast<std::int64_t>(180)}}, {"revealed", LayoutValue{false}}},
      };
      fixture.setComponentState("classic", state);

      auto const restoredPtr = layoutRuntime.build(ctx, preparedLayout(doc));
      auto* const restoredBox = collapsibleSplitBox(*restoredPtr);
      REQUIRE(restoredBox != nullptr);

      auto* const restoredRevealer = endSideCollapsibleRevealer(*restoredBox);
      REQUIRE(restoredRevealer != nullptr);
      CHECK(restoredRevealer->get_reveal_child() == false);

      state.components["detail-split"].baselineHash = "stale";
      fixture.setComponentState("classic", state);

      auto const fallbackPtr = layoutRuntime.build(ctx, preparedLayout(doc));
      auto* const fallbackBox = collapsibleSplitBox(*fallbackPtr);
      REQUIRE(fallbackBox != nullptr);

      auto* const fallbackRevealer = endSideCollapsibleRevealer(*fallbackBox);
      REQUIRE(fallbackRevealer != nullptr);
      CHECK(fallbackRevealer->get_reveal_child() == true);
    }
  }

  TEST_CASE("CollapsibleSplitComponent - persists current state and fences retired generations",
            "[gtk][unit][layout-component][geometry][async]")
  {
    auto stateStore = FakeLayoutComponentStateStore{};
    auto fixture = LayoutRuntimeFixture{"io.github.aobus.layout_test", {}, "en", nullptr, &stateStore};
    auto& ctx = fixture.context();
    auto& layoutRuntime = fixture.layoutRuntime();

    SECTION("collapsibleSplit toggle persists revealed state and current size")
    {
      fixture.setComponentState("classic", LayoutComponentStateDocument{.preset = "classic"});

      auto doc = LayoutDocument{};
      doc.root.id = "detail-split";
      doc.root.type = "collapsibleSplit";
      doc.root.props["orientation"] = LayoutValue{std::string{"horizontal"}};
      doc.root.props["position"] = LayoutValue{static_cast<std::int64_t>(180)};
      doc.root.props["revealed"] = LayoutValue{true};

      doc.root.children.push_back(LayoutNode{.type = "spacer"});
      doc.root.children.push_back(LayoutNode{.type = "spacer"});

      auto const compPtr = layoutRuntime.build(ctx, preparedLayout(doc));
      auto* const box = collapsibleSplitBox(*compPtr);
      REQUIRE(box != nullptr);

      auto* const handleButton = endSideCollapsibleToggle(*box);
      REQUIRE(handleButton != nullptr);

      emitClicked(*handleButton);

      CHECK(stateStore.saveCount() == 1);
      REQUIRE(stateStore.document().components.contains("detail-split"));
      auto const& entry = stateStore.document().components.at("detail-split");
      CHECK(entry.type == "collapsibleSplit");
      CHECK(entry.baselineHash == componentBaselineHash(doc.root));
      CHECK(entry.state.at("revealed").asBool(true) == false);
      CHECK(entry.state.at("size").asInt() == 180);
    }

    SECTION("collapsibleSplit edit mode toggle does not persist runtime state")
    {
      fixture.setComponentState("classic", LayoutComponentStateDocument{.preset = "classic"}, true);

      auto doc = LayoutDocument{};
      doc.root.id = "detail-split";
      doc.root.type = "collapsibleSplit";
      doc.root.props["orientation"] = LayoutValue{std::string{"horizontal"}};
      doc.root.props["position"] = LayoutValue{static_cast<std::int64_t>(180)};
      doc.root.props["revealed"] = LayoutValue{true};

      doc.root.children.push_back(LayoutNode{.type = "spacer"});
      doc.root.children.push_back(LayoutNode{.type = "spacer"});

      auto const compPtr = layoutRuntime.build(ctx, preparedLayout(doc));
      auto* const box = collapsibleSplitBox(*compPtr);
      REQUIRE(box != nullptr);

      auto* const handleButton = endSideCollapsibleToggle(*box);
      REQUIRE(handleButton != nullptr);

      emitClicked(*handleButton);

      CHECK(stateStore.saveCount() == 0);
      CHECK(stateStore.document().components.empty());
    }

    SECTION("collapsibleSplit ignores state writes after context generation advances")
    {
      fixture.setComponentState("classic", LayoutComponentStateDocument{.preset = "classic"});

      auto doc = LayoutDocument{};
      doc.root.id = "detail-split";
      doc.root.type = "collapsibleSplit";
      doc.root.props["orientation"] = LayoutValue{std::string{"horizontal"}};
      doc.root.props["position"] = LayoutValue{static_cast<std::int64_t>(180)};
      doc.root.props["revealed"] = LayoutValue{true};

      doc.root.children.push_back(LayoutNode{.type = "spacer"});
      doc.root.children.push_back(LayoutNode{.type = "spacer"});

      {
        auto const compPtr = layoutRuntime.build(ctx, preparedLayout(doc));
        auto* const box = collapsibleSplitBox(*compPtr);
        REQUIRE(box != nullptr);

        auto* const handleButton = endSideCollapsibleToggle(*box);
        REQUIRE(handleButton != nullptr);

        fixture.advanceGeneration();
        emitClicked(*handleButton);
      }

      CHECK(stateStore.saveCount() == 0);
      CHECK(stateStore.document().components.empty());
    }
  }

  TEST_CASE("CollapsibleSplitComponent - toggle accessibility copy follows the selected locale",
            "[gtk][unit][layout-component][localization]")
  {
    auto fixture = LayoutRuntimeFixture{"io.github.aobus.collapsible_localization_test", {}, "de-DE"};
    auto& ctx = fixture.context();
    auto& layoutRuntime = fixture.layoutRuntime();
    auto doc = LayoutDocument{};
    doc.root.type = "collapsibleSplit";
    doc.root.props["revealed"] = LayoutValue{false};
    doc.root.children.push_back(LayoutNode{.type = "spacer"});
    doc.root.children.push_back(LayoutNode{.type = "spacer"});

    auto const componentPtr = layoutRuntime.build(ctx, preparedLayout(doc));
    auto* const box = collapsibleSplitBox(*componentPtr);
    REQUIRE(box != nullptr);
    auto* const toggle = endSideCollapsibleToggle(*box);
    REQUIRE(toggle != nullptr);
    CHECK(hasAccessibleLabel(*toggle, "Bereich ausklappen"));

    emitClicked(*toggle);
    CHECK(hasAccessibleLabel(*toggle, "Bereich einklappen"));
  }
} // namespace ao::gtk::layout::test
