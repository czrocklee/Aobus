// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "app/linux-gtk/layout/components/Containers.h"

#include "../../GtkTestSupport.h"
#include "app/linux-gtk/layout/runtime/ComponentRegistry.h"
#include "app/linux-gtk/layout/runtime/ActionRegistry.h"
#include "app/linux-gtk/layout/runtime/LayoutHost.h"
#include "app/linux-gtk/layout/runtime/LayoutRuntime.h"
#include "layout/document/LayoutDocument.h"
#include "test/unit/lmdb/TestUtils.h"
#include <ao/rt/AppRuntime.h>
#include <ao/rt/ConfigStore.h>

#include <catch2/catch_test_macros.hpp>
#include <gtkmm/application.h>
#include <gtkmm/box.h>
#include <gtkmm/centerbox.h>
#include <gtkmm/enums.h>
#include <gtkmm/label.h>
#include <gtkmm/paned.h>
#include <gtkmm/scrolledwindow.h>
#include <gtkmm/separator.h>
#include <gtkmm/stack.h>
#include <gtkmm/window.h>

#include <cstdint>
#include <memory>
#include <vector>

namespace ao::gtk::layout::test
{
  using ao::gtk::test::ImmediateExecutor;

  namespace
  {
    using namespace ao::lmdb::test;
  } // namespace

