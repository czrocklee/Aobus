// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include <app/linux-gtk/layout/components/Containers.h>
#include <app/linux-gtk/layout/document/LayoutDocument.h>
#include <app/linux-gtk/layout/runtime/ComponentRegistry.h>
#include <app/linux-gtk/layout/runtime/LayoutHost.h>
#include <app/linux-gtk/layout/runtime/LayoutRuntime.h>

#include <app/runtime/AppSession.h>
#include <app/runtime/ConfigStore.h>

#include <catch2/catch_test_macros.hpp>
#include <gtkmm/application.h>
#include <gtkmm/box.h>
#include <gtkmm/label.h>
#include <gtkmm/paned.h>
#include <gtkmm/scrolledwindow.h>
#include <gtkmm/stack.h>
#include <gtkmm/window.h>

#include <test/unit/lmdb/TestUtils.h>

using namespace ao::gtk::layout;

namespace
{
  class MockExecutor final : public ao::rt::IControlExecutor
  {
  public:
    bool isCurrent() const noexcept override { return true; }
    void dispatch(std::move_only_function<void()> task) override { task(); }
    void defer(std::move_only_function<void()> task) override { task(); }
  };
} // namespace

// ---------------------------------------------------------------------------
// LayoutRuntime building
// ---------------------------------------------------------------------------

TEST_CASE("LayoutRuntime building", "[layout][containers]")
{
  auto const app = Gtk::Application::create("io.github.aobus.layout_test");

  auto const tempDir = TempDir{};
  auto const executor = std::make_shared<MockExecutor>();
  auto const configStore = std::make_shared<ao::rt::ConfigStore>(std::filesystem::path{tempDir.path()} / "config.yaml");

  auto session = ao::rt::AppSession{
    ao::rt::AppSessionDependencies{.executor = executor, .libraryRoot = tempDir.path(), .configStore = configStore}};

  auto registry = ComponentRegistry{};
  LayoutRuntime::registerStandardComponents(registry);

  auto window = Gtk::Window{};
  auto ctx = LayoutDependencies{.registry = registry, .session = session, .parentWindow = window, .onNodeMoved = {}};

  auto runtime = LayoutRuntime{registry};

  SECTION("Build default layout")
  {
    auto const doc = createDefaultLayout();
    auto const rootComponent = runtime.build(ctx, doc);

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

    auto const rootComponent = runtime.build(ctx, doc);

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

    auto const rootComponent = runtime.build(ctx, doc);

    REQUIRE(rootComponent != nullptr);

    auto& widget = rootComponent->widget();
    auto* const label = dynamic_cast<Gtk::Label*>(&widget);
    REQUIRE(label != nullptr);
    CHECK(label->get_label().find("[Layout Error]") != std::string::npos);
    CHECK(label->get_label().find("nonexistent.component") != std::string::npos);
  }
}

// ---------------------------------------------------------------------------
// Container error states
// ---------------------------------------------------------------------------

