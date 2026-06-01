// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "app/linux-gtk/layout/components/Containers.h"

#include "../../GtkTestSupport.h"
#include "app/linux-gtk/layout/document/LayoutNode.h"
#include "app/linux-gtk/layout/runtime/ActionRegistry.h"
#include "app/linux-gtk/layout/runtime/ComponentRegistry.h"
#include "app/linux-gtk/layout/runtime/LayoutHost.h"
#include "app/linux-gtk/layout/runtime/LayoutRuntime.h"
#include "layout/document/LayoutDocument.h"
#include "test/unit/lmdb/TestUtils.h"

#include <catch2/catch_test_macros.hpp>
#include <gtkmm/application.h>
#include <gtkmm/box.h>
#include <gtkmm/button.h>
#include <gtkmm/centerbox.h>
#include <gtkmm/enums.h>
#include <gtkmm/label.h>
#include <gtkmm/paned.h>
#include <gtkmm/revealer.h>
#include <gtkmm/scrolledwindow.h>
#include <gtkmm/separator.h>
#include <gtkmm/stack.h>
#include <gtkmm/window.h>

#include <cstdint>
#include <memory>
#include <vector>

namespace ao::gtk::layout::test
{
  using ao::gtk::test::makeRuntime;

  namespace
  {
    using namespace ao::lmdb::test;
  } // namespace

  TEST_CASE("LayoutRuntime building", "[layout][unit][containers]")
  {
    auto const appPtr = Gtk::Application::create("io.github.aobus.layout_test");

    auto const tempDir = TempDir{};
    auto runtime = makeRuntime(tempDir);

    auto registry = ComponentRegistry{};
    LayoutRuntime::registerStandardComponents(registry);

    auto window = Gtk::Window{};
    auto const actionRegistry = ActionRegistry{};
    auto ctx =
      LayoutContext{.registry = registry, .actionRegistry = actionRegistry, .runtime = runtime, .parentWindow = window};

    auto layoutRuntime = LayoutRuntime{registry};

    SECTION("Build default layout")
    {
      auto const doc = createDefaultLayout();
      auto const rootComponentPtr = layoutRuntime.build(ctx, doc);

      REQUIRE(rootComponentPtr != nullptr);

      auto& widget = rootComponentPtr->widget();
      CHECK(dynamic_cast<Gtk::Box*>(&widget) != nullptr);
    }

    SECTION("Build nested layout")
    {
      auto doc = LayoutDocument{};
      doc.root.type = "box";
      doc.root.props["orientation"] = LayoutValue{std::string{"horizontal"}};

      auto child1 = LayoutNode{};
      child1.type = "spacer";
      doc.root.children.push_back(child1);

      auto child2 = LayoutNode{};
      child2.type = "box";
      child2.props["orientation"] = LayoutValue{std::string{"vertical"}};
      doc.root.children.push_back(child2);

      auto const rootComponentPtr = layoutRuntime.build(ctx, doc);

      REQUIRE(rootComponentPtr != nullptr);

      auto& widget = rootComponentPtr->widget();
      auto* const box = dynamic_cast<Gtk::Box*>(&widget);
      REQUIRE(box != nullptr);
      CHECK(box->get_orientation() == Gtk::Orientation::HORIZONTAL);
    }

    SECTION("Unknown component type produces error label")
    {
      auto doc = LayoutDocument{};
      doc.root.type = "nonexistent.component";

      auto const rootComponentPtr = layoutRuntime.build(ctx, doc);

      REQUIRE(rootComponentPtr != nullptr);

      auto& widget = rootComponentPtr->widget();
      auto* const label = dynamic_cast<Gtk::Label*>(&widget);
      REQUIRE(label != nullptr);
      CHECK(label->get_label().find("[Layout Error]") != std::string::npos);
      CHECK(label->get_label().find("nonexistent.component") != std::string::npos);
    }

    SECTION("Box component forwards cssClasses to widget")
    {
      auto doc = LayoutDocument{};
      doc.root.type = "box";
      doc.root.layout["cssClasses"] = LayoutValue{std::string{"ao-test-class"}};

      auto const rootComponentPtr = layoutRuntime.build(ctx, doc);

      REQUIRE(rootComponentPtr != nullptr);
      auto* const box = dynamic_cast<Gtk::Box*>(&rootComponentPtr->widget());
      REQUIRE(box != nullptr);
      CHECK(box->has_css_class("ao-test-class"));
    }

    SECTION("Playback bar groups carry ao-grouping-region (direct template)")
    {
      auto const templates = getBuiltInTemplates();
      auto const& barTemplate = templates.at("playback.defaultBar");
      auto const barCompPtr = ctx.registry.create(ctx, barTemplate);

      REQUIRE(barCompPtr != nullptr);
      auto* const barBox = dynamic_cast<Gtk::Box*>(&barCompPtr->widget());
      REQUIRE(barBox != nullptr);

      auto* const leftChild = barBox->get_first_child();
      REQUIRE(leftChild != nullptr);
      CHECK(leftChild->has_css_class("ao-grouping-region"));

      auto* const rightChild = leftChild->get_next_sibling()->get_next_sibling();
      REQUIRE(rightChild != nullptr);
      CHECK(rightChild->has_css_class("ao-grouping-region"));
    }

    SECTION("Playback bar groups carry ao-grouping-region (via template expansion)")
    {
      // This exercises the same path as the real app: default layout with
      // template reference node, expanded through LayoutRuntime::build().
      auto doc = createDefaultLayout();
      doc.templates = getBuiltInTemplates();
      auto const fullLayoutPtr = layoutRuntime.build(ctx, doc);

      REQUIRE(fullLayoutPtr != nullptr);
      // Find the playback row child within the root box.
      auto* const rootBox = dynamic_cast<Gtk::Box*>(&fullLayoutPtr->widget());
      REQUIRE(rootBox != nullptr);

      // Child order: 0=menuBar, 1=playback-bar (expanded template), 2=split, 3=status bar
      auto* const playbackBar = rootBox->get_first_child()->get_next_sibling();
      REQUIRE(playbackBar != nullptr);
      auto* const barBox = dynamic_cast<Gtk::Box*>(playbackBar);
      REQUIRE(barBox != nullptr);

      auto* const leftChild = barBox->get_first_child();
      REQUIRE(leftChild != nullptr);
      CHECK(leftChild->has_css_class("ao-grouping-region"));

      auto* const rightChild = leftChild->get_next_sibling()->get_next_sibling();
      REQUIRE(rightChild != nullptr);
      CHECK(rightChild->has_css_class("ao-grouping-region"));
    }
  }