  TEST_CASE("LayoutRuntime building", "[layout][unit][containers]")
  {
    auto const app = Gtk::Application::create("io.github.aobus.layout_test");

    auto const tempDir = TempDir{};
    auto const configStore = std::make_shared<rt::ConfigStore>(std::filesystem::path{tempDir.path()} / "config.yaml");

    auto runtime = rt::AppRuntime{
      rt::AppRuntimeDependencies{.executor = std::make_unique<ImmediateExecutor>(),
                                 .musicRoot = tempDir.path(),
                                 .databasePath = std::filesystem::path{tempDir.path()} / ".aobus" / "library",
                                 .workspaceConfigStore = configStore}};

    auto registry = ComponentRegistry{};
    LayoutRuntime::registerStandardComponents(registry);

    auto window = Gtk::Window{};
    auto const actionRegistry = ActionRegistry{};
    auto ctx = LayoutContext{.registry = registry, .actionRegistry = actionRegistry, .runtime = runtime, .parentWindow = window};

    auto layoutRuntime = LayoutRuntime{registry};

    SECTION("Build default layout")
    {
      auto const doc = createDefaultLayout();
      auto const rootComponent = layoutRuntime.build(ctx, doc);

      REQUIRE(rootComponent != nullptr);

      auto& widget = rootComponent->widget();
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

      auto const rootComponent = layoutRuntime.build(ctx, doc);

      REQUIRE(rootComponent != nullptr);

      auto& widget = rootComponent->widget();
      auto* const box = dynamic_cast<Gtk::Box*>(&widget);
      REQUIRE(box != nullptr);
      CHECK(box->get_orientation() == Gtk::Orientation::HORIZONTAL);
    }

    SECTION("Unknown component type produces error label")
    {
      auto doc = LayoutDocument{};
      doc.root.type = "nonexistent.component";

      auto const rootComponent = layoutRuntime.build(ctx, doc);

      REQUIRE(rootComponent != nullptr);

      auto& widget = rootComponent->widget();
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

      auto const rootComponent = layoutRuntime.build(ctx, doc);

      REQUIRE(rootComponent != nullptr);
      auto* const box = dynamic_cast<Gtk::Box*>(&rootComponent->widget());
      REQUIRE(box != nullptr);
      CHECK(box->has_css_class("ao-test-class"));
    }

    SECTION("Playback bar groups carry ao-grouping-region (direct template)")
    {
      auto const templates = getBuiltInTemplates();
      auto const& barTemplate = templates.at("playback.defaultBar");
      auto const barComp = ctx.registry.create(ctx, barTemplate);

      REQUIRE(barComp != nullptr);
      auto* const barBox = dynamic_cast<Gtk::Box*>(&barComp->widget());
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
      auto const fullLayout = layoutRuntime.build(ctx, doc);

      REQUIRE(fullLayout != nullptr);
      // Find the playback row child within the root box.
      auto* const rootBox = dynamic_cast<Gtk::Box*>(&fullLayout->widget());
      REQUIRE(rootBox != nullptr);

      // Child order: 0=menuBar, 1=playback-row (expanded template), 2=split, 3=status region
      auto* const playbackRow = rootBox->get_first_child()->get_next_sibling();
      REQUIRE(playbackRow != nullptr);
      auto* const barBox = dynamic_cast<Gtk::Box*>(playbackRow);
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
    auto const app = Gtk::Application::create("io.github.aobus.layout_test");

    auto const tempDir = TempDir{};
    auto const configStore = std::make_shared<rt::ConfigStore>(std::filesystem::path{tempDir.path()} / "config.yaml");

    auto runtime = rt::AppRuntime{
      rt::AppRuntimeDependencies{.executor = std::make_unique<ImmediateExecutor>(),
                                 .musicRoot = tempDir.path(),
                                 .databasePath = std::filesystem::path{tempDir.path()} / ".aobus" / "library",
                                 .workspaceConfigStore = configStore}};

    auto registry = ComponentRegistry{};
    LayoutRuntime::registerStandardComponents(registry);

    auto window = Gtk::Window{};
    auto const actionRegistry = ActionRegistry{};
    auto ctx = LayoutContext{.registry = registry, .actionRegistry = actionRegistry, .runtime = runtime, .parentWindow = window};

    auto layoutRuntime = LayoutRuntime{registry};

    auto const checkError = [](ILayoutComponent& comp, std::string const& expectedFragment)
    {
      auto* const label = dynamic_cast<Gtk::Label*>(&comp.widget());
      REQUIRE(label != nullptr);
      CHECK(label->get_label().find("[Layout Error]") != std::string::npos);
      CHECK(label->get_label().find(expectedFragment) != std::string::npos);
    };

    SECTION("split with 0 children returns error")
    {
      auto doc = LayoutDocument{};
      doc.root.type = "split";
      auto const comp = layoutRuntime.build(ctx, doc);
      checkError(*comp, "2 children");
    }

    SECTION("split with 1 child returns error")
    {
      auto doc = LayoutDocument{};
      doc.root.type = "split";
      doc.root.children.push_back(LayoutNode{.type = "spacer"});
      auto const comp = layoutRuntime.build(ctx, doc);
      checkError(*comp, "2 children");
    }

    SECTION("split with 3 children returns error")
    {
      auto doc = LayoutDocument{};
      doc.root.type = "split";
      doc.root.children.push_back(LayoutNode{.type = "spacer"});
      doc.root.children.push_back(LayoutNode{.type = "spacer"});
      doc.root.children.push_back(LayoutNode{.type = "spacer"});
      auto const comp = layoutRuntime.build(ctx, doc);
      checkError(*comp, "2 children");
    }

    SECTION("scroll with 0 children returns error")
    {
      auto doc = LayoutDocument{};
      doc.root.type = "scroll";
      auto const comp = layoutRuntime.build(ctx, doc);
      checkError(*comp, "1 child");
    }

    SECTION("scroll with 2 children returns error")
    {
      auto doc = LayoutDocument{};
      doc.root.type = "scroll";
      doc.root.children.push_back(LayoutNode{.type = "spacer"});
      doc.root.children.push_back(LayoutNode{.type = "spacer"});
      auto const comp = layoutRuntime.build(ctx, doc);
      checkError(*comp, "1 child");
    }

    SECTION("tabs with 0 children returns error")
    {
      auto doc = LayoutDocument{};
      doc.root.type = "tabs";
      auto const comp = layoutRuntime.build(ctx, doc);
      checkError(*comp, "at least 1 child");
    }
  }

  // ---------------------------------------------------------------------------
  // Container success states
  // ---------------------------------------------------------------------------
  TEST_CASE("Container success states", "[layout][unit][containers]")
  {
    auto const app = Gtk::Application::create("io.github.aobus.layout_test");

    auto const tempDir = TempDir{};
    auto const configStore = std::make_shared<rt::ConfigStore>(std::filesystem::path{tempDir.path()} / "config.yaml");

    auto runtime = rt::AppRuntime{
      rt::AppRuntimeDependencies{.executor = std::make_unique<ImmediateExecutor>(),
                                 .musicRoot = tempDir.path(),
                                 .databasePath = std::filesystem::path{tempDir.path()} / ".aobus" / "library",
                                 .workspaceConfigStore = configStore}};

    auto registry = ComponentRegistry{};
    LayoutRuntime::registerStandardComponents(registry);

    auto window = Gtk::Window{};
    auto const actionRegistry = ActionRegistry{};
    auto ctx = LayoutContext{.registry = registry, .actionRegistry = actionRegistry, .runtime = runtime, .parentWindow = window};

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

      auto const comp = layoutRuntime.build(ctx, doc);
      auto* const paned = dynamic_cast<Gtk::Paned*>(&comp->widget());

      REQUIRE(paned != nullptr);

      int const expectedPos = 200;
      CHECK(paned->get_position() == expectedPos);
      CHECK(paned->get_resize_start_child() == false);
      CHECK(paned->get_shrink_end_child() == true);
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

      auto const comp = layoutRuntime.build(ctx, doc);
      auto* const cb = dynamic_cast<Gtk::CenterBox*>(&comp->widget());

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

      auto const comp = layoutRuntime.build(ctx, doc);
      auto* const sw = dynamic_cast<Gtk::ScrolledWindow*>(&comp->widget());

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

      auto const comp = layoutRuntime.build(ctx, doc);
      auto* const sw = dynamic_cast<Gtk::ScrolledWindow*>(&comp->widget());

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

      auto const comp = layoutRuntime.build(ctx, doc);
      auto* const sep = dynamic_cast<Gtk::Separator*>(&comp->widget());

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

      auto const comp = layoutRuntime.build(ctx, doc);
      auto* const box = dynamic_cast<Gtk::Box*>(&comp->widget());

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

      auto const comp = layoutRuntime.build(ctx, doc);

      REQUIRE(comp != nullptr);

      auto* const box = dynamic_cast<Gtk::Box*>(&comp->widget());
      REQUIRE(box != nullptr);
    }
  }