TEST_CASE("Container error states", "[layout][containers]")
{
  auto const app = Gtk::Application::create("io.github.aobus.layout_test");

  auto const tempDir = TempDir{};
  auto const executor = std::make_shared<MockExecutor>();
  auto const configStore = std::make_shared<ao::rt::ConfigStore>(std::filesystem::path{tempDir.path()} / "config.yaml");

  auto session = ao::rt::AppSession{
    ao::rt::AppSessionDependencies{.executor = executor, .libraryRoot = tempDir.path(), .configStore = configStore}};

  auto registry = ComponentRegistry{};
  LayoutRuntime::registerStandardComponents(registry);

  auto window = Gtk::Window{};
  auto ctx = LayoutDependencies{.registry = registry, .session = session, .parentWindow = window, .onNodeMoved = {}};

  auto runtime = LayoutRuntime{registry};

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
    auto const comp = runtime.build(ctx, doc);
    checkError(*comp, "2 children");
  }

  SECTION("split with 1 child returns error")
  {
    auto doc = LayoutDocument{};
    doc.root.type = "split";
    doc.root.children.push_back(LayoutNode{.type = "spacer"});
    auto const comp = runtime.build(ctx, doc);
    checkError(*comp, "2 children");
  }

  SECTION("split with 3 children returns error")
  {
    auto doc = LayoutDocument{};
    doc.root.type = "split";
    doc.root.children.push_back(LayoutNode{.type = "spacer"});
    doc.root.children.push_back(LayoutNode{.type = "spacer"});
    doc.root.children.push_back(LayoutNode{.type = "spacer"});
    auto const comp = runtime.build(ctx, doc);
    checkError(*comp, "2 children");
  }

  SECTION("scroll with 0 children returns error")
  {
    auto doc = LayoutDocument{};
    doc.root.type = "scroll";
    auto const comp = runtime.build(ctx, doc);
    checkError(*comp, "1 child");
  }

  SECTION("scroll with 2 children returns error")
  {
    auto doc = LayoutDocument{};
    doc.root.type = "scroll";
    doc.root.children.push_back(LayoutNode{.type = "spacer"});
    doc.root.children.push_back(LayoutNode{.type = "spacer"});
    auto const comp = runtime.build(ctx, doc);
    checkError(*comp, "1 child");
  }

  SECTION("tabs with 0 children returns error")
  {
    auto doc = LayoutDocument{};
    doc.root.type = "tabs";
    auto const comp = runtime.build(ctx, doc);
    checkError(*comp, "at least 1 child");
  }
}

// ---------------------------------------------------------------------------
// Container success states
// ---------------------------------------------------------------------------

TEST_CASE("Container success states", "[layout][containers]")
{
  auto const app = Gtk::Application::create("io.github.aobus.layout_test");

  auto const tempDir = TempDir{};
  auto const executor = std::make_shared<MockExecutor>();
  auto const configStore = std::make_shared<ao::rt::ConfigStore>(std::filesystem::path{tempDir.path()} / "config.yaml");

  auto session = ao::rt::AppSession{
    ao::rt::AppSessionDependencies{.executor = executor, .libraryRoot = tempDir.path(), .configStore = configStore}};

  auto registry = ComponentRegistry{};
  LayoutRuntime::registerStandardComponents(registry);

  auto window = Gtk::Window{};
  auto ctx = LayoutDependencies{.registry = registry, .session = session, .parentWindow = window, .onNodeMoved = {}};

  auto runtime = LayoutRuntime{registry};

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

    auto const comp = runtime.build(ctx, doc);
    auto* const paned = dynamic_cast<Gtk::Paned*>(&comp->widget());

    REQUIRE(paned != nullptr);

    int const expectedPos = 200;
    CHECK(paned->get_position() == expectedPos);
    CHECK(paned->get_resize_start_child() == false);
    CHECK(paned->get_shrink_end_child() == true);
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

    auto const comp = runtime.build(ctx, doc);
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

    auto const comp = runtime.build(ctx, doc);
    auto* const sw = dynamic_cast<Gtk::ScrolledWindow*>(&comp->widget());

    REQUIRE(sw != nullptr);

    auto hpolicy = Gtk::PolicyType::NEVER;
    auto vpolicy = Gtk::PolicyType::NEVER;
    sw->get_policy(hpolicy, vpolicy);

    CHECK(hpolicy == Gtk::PolicyType::AUTOMATIC);
    CHECK(vpolicy == Gtk::PolicyType::AUTOMATIC);
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

    auto const comp = runtime.build(ctx, doc);
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

    auto const comp = runtime.build(ctx, doc);

    REQUIRE(comp != nullptr);

    auto* const box = dynamic_cast<Gtk::Box*>(&comp->widget());
    REQUIRE(box != nullptr);
  }
}

// ---------------------------------------------------------------------------
// applyCommonProps
// ---------------------------------------------------------------------------