  // ---------------------------------------------------------------------------
  // Container error states
  // ---------------------------------------------------------------------------
  TEST_CASE("Container error states", "[layout][unit][containers]")
  {
    auto const appPtr = Gtk::Application::create("io.github.aobus.layout_test");

    auto const tempDir = TempDir{};
    auto runtime = makeRuntime(tempDir);

    auto registry = ComponentRegistry{};
    LayoutRuntime::registerStandardComponents(registry);

    auto window = Gtk::Window{};
    auto const actionRegistry = ActionRegistry{};
    auto ctx =
      LayoutContext{.registry = registry, .actionRegistry = actionRegistry, .runtime = runtime, .parentWindow = window};

    auto layoutRuntime = LayoutRuntime{registry};

    auto const checkError = [](ILayoutComponent& compPtr, std::string const& expectedFragment)
    {
      auto* const label = dynamic_cast<Gtk::Label*>(&compPtr.widget());
      REQUIRE(label != nullptr);
      CHECK(label->get_label().find("[Layout Error]") != std::string::npos);
      CHECK(label->get_label().find(expectedFragment) != std::string::npos);
    };

    SECTION("split with 0 children returns error")
    {
      auto doc = LayoutDocument{};
      doc.root.type = "split";
      auto const compPtr = layoutRuntime.build(ctx, doc);
      checkError(*compPtr, "2 children");
    }

    SECTION("split with 1 child returns error")
    {
      auto doc = LayoutDocument{};
      doc.root.type = "split";
      doc.root.children.push_back(LayoutNode{.type = "spacer"});
      auto const compPtr = layoutRuntime.build(ctx, doc);
      checkError(*compPtr, "2 children");
    }

    SECTION("split with 3 children returns error")
    {
      auto doc = LayoutDocument{};
      doc.root.type = "split";
      doc.root.children.push_back(LayoutNode{.type = "spacer"});
      doc.root.children.push_back(LayoutNode{.type = "spacer"});
      doc.root.children.push_back(LayoutNode{.type = "spacer"});
      auto const compPtr = layoutRuntime.build(ctx, doc);
      checkError(*compPtr, "2 children");
    }

    SECTION("scroll with 0 children returns error")
    {
      auto doc = LayoutDocument{};
      doc.root.type = "scroll";
      auto const compPtr = layoutRuntime.build(ctx, doc);
      checkError(*compPtr, "1 child");
    }

    SECTION("scroll with 2 children returns error")
    {
      auto doc = LayoutDocument{};
      doc.root.type = "scroll";
      doc.root.children.push_back(LayoutNode{.type = "spacer"});
      doc.root.children.push_back(LayoutNode{.type = "spacer"});
      auto const compPtr = layoutRuntime.build(ctx, doc);
      checkError(*compPtr, "1 child");
    }

    SECTION("tabs with 0 children returns error")
    {
      auto doc = LayoutDocument{};
      doc.root.type = "tabs";
      auto const compPtr = layoutRuntime.build(ctx, doc);
      checkError(*compPtr, "at least 1 child");
    }
  }

