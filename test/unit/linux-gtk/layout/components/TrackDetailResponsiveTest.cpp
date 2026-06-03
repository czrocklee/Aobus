// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "../../GtkTestSupport.h"
#include "app/linux-gtk/layout/document/LayoutNode.h"
#include "app/linux-gtk/layout/runtime/ActionRegistry.h"
#include "app/linux-gtk/layout/runtime/ComponentRegistry.h"
#include "app/linux-gtk/layout/runtime/LayoutRuntime.h"
#include "layout/component/track/TrackDetailScope.h"
#include "layout/component/track/TrackDetailSizing.h"
#include "test/unit/lmdb/TestUtils.h"
#include <ao/rt/ProjectionTypes.h>
#include <ao/rt/TrackField.h>

#include <catch2/catch_test_macros.hpp>
#include <glibmm/main.h>
#include <glibmm/ustring.h>
#include <gtkmm/application.h>
#include <gtkmm/button.h>
#include <gtkmm/editablelabel.h>
#include <gtkmm/enums.h>
#include <gtkmm/grid.h>
#include <gtkmm/label.h>
#include <gtkmm/window.h>
#include <pangomm/layout.h>
#include <sigc++/signal.h>

#include <algorithm>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace ao::gtk::layout::test
{
  using namespace ao::lmdb::test;
  using ao::gtk::test::makeRuntime;

  namespace
  {
    class FakeTrackDetailScope final : public ITrackDetailScope
    {
    public:
      explicit FakeTrackDetailScope(rt::TrackDetailSnapshot snap)
        : _snap{std::move(snap)}
      {
      }

      rt::TrackDetailSnapshot const& snapshot() const override { return _snap; }
      bool isEditLocked() const override { return _editLocked; }
      void setEditLocked(bool locked) override
      {
        _editLocked = locked;
        _signalEditLockChanged.emit(_editLocked);
      }

      sigc::signal<void(rt::TrackDetailSnapshot const&)>& signalSnapshotChanged() override
      {
        return _signalSnapshotChanged;
      }

      sigc::signal<void(bool)>& signalEditLockChanged() override { return _signalEditLockChanged; }

    private:
      rt::TrackDetailSnapshot _snap;
      bool _editLocked = true;
      sigc::signal<void(rt::TrackDetailSnapshot const&)> _signalSnapshotChanged;
      sigc::signal<void(bool)> _signalEditLockChanged;
    };

    void walkWidgets(Gtk::Widget& root, auto const& visit)
    {
      visit(root);

      for (auto* child = root.get_first_child(); child != nullptr; child = child->get_next_sibling())
      {
        walkWidgets(*child, visit);
      }
    }

    std::string invalidUtf8Text(std::string text)
    {
      text.push_back(static_cast<char>(0xFF));
      text += " text";
      return text;
    }

    std::string replacementText(std::string text)
    {
      text.append("\xEF\xBF\xBD", 3);
      text += " text";
      return text;
    }
  } // namespace

  TEST_CASE("TrackDetail layout mode computation", "[layout][components]")
  {
    using enum LayoutMode;

    CHECK(computeLayoutMode(299) == Standard);
    CHECK(computeLayoutMode(300) == Standard);
    CHECK(computeLayoutMode(549) == Standard);
    CHECK(computeLayoutMode(550) == Wide);
    CHECK(computeLayoutMode(1000) == Wide);
  }

  TEST_CASE("TrackDetail cover art sizing", "[layout][components]")
  {
    int const targetSize = 250;

    CHECK(coverArtSideForWidth(-1, targetSize) == targetSize);
    CHECK(coverArtSideForWidth(0, targetSize) == targetSize);
    CHECK(coverArtSideForWidth(80, targetSize) == 80);
    CHECK(coverArtSideForWidth(250, targetSize) == targetSize);
    CHECK(coverArtSideForWidth(400, targetSize) == targetSize);
    CHECK(coverArtSideForWidth(180, 0) == 0);
  }

  TEST_CASE("TrackFieldGrid responsive behavior", "[layout][components][responsive]")
  {
    auto const appPtr = Gtk::Application::create("io.github.aobus.layout_test");
    auto const tempDir = TempDir{};
    auto runtime = makeRuntime(tempDir);

    auto registry = ComponentRegistry{};
    LayoutRuntime::registerStandardComponents(registry);

    auto window = Gtk::Window{};
    auto actionRegistry = ActionRegistry{};
    auto ctx =
      LayoutContext{.registry = registry, .actionRegistry = actionRegistry, .runtime = runtime, .parentWindow = window};

    auto const node = LayoutNode{.type = "track.fieldGrid"};
    auto const compPtr = registry.create(ctx, node);

    REQUIRE(compPtr != nullptr);
    auto& wrapper = compPtr->widget();

    auto* grid = dynamic_cast<Gtk::Grid*>(wrapper.get_first_child());
    REQUIRE(grid != nullptr);

    auto flushIdle = []
    {
      auto contextPtr = Glib::MainContext::get_default();

      while (contextPtr->pending())
      {
        contextPtr->iteration(false);
      }
    };

    auto getGridStats = [&]
    {
      std::int32_t maxRow = -1;
      std::int32_t count = 0;

      for (auto* child = grid->get_first_child(); child; child = child->get_next_sibling())
      {
        std::int32_t l = 0;
        std::int32_t t = 0;
        std::int32_t w = 0;
        std::int32_t h = 0;
        grid->query_child(*child, l, t, w, h);
        maxRow = std::max(maxRow, t);
        count++;
      }

      return std::make_pair(maxRow, count);
    };

    auto allocate = [&](std::int32_t w, std::int32_t h)
    {
      std::int32_t minWidth = 0;
      std::int32_t natWidth = 0;
      std::int32_t minHeight = 0;
      std::int32_t natHeight = 0;
      wrapper.measure(Gtk::Orientation::HORIZONTAL, -1, minWidth, natWidth, minHeight, natHeight);
      wrapper.measure(Gtk::Orientation::VERTICAL, w, minHeight, natHeight, minWidth, natWidth);
      wrapper.size_allocate(Gtk::Allocation{0, 0, w, h}, -1);
    };

    auto measureWidth = [&]
    {
      std::int32_t minWidth = 0;
      std::int32_t natWidth = 0;
      std::int32_t minBaseline = -1;
      std::int32_t natBaseline = -1;
      wrapper.measure(Gtk::Orientation::HORIZONTAL, -1, minWidth, natWidth, minBaseline, natBaseline);
      return std::make_pair(minWidth, natWidth);
    };

    auto measureGridMinWidth = [&]
    {
      std::int32_t minWidth = 0;
      std::int32_t natWidth = 0;
      std::int32_t minBaseline = -1;
      std::int32_t natBaseline = -1;
      grid->measure(Gtk::Orientation::HORIZONTAL, -1, minWidth, natWidth, minBaseline, natBaseline);
      return minWidth;
    };

    SECTION("Horizontal measure is clamped to panel allocation")
    {
      CHECK(wrapper.get_request_mode() == Gtk::SizeRequestMode::HEIGHT_FOR_WIDTH);
      CHECK(wrapper.get_overflow() == Gtk::Overflow::HIDDEN);

      auto const [initialMinWidth, initialNatWidth] = measureWidth();
      CHECK(initialMinWidth == 0);
      CHECK(initialNatWidth == 0);

      allocate(320, 2000);
      auto const [allocatedMinWidth, allocatedNatWidth] = measureWidth();
      auto const gridMinWidth = measureGridMinWidth();
      CHECK(allocatedMinWidth == 0);
      CHECK(allocatedNatWidth == 0);
      CHECK(wrapper.get_width() == 320);
      CHECK(grid->get_width() == std::max(320, gridMinWidth));
    }

    SECTION("Inner grid can be allocated to the panel width")
    {
      auto const gridMinWidth = measureGridMinWidth();
      CHECK(gridMinWidth <= 220);

      auto const panelWidth = 220;
      allocate(panelWidth, 2000);

      CHECK(wrapper.get_width() == panelWidth);
      CHECK(grid->get_width() == panelWidth);
    }

    SECTION("Value cells do not drive content-based horizontal width")
    {
      bool sawKeyLabel = false;
      bool sawValueEditable = false;

      walkWidgets(*grid,
                  [&](Gtk::Widget& widget)
                  {
                    if (auto* label = dynamic_cast<Gtk::Label*>(&widget); label != nullptr)
                    {
                      std::int32_t width = -1;
                      std::int32_t height = -1;
                      label->get_size_request(width, height);

                      if (label->has_css_class("ao-property-label"))
                      {
                        sawKeyLabel = true;
                        CHECK(width == 0);
                        CHECK(height == -1);
                        CHECK(label->get_halign() == Gtk::Align::START);
                        CHECK(label->get_overflow() == Gtk::Overflow::HIDDEN);
                        CHECK(label->get_ellipsize() == Pango::EllipsizeMode::NONE);
                      }
                    }
                    else if (auto* editable = dynamic_cast<Gtk::EditableLabel*>(&widget); editable != nullptr)
                    {
                      if (!editable->has_css_class("ao-property-value"))
                      {
                        return;
                      }

                      sawValueEditable = true;
                      std::int32_t width = -1;
                      std::int32_t height = -1;
                      editable->get_size_request(width, height);

                      CHECK(width == 0);
                      CHECK(height == -1);
                      CHECK(editable->get_halign() == Gtk::Align::FILL);
                      CHECK(editable->get_overflow() == Gtk::Overflow::HIDDEN);
                      CHECK(editable->get_width_chars() == 0);
                      CHECK(editable->get_max_width_chars() == 1);
                    }
                  });

      CHECK(sawKeyLabel);
      CHECK(sawValueEditable);
    }

    SECTION("Custom row actions stay inside the panel and values ellipsize")
    {
      auto const customValue =
        std::string{"A very long custom metadata value that must end-ellipsize inside the panel"};
      auto snap = rt::TrackDetailSnapshot{};
      snap.selectionKind = rt::SelectionKind::Single;
      snap.trackIds = {TrackId{1}};
      snap.customMetadata.push_back(rt::CustomMetadataItem{
        .key = "very long custom metadata key",
        .value = {.optValue = customValue, .mixed = false},
        .presentOnAll = true,
        .presentOnAny = true,
      });

      auto scope = FakeTrackDetailScope{std::move(snap)};
      auto scopedCtx = LayoutContext{.registry = registry,
                                     .actionRegistry = actionRegistry,
                                     .runtime = runtime,
                                     .parentWindow = window,
                                     .track = {.detailScope = &scope}};
      auto const scopedCompPtr = registry.create(scopedCtx, node);

      REQUIRE(scopedCompPtr != nullptr);
      auto& scopedWrapper = scopedCompPtr->widget();
      auto* const scopedGrid = dynamic_cast<Gtk::Grid*>(scopedWrapper.get_first_child());
      REQUIRE(scopedGrid != nullptr);

      std::int32_t minWidth = 0;
      std::int32_t natWidth = 0;
      std::int32_t minHeight = 0;
      std::int32_t natHeight = 0;
      scopedWrapper.measure(Gtk::Orientation::HORIZONTAL, -1, minWidth, natWidth, minHeight, natHeight);
      scopedWrapper.measure(Gtk::Orientation::VERTICAL, 220, minHeight, natHeight, minWidth, natWidth);
      scopedWrapper.size_allocate(Gtk::Allocation{0, 0, 220, 2000}, -1);

      Gtk::Button* deleteButton = nullptr;
      Gtk::Label* customKeyLabel = nullptr;
      Gtk::EditableLabel* customValueEditable = nullptr;
      walkWidgets(*scopedGrid,
                  [&](Gtk::Widget& widget)
                  {
                    if (auto* button = dynamic_cast<Gtk::Button*>(&widget);
                        button != nullptr && button->get_tooltip_text() == "Delete Property")
                    {
                      deleteButton = button;
                    }

                    if (auto* label = dynamic_cast<Gtk::Label*>(&widget);
                        label != nullptr && label->has_css_class("ao-property-label") &&
                        label->get_text() == "very long custom metadata key")
                    {
                      customKeyLabel = label;
                    }

                    if (auto* editable = dynamic_cast<Gtk::EditableLabel*>(&widget);
                        editable != nullptr && editable->has_css_class("ao-property-value") &&
                        editable->get_text().raw() == customValue)
                    {
                      customValueEditable = editable;
                    }
                  });

      REQUIRE(deleteButton != nullptr);
      REQUIRE(deleteButton->get_parent() != nullptr);
      REQUIRE(customKeyLabel != nullptr);
      REQUIRE(customValueEditable != nullptr);
      CHECK(scopedWrapper.get_width() == 220);
      CHECK(scopedGrid->get_width() == 220);
      CHECK(deleteButton->get_parent()->get_width() == 220);
      CHECK(customKeyLabel->get_ellipsize() == Pango::EllipsizeMode::END);
      CHECK(customKeyLabel->get_max_width_chars() == 24);
      CHECK(customValueEditable->get_overflow() == Gtk::Overflow::HIDDEN);
      CHECK(customValueEditable->get_max_width_chars() == 1);
      REQUIRE(customValueEditable->get_parent() != nullptr);
      CHECK(customValueEditable->get_parent()->get_width() > 0);
    }

    SECTION("Unlock enables inline editors")
    {
      auto snap = rt::TrackDetailSnapshot{};
      snap.selectionKind = rt::SelectionKind::Single;
      snap.trackIds = {TrackId{1}};
      rt::trackFieldArrayAt(snap.fields, rt::TrackField::Title).optValue =
        rt::TrackFieldRawValue{std::in_place_type<std::string>, "editable title"};
      snap.customMetadata.push_back(rt::CustomMetadataItem{
        .key = "editable custom",
        .value = {.optValue = std::string{"editable value"}, .mixed = false},
        .presentOnAll = true,
        .presentOnAny = true,
      });

      auto scope = FakeTrackDetailScope{std::move(snap)};
      auto scopedCtx = LayoutContext{.registry = registry,
                                     .actionRegistry = actionRegistry,
                                     .runtime = runtime,
                                     .parentWindow = window,
                                     .track = {.detailScope = &scope}};
      auto const scopedCompPtr = registry.create(scopedCtx, node);

      REQUIRE(scopedCompPtr != nullptr);

      auto editors = std::vector<Gtk::EditableLabel*>{};
      walkWidgets(scopedCompPtr->widget(),
                  [&](Gtk::Widget& widget)
                  {
                    if (auto* editable = dynamic_cast<Gtk::EditableLabel*>(&widget);
                        editable != nullptr && editable->has_css_class("ao-property-editable"))
                    {
                      editors.push_back(editable);
                    }
                  });

      REQUIRE(editors.size() >= 2);

      for (auto* const editor : editors)
      {
        CHECK_FALSE(editor->get_editable());
        CHECK_FALSE(editor->has_css_class("ao-property-editable-active"));
      }

      scope.setEditLocked(false);

      for (auto* const editor : editors)
      {
        CHECK(editor->get_editable());
        CHECK(editor->has_css_class("ao-property-editable-active"));
      }

      scope.setEditLocked(true);

      for (auto* const editor : editors)
      {
        CHECK_FALSE(editor->get_editable());
        CHECK_FALSE(editor->has_css_class("ao-property-editable-active"));
      }
    }

    SECTION("Invalid UTF-8 metadata is made valid before display")
    {
      auto const invalidTitle = invalidUtf8Text("title");
      auto const invalidKey = invalidUtf8Text("key");
      auto const invalidValue = invalidUtf8Text("value");
      auto snap = rt::TrackDetailSnapshot{};
      snap.selectionKind = rt::SelectionKind::Single;
      snap.trackIds = {TrackId{1}};
      rt::trackFieldArrayAt(snap.fields, rt::TrackField::Title).optValue =
        rt::TrackFieldRawValue{std::in_place_type<std::string>, invalidTitle};
      snap.customMetadata.push_back(rt::CustomMetadataItem{
        .key = invalidKey,
        .value = {.optValue = invalidValue, .mixed = false},
        .presentOnAll = true,
        .presentOnAny = true,
      });

      auto scope = FakeTrackDetailScope{std::move(snap)};
      auto scopedCtx = LayoutContext{.registry = registry,
                                     .actionRegistry = actionRegistry,
                                     .runtime = runtime,
                                     .parentWindow = window,
                                     .track = {.detailScope = &scope}};
      auto const scopedCompPtr = registry.create(scopedCtx, node);

      REQUIRE(scopedCompPtr != nullptr);

      auto const expectedTitle = replacementText("title");
      auto const expectedKey = replacementText("key");
      auto const expectedValue = replacementText("value");
      auto sawTitle = false;
      auto sawKey = false;
      auto sawValue = false;

      walkWidgets(scopedCompPtr->widget(),
                  [&](Gtk::Widget& widget)
                  {
                    if (auto* label = dynamic_cast<Gtk::Label*>(&widget); label != nullptr &&
                                                                          label->has_css_class("ao-property-label") &&
                                                                          label->get_text() == expectedKey)
                    {
                      sawKey = true;
                    }

                    if (auto* editable = dynamic_cast<Gtk::EditableLabel*>(&widget);
                        editable != nullptr && editable->has_css_class("ao-property-value"))
                    {
                      auto const text = editable->get_text().raw();

                      if (text == expectedTitle)
                      {
                        sawTitle = true;
                      }

                      if (text == expectedValue)
                      {
                        sawValue = true;
                      }
                    }
                  });

      CHECK(sawTitle);
      CHECK(sawKey);
      CHECK(sawValue);
    }

    SECTION("Transitions between modes")
    {
      allocate(200, 2000);
      flushIdle();
      auto const [compactMaxRow, compactCount] = getGridStats();

      allocate(400, 2000);
      flushIdle();
      auto const [standardMaxRow, standardCount] = getGridStats();

      allocate(800, 2000);
      flushIdle();
      auto const [wideMaxRow, wideCount] = getGridStats();

      CHECK(compactMaxRow == standardMaxRow);
      CHECK(compactCount == standardCount);
      CHECK(standardMaxRow > wideMaxRow);
      CHECK(standardCount == wideCount);
    }
  }
} // namespace ao::gtk::layout::test