  // ---------------------------------------------------------------------------
  // applyCommonProps
  // ---------------------------------------------------------------------------
  TEST_CASE("applyCommonProps coverage", "[layout][unit][containers]")
  {
    auto const app = Gtk::Application::create("io.github.aobus.layout_test");

    auto const tempDir = TempDir{};
    auto const configStore = std::make_shared<rt::ConfigStore>(std::filesystem::path{tempDir.path()} / "config.yaml");

    auto runtime = rt::AppRuntime{
      rt::AppRuntimeDependencies{.executor = std::make_unique<ImmediateExecutor>(),
                                 .musicRoot = tempDir.path(),
                                 .databasePath = std::filesystem::path{tempDir.path()} / ".aobus" / "library",
                                 .workspaceConfigStore = configStore}};

    auto registry = ComponentRegistry{};
    LayoutRuntime::registerStandardComponents(registry);

    auto window = Gtk::Window{};
    auto const actionRegistry = ActionRegistry{};
    auto ctx = LayoutContext{.registry = registry, .actionRegistry = actionRegistry, .runtime = runtime, .parentWindow = window};

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

      auto const comp = layoutRuntime.build(ctx, doc);
      auto* const box = dynamic_cast<Gtk::Box*>(&comp->widget());

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

      auto const comp = layoutRuntime.build(ctx, doc);
      auto* const box = dynamic_cast<Gtk::Box*>(&comp->widget());

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

      auto const comp = layoutRuntime.build(ctx, doc);
      auto* const box = dynamic_cast<Gtk::Box*>(&comp->widget());

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

      auto const comp = layoutRuntime.build(ctx, doc);
      auto* const box = dynamic_cast<Gtk::Box*>(&comp->widget());

      REQUIRE(box != nullptr);

      auto* const spacer = box->get_first_child();
      REQUIRE(spacer != nullptr);
      CHECK(spacer->has_css_class("my-class"));
      CHECK(spacer->has_css_class("another"));
    }

    SECTION("minWidth/minHeight set size request")
    {
      auto doc = LayoutDocument{};
      doc.root.type = "box";

      auto child = LayoutNode{};
      child.type = "spacer";
      child.layout["minWidth"] = LayoutValue{static_cast<std::int64_t>(200)};
      child.layout["minHeight"] = LayoutValue{static_cast<std::int64_t>(100)};
      doc.root.children.push_back(child);

      auto const comp = layoutRuntime.build(ctx, doc);
      auto* const box = dynamic_cast<Gtk::Box*>(&comp->widget());

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
    auto const app = Gtk::Application::create("io.github.aobus.layout_test");

    auto const tempDir = TempDir{};
    auto const configStore = std::make_shared<rt::ConfigStore>(std::filesystem::path{tempDir.path()} / "config.yaml");

    auto runtime = rt::AppRuntime{
      rt::AppRuntimeDependencies{.executor = std::make_unique<ImmediateExecutor>(),
                                 .musicRoot = tempDir.path(),
                                 .databasePath = std::filesystem::path{tempDir.path()} / ".aobus" / "library",
                                 .workspaceConfigStore = configStore}};

    auto registry = ComponentRegistry{};
    registerContainerComponents(registry);

    auto window = Gtk::Window{};
    auto const actionRegistry = ActionRegistry{};
    auto ctx = LayoutContext{.registry = registry, .actionRegistry = actionRegistry, .runtime = runtime, .parentWindow = window};

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
      auto const configStore2 =
        std::make_shared<rt::ConfigStore>(std::filesystem::path{tempDir2.path()} / "config.yaml");

      auto runtime = rt::AppRuntime{
        rt::AppRuntimeDependencies{.executor = std::make_unique<ImmediateExecutor>(),
                                   .musicRoot = tempDir2.path(),
                                   .databasePath = std::filesystem::path{tempDir2.path()} / ".aobus" / "library",
                                   .workspaceConfigStore = configStore2}};
      auto const actionRegistry2 = ActionRegistry{};
      auto ctx2 = LayoutContext{.registry = registry2, .actionRegistry = actionRegistry2, .runtime = runtime, .parentWindow = window2};

      auto const node = LayoutNode{};
      REQUIRE_NOTHROW(registry2.create(ctx2, node));
    }
  }
} // namespace ao::gtk::layout::test