  // ---------------------------------------------------------------------------
  // Container success states
  // ---------------------------------------------------------------------------
  TEST_CASE("Container success states", "[layout][unit][containers]")
  {
    auto const appPtr = Gtk::Application::create("io.github.aobus.layout_test");

    auto const tempDir = TempDir{};
    auto runtime = makeRuntime(tempDir);

    auto registry = ComponentRegistry{};
    LayoutRuntime::registerStandardComponents(registry);

    auto window = Gtk::Window{};
    auto const actionRegistry = ActionRegistry{};
    auto ctx =
      LayoutContext{.registry = registry, .actionRegistry = actionRegistry, .runtime = runtime, .parentWindow = window};

    auto layoutRuntime = LayoutRuntime{registry};

    SECTION("split with 2 children builds Gtk::Paned")
    {
      auto doc = LayoutDocument{};
      doc.root.type = "split";
      doc.root.props["orientation"] = LayoutValue{std::string{"horizontal"}};
      doc.root.props["position"] = LayoutValue{static_cast<std::int64_t>(200)};
      doc.root.props["resizeStart"] = LayoutValue{false};
      doc.root.props["shrinkEnd"] = LayoutValue{true};

      doc.root.children.push_back(LayoutNode{.type = "spacer"});
      doc.root.children.push_back(LayoutNode{.type = "spacer"});

      auto const compPtr = layoutRuntime.build(ctx, doc);
      auto* const paned = dynamic_cast<Gtk::Paned*>(&compPtr->widget());

      REQUIRE(paned != nullptr);

      int const expectedPos = 200;
      CHECK(paned->get_position() == expectedPos);
      CHECK(paned->get_resize_start_child() == false);
      CHECK(paned->get_shrink_end_child() == true);
    }

    SECTION("collapsibleSplit wraps the collapsible child in a revealer sizing pane")
    {
      auto doc = LayoutDocument{};
      doc.root.type = "collapsibleSplit";
      doc.root.props["orientation"] = LayoutValue{std::string{"horizontal"}};
      doc.root.props["position"] = LayoutValue{static_cast<std::int64_t>(420)};
      doc.root.props["revealed"] = LayoutValue{false};

      doc.root.children.push_back(LayoutNode{.type = "spacer"});
      doc.root.children.push_back(LayoutNode{.type = "spacer"});

      auto const compPtr = layoutRuntime.build(ctx, doc);
      auto* const box = dynamic_cast<Gtk::Box*>(&compPtr->widget());

      REQUIRE(box != nullptr);
      CHECK(box->get_orientation() == Gtk::Orientation::HORIZONTAL);

      auto* const workspace = box->get_first_child();
      REQUIRE(workspace != nullptr);
      CHECK(workspace->get_hexpand() == true);

      auto* const gutterBox = workspace->get_next_sibling();
      REQUIRE(gutterBox != nullptr);
      CHECK(dynamic_cast<Gtk::Box*>(gutterBox) != nullptr);

      auto* const resizeGrip = gutterBox->get_first_child();
      REQUIRE(resizeGrip != nullptr);
      CHECK(resizeGrip->has_css_class("ao-detail-resize-grip"));

      auto* const handleWidget = resizeGrip->get_next_sibling();
      REQUIRE(handleWidget != nullptr);
      CHECK(dynamic_cast<Gtk::Button*>(handleWidget) != nullptr);

      auto* const revealer = dynamic_cast<Gtk::Revealer*>(gutterBox->get_next_sibling());
      REQUIRE(revealer != nullptr);
      CHECK(revealer->get_reveal_child() == false);
      CHECK(revealer->get_transition_type() == Gtk::RevealerTransitionType::SLIDE_LEFT);

      auto* const paneSizer = revealer->get_child();
      REQUIRE(paneSizer != nullptr);

      std::int32_t width = -1;
      std::int32_t height = -1;
      paneSizer->get_size_request(width, height);

      int const expectedWidth = 420;
      CHECK(width == expectedWidth);
      CHECK(height == -1);
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

      auto const compPtr = layoutRuntime.build(ctx, doc);
      auto* const box = dynamic_cast<Gtk::Box*>(&compPtr->widget());

      REQUIRE(box != nullptr);
      CHECK(box->get_orientation() == Gtk::Orientation::VERTICAL);

      auto* const revealer = dynamic_cast<Gtk::Revealer*>(box->get_first_child());
      REQUIRE(revealer != nullptr);
      CHECK(revealer->get_reveal_child() == true);
      CHECK(revealer->get_transition_type() == Gtk::RevealerTransitionType::SLIDE_DOWN);

      auto* const gutterBox = revealer->get_next_sibling();
      REQUIRE(gutterBox != nullptr);
      CHECK(dynamic_cast<Gtk::Box*>(gutterBox) != nullptr);

      auto* const handleWidget = gutterBox->get_first_child();
      REQUIRE(handleWidget != nullptr);
      CHECK(dynamic_cast<Gtk::Button*>(handleWidget) != nullptr);

      auto* const resizeGrip = handleWidget->get_next_sibling();
      REQUIRE(resizeGrip != nullptr);
      CHECK(resizeGrip->has_css_class("ao-detail-resize-grip"));

      auto* const paneSizer = revealer->get_child();
      REQUIRE(paneSizer != nullptr);

      std::int32_t width = -1;
      std::int32_t height = -1;
      paneSizer->get_size_request(width, height);

      int const expectedHeight = 180;
      CHECK(width == -1);
      CHECK(height == expectedHeight);
    }

    SECTION("collapsibleSplit invalid position falls back to default detail size")
    {
      auto doc = LayoutDocument{};
      doc.root.type = "collapsibleSplit";
      doc.root.props["orientation"] = LayoutValue{std::string{"horizontal"}};
      doc.root.props["position"] = LayoutValue{static_cast<std::int64_t>(-1)};

      doc.root.children.push_back(LayoutNode{.type = "spacer"});
      doc.root.children.push_back(LayoutNode{.type = "spacer"});

      auto const compPtr = layoutRuntime.build(ctx, doc);
      auto* const box = dynamic_cast<Gtk::Box*>(&compPtr->widget());

      REQUIRE(box != nullptr);

      auto* const workspace = box->get_first_child();
      REQUIRE(workspace != nullptr);

      auto* const gutterBox = workspace->get_next_sibling();
      REQUIRE(gutterBox != nullptr);

      auto* const revealer = dynamic_cast<Gtk::Revealer*>(gutterBox->get_next_sibling());
      REQUIRE(revealer != nullptr);

      auto* const paneSizer = revealer->get_child();
      REQUIRE(paneSizer != nullptr);

      std::int32_t width = -1;
      std::int32_t height = -1;
      paneSizer->get_size_request(width, height);

      int const expectedDefaultWidth = 300;
      CHECK(width == expectedDefaultWidth);
      CHECK(height == -1);
    }

    SECTION("centerBox with 3 children builds Gtk::CenterBox")
    {
      auto doc = LayoutDocument{};
      doc.root.type = "centerBox";
      doc.root.props["orientation"] = LayoutValue{std::string{"horizontal"}};

      auto c1 = LayoutNode{};
      c1.type = "spacer";
      c1.layout["slot"] = LayoutValue{std::string{"start"}};
      doc.root.children.push_back(c1);

      auto c2 = LayoutNode{};
      c2.type = "spacer";
      c2.layout["slot"] = LayoutValue{std::string{"center"}};
      doc.root.children.push_back(c2);

      auto c3 = LayoutNode{};
      c3.type = "spacer";
      c3.layout["slot"] = LayoutValue{std::string{"end"}};
      doc.root.children.push_back(c3);

      auto const compPtr = layoutRuntime.build(ctx, doc);
      auto* const cb = dynamic_cast<Gtk::CenterBox*>(&compPtr->widget());

      REQUIRE(cb != nullptr);
      CHECK(cb->get_orientation() == Gtk::Orientation::HORIZONTAL);
      CHECK(cb->get_start_widget() != nullptr);
      CHECK(cb->get_center_widget() != nullptr);
      CHECK(cb->get_end_widget() != nullptr);
    }

    SECTION("scroll with 1 child builds Gtk::ScrolledWindow")
    {
      auto doc = LayoutDocument{};
      doc.root.type = "scroll";
      doc.root.props["hscrollPolicy"] = LayoutValue{std::string{"never"}};
      doc.root.props["vscrollPolicy"] = LayoutValue{std::string{"always"}};
      doc.root.props["minContentWidth"] = LayoutValue{static_cast<std::int64_t>(400)};
      doc.root.props["minContentHeight"] = LayoutValue{static_cast<std::int64_t>(300)};
      doc.root.props["propagateNaturalWidth"] = LayoutValue{true};
      doc.root.props["propagateNaturalHeight"] = LayoutValue{true};

      doc.root.children.push_back(LayoutNode{.type = "spacer"});

      auto const compPtr = layoutRuntime.build(ctx, doc);
      auto* const sw = dynamic_cast<Gtk::ScrolledWindow*>(&compPtr->widget());

      REQUIRE(sw != nullptr);

      auto hpolicy = Gtk::PolicyType::NEVER;
      auto vpolicy = Gtk::PolicyType::NEVER;
      sw->get_policy(hpolicy, vpolicy);

      CHECK(hpolicy == Gtk::PolicyType::NEVER);
      CHECK(vpolicy == Gtk::PolicyType::ALWAYS);

      int const expectedW = 400;
      int const expectedH = 300;
      CHECK(sw->get_min_content_width() == expectedW);
      CHECK(sw->get_min_content_height() == expectedH);
      CHECK(sw->get_propagate_natural_width() == true);
      CHECK(sw->get_propagate_natural_height() == true);
    }

    SECTION("scroll with policy defaults uses automatic")
    {
      auto doc = LayoutDocument{};
      doc.root.type = "scroll";
      doc.root.children.push_back(LayoutNode{.type = "spacer"});

      auto const compPtr = layoutRuntime.build(ctx, doc);
      auto* const sw = dynamic_cast<Gtk::ScrolledWindow*>(&compPtr->widget());

      REQUIRE(sw != nullptr);

      auto hpolicy = Gtk::PolicyType::NEVER;
      auto vpolicy = Gtk::PolicyType::NEVER;
      sw->get_policy(hpolicy, vpolicy);

      CHECK(hpolicy == Gtk::PolicyType::AUTOMATIC);
      CHECK(vpolicy == Gtk::PolicyType::AUTOMATIC);
    }

    SECTION("separator builds Gtk::Separator")
    {
      auto doc = LayoutDocument{};
      doc.root.type = "separator";
      doc.root.props["orientation"] = LayoutValue{std::string{"vertical"}};

      auto const compPtr = layoutRuntime.build(ctx, doc);
      auto* const sep = dynamic_cast<Gtk::Separator*>(&compPtr->widget());

      REQUIRE(sep != nullptr);
      CHECK(sep->get_orientation() == Gtk::Orientation::VERTICAL);
    }

    SECTION("tabs with children builds Gtk::Stack")
    {
      auto doc = LayoutDocument{};
      doc.root.type = "tabs";

      auto c1 = LayoutNode{};
      c1.type = "spacer";
      c1.id = "tab1";
      c1.layout["title"] = LayoutValue{std::string{"First Tab"}};
      doc.root.children.push_back(c1);

      auto c2 = LayoutNode{};
      c2.type = "spacer";
      c2.id = "tab2";
      c2.layout["title"] = LayoutValue{std::string{"Second Tab"}};
      doc.root.children.push_back(c2);

      auto const compPtr = layoutRuntime.build(ctx, doc);
      auto* const box = dynamic_cast<Gtk::Box*>(&compPtr->widget());

      REQUIRE(box != nullptr);

      auto* const firstChild = box->get_first_child();
      REQUIRE(firstChild != nullptr);

      auto* const stackWidget = firstChild->get_next_sibling();
      REQUIRE(stackWidget != nullptr);

      auto* const stack = dynamic_cast<Gtk::Stack*>(stackWidget);
      REQUIRE(stack != nullptr);

      auto* const stackChild = stack->get_first_child();
      CHECK(stackChild != nullptr);
    }

    SECTION("tabs child without id uses type as tab name")
    {
      auto doc = LayoutDocument{};
      doc.root.type = "tabs";

      auto c1 = LayoutNode{};
      c1.type = "spacer";
      c1.layout["title"] = LayoutValue{std::string{"Spacer Tab"}};
      doc.root.children.push_back(c1);

      auto const compPtr = layoutRuntime.build(ctx, doc);

      REQUIRE(compPtr != nullptr);

      auto* const box = dynamic_cast<Gtk::Box*>(&compPtr->widget());
      REQUIRE(box != nullptr);
    }
  }