TEST_CASE("applyCommonProps coverage", "[layout][containers]")
{
  auto const app = Gtk::Application::create("io.github.aobus.layout_test");

  auto const tempDir = TempDir{};
  auto const executor = std::make_shared<MockExecutor>();
  auto const configStore = std::make_shared<ao::rt::ConfigStore>(std::filesystem::path{tempDir.path()} / "config.yaml");

  auto session = ao::rt::AppSession{
    ao::rt::AppSessionDependencies{.executor = executor, .libraryRoot = tempDir.path(), .configStore = configStore}};

  auto registry = ComponentRegistry{};
  LayoutRuntime::registerStandardComponents(registry);

  auto window = Gtk::Window{};
  auto ctx = LayoutDependencies{.registry = registry, .session = session, .parentWindow = window, .onNodeMoved = {}};

  auto runtime = LayoutRuntime{registry};

  SECTION("hexpand/vexpand applied to child")
  {
    auto doc = LayoutDocument{};
    doc.root.type = "box";

    auto child = LayoutNode{};
    child.type = "spacer";
    child.layout["hexpand"] = LayoutValue{true};
    child.layout["vexpand"] = LayoutValue{false};
    doc.root.children.push_back(child);

    auto const comp = runtime.build(ctx, doc);
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

    auto const comp = runtime.build(ctx, doc);
    auto* const box = dynamic_cast<Gtk::Box*>(&comp->widget());

    REQUIRE(box != nullptr);

    auto* const spacer = box->get_first_child();
    REQUIRE(spacer != nullptr);
    CHECK(spacer->get_halign() == Gtk::Align::CENTER);
    CHECK(spacer->get_valign() == Gtk::Align::END);
  }

  SECTION("margin applied to child")
  {
    auto doc = LayoutDocument{};
    doc.root.type = "box";

    auto child = LayoutNode{};
    child.type = "spacer";
    child.layout["margin"] = LayoutValue{static_cast<std::int64_t>(10)};
    doc.root.children.push_back(child);

    auto const comp = runtime.build(ctx, doc);
    auto* const box = dynamic_cast<Gtk::Box*>(&comp->widget());

    REQUIRE(box != nullptr);

    auto* const spacer = box->get_first_child();
    REQUIRE(spacer != nullptr);

    int const expectedMargin = 10;
    CHECK(spacer->get_margin_top() == expectedMargin);
    CHECK(spacer->get_margin_bottom() == expectedMargin);
    CHECK(spacer->get_margin_start() == expectedMargin);
    CHECK(spacer->get_margin_end() == expectedMargin);
  }

  SECTION("visible=false hides child")
  {
    auto doc = LayoutDocument{};
    doc.root.type = "box";

    auto child = LayoutNode{};
    child.type = "spacer";
    child.layout["visible"] = LayoutValue{false};
    doc.root.children.push_back(child);

    auto const comp = runtime.build(ctx, doc);
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

    auto const comp = runtime.build(ctx, doc);
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

    auto const comp = runtime.build(ctx, doc);
    auto* const box = dynamic_cast<Gtk::Box*>(&comp->widget());

    REQUIRE(box != nullptr);

    auto* const spacer = box->get_first_child();
    REQUIRE(spacer != nullptr);

    int width = -1;
    int height = -1;
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

TEST_CASE("LayoutHost rebuild", "[layout][containers]")
{
  auto const app = Gtk::Application::create("io.github.aobus.layout_test");

  auto const tempDir = TempDir{};
  auto const executor = std::make_shared<MockExecutor>();
  auto const configStore = std::make_shared<ao::rt::ConfigStore>(std::filesystem::path{tempDir.path()} / "config.yaml");

  auto session = ao::rt::AppSession{
    ao::rt::AppSessionDependencies{.executor = executor, .libraryRoot = tempDir.path(), .configStore = configStore}};

  auto registry = ComponentRegistry{};
  registerContainerComponents(registry);

  auto window = Gtk::Window{};
  auto ctx = LayoutDependencies{.registry = registry, .session = session, .parentWindow = window};

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
    auto const executor2 = std::make_shared<MockExecutor>();
    auto const configStore2 =
      std::make_shared<ao::rt::ConfigStore>(std::filesystem::path{tempDir2.path()} / "config.yaml");

    auto session2 = ao::rt::AppSession{ao::rt::AppSessionDependencies{
      .executor = executor2, .libraryRoot = tempDir2.path(), .configStore = configStore2}};
    auto ctx2 =
      LayoutDependencies{.registry = registry2, .session = session2, .parentWindow = window2, .onNodeMoved = {}};

    auto const node = LayoutNode{};
    REQUIRE_NOTHROW(registry2.create(ctx2, node));
  }
}