  // ---------------------------------------------------------------------------
  // applyCommonProps
  // ---------------------------------------------------------------------------
  TEST_CASE("applyCommonProps coverage", "[layout][unit][containers]")
  {
    auto const appPtr = Gtk::Application::create("io.github.aobus.layout_test");

    auto const tempDir = TempDir{};
    auto runtime = makeRuntime(tempDir);

    auto registry = ComponentRegistry{};
    LayoutRuntime::registerStandardComponents(registry);

    auto window = Gtk::Window{};
    auto const actionRegistry = ActionRegistry{};
    auto ctx =
      LayoutContext{.registry = registry, .actionRegistry = actionRegistry, .runtime = runtime, .parentWindow = window};

    auto layoutRuntime = LayoutRuntime{registry};

    SECTION("hexpand/vexpand applied to child")
    {
      auto doc = LayoutDocument{};
      doc.root.type = "box";

      auto child = LayoutNode{};
      child.type = "spacer";
      child.layout["hexpand"] = LayoutValue{true};
      child.layout["vexpand"] = LayoutValue{false};
      doc.root.children.push_back(child);

      auto const compPtr = layoutRuntime.build(ctx, doc);
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

      auto const compPtr = layoutRuntime.build(ctx, doc);
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

      auto const compPtr = layoutRuntime.build(ctx, doc);
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

      auto const compPtr = layoutRuntime.build(ctx, doc);
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

      auto const compPtr = layoutRuntime.build(ctx, doc);
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
  }

  // ---------------------------------------------------------------------------
  // LayoutHost
  // ---------------------------------------------------------------------------
  TEST_CASE("LayoutHost rebuild", "[layout][unit][containers]")
  {
    auto const appPtr = Gtk::Application::create("io.github.aobus.layout_test");

    auto const tempDir = TempDir{};
    auto runtime = makeRuntime(tempDir);

    auto registry = ComponentRegistry{};
    registerContainerComponents(registry);

    auto window = Gtk::Window{};
    auto const actionRegistry = ActionRegistry{};
    auto ctx =
      LayoutContext{.registry = registry, .actionRegistry = actionRegistry, .runtime = runtime, .parentWindow = window};

    auto host = LayoutHost{registry};

    SECTION("Initial layout is empty before setLayout")
    {
      CHECK(host.get_first_child() == nullptr);
    }

    SECTION("setLayout with default document populates widget")
    {
      host.setLayout(ctx, createDefaultLayout());

      auto* const child = host.get_first_child();
      REQUIRE(child != nullptr);
      CHECK(dynamic_cast<Gtk::Widget*>(child) != nullptr);
    }

    SECTION("setLayout replaces previous layout")
    {
      host.setLayout(ctx, createDefaultLayout());

      auto* const first = host.get_first_child();
      REQUIRE(first != nullptr);

      auto newDoc = LayoutDocument{};
      newDoc.root.type = "spacer";
      host.setLayout(ctx, newDoc);

      auto* const second = host.get_first_child();
      REQUIRE(second != nullptr);
      CHECK(second != first);
    }

    SECTION("Semantic components registration")
    {
      auto registry2 = ComponentRegistry{};
      LayoutRuntime::registerStandardComponents(registry2);

      auto window2 = Gtk::Window{};
      auto const tempDir2 = TempDir{};
      auto runtime = makeRuntime(tempDir2);
      auto const actionRegistry2 = ActionRegistry{};
      auto ctx2 = LayoutContext{
        .registry = registry2, .actionRegistry = actionRegistry2, .runtime = runtime, .parentWindow = window2};

      auto const node = LayoutNode{};
      REQUIRE_NOTHROW(registry2.create(ctx2, node));
    }
  }
} // namespace ao::gtk::layout::test
