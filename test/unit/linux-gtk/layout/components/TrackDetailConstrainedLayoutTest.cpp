// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "../../GtkTestSupport.h"
#include "layout/component/track/TrackDetailSizing.h"
#include "layout/component/track/TrackFieldGridCustomControls.h"
#include "layout/component/track/TrackFieldGridWidgets.h"
#include "test/unit/linux-gtk/layout/LayoutTestSupport.h"
#include <ao/CoreIds.h>
#include <ao/rt/TrackField.h>
#include <ao/rt/projection/ProjectionTypes.h>
#include <ao/uimodel/layout/document/LayoutNode.h>

#include <catch2/catch_test_macros.hpp>
#include <gtkmm/application.h>
#include <gtkmm/button.h>
#include <gtkmm/entry.h>
#include <gtkmm/enums.h>
#include <gtkmm/grid.h>
#include <gtkmm/image.h>
#include <gtkmm/label.h>
#include <gtkmm/popover.h>
#include <gtkmm/scrolledwindow.h>
#include <gtkmm/widget.h>
#include <gtkmm/window.h>
#include <pangomm/layout.h>

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ao::gtk::layout::test
{
  using namespace uimodel;
  using ao::gtk::test::collectAll;
  using ao::gtk::test::emitClicked;
  using ao::gtk::test::emitGesturePressed;
  using ao::gtk::test::findButtonByLabel;
  using ao::gtk::test::findWidget;
  using ao::gtk::test::findWidgetByClass;
  using ao::gtk::test::walkWidgets;

  namespace
  {
    Gtk::Widget* findAncestorWithClass(Gtk::Widget* widget, std::string_view const className)
    {
      for (auto* current = widget; current != nullptr; current = current->get_parent())
      {
        if (current->has_css_class(std::string{className}))
        {
          return current;
        }
      }

      return nullptr;
    }

    Gtk::Widget* findFirstPropertySlot(Gtk::Widget& root)
    {
      auto* const label = findWidgetByClass<Gtk::Label>(root, "ao-property-label");
      return label != nullptr ? label->get_parent() : nullptr;
    }

    Gtk::Widget* gridChildAt(Gtk::Grid& grid, std::int32_t const leftColumn, std::int32_t const topRow)
    {
      for (auto* child = grid.get_first_child(); child != nullptr; child = child->get_next_sibling())
      {
        std::int32_t left = 0;
        std::int32_t top = 0;
        std::int32_t width = 0;
        std::int32_t height = 0;
        grid.query_child(*child, left, top, width, height);

        if (left == leftColumn && top == topRow)
        {
          return child;
        }
      }

      return nullptr;
    }

    track_field_grid::DetailFieldEditor* findEditableBuiltInEditor(Gtk::Widget& root)
    {
      track_field_grid::DetailFieldEditor* result = nullptr;
      walkWidgets(root,
                  [&](Gtk::Widget& widget)
                  {
                    if (result != nullptr)
                    {
                      return;
                    }

                    auto* const editor = dynamic_cast<track_field_grid::DetailFieldEditor*>(&widget);

                    if (editor != nullptr && editor->has_css_class("ao-property-editable") &&
                        findAncestorWithClass(editor, "ao-detail-custom-row") == nullptr)
                    {
                      result = editor;
                    }
                  });
      return result;
    }
  } // namespace

  TEST_CASE("TrackDetail - cover art keeps square sizing under constrained width", "[gtk][unit][geometry]")
  {
    int const targetSize = 250;

    CHECK(coverArtSideForWidth(-1, targetSize) == targetSize);
    CHECK(coverArtSideForWidth(0, targetSize) == targetSize);
    CHECK(coverArtSideForWidth(80, targetSize) == 80);
    CHECK(coverArtSideForWidth(250, targetSize) == targetSize);
    CHECK(coverArtSideForWidth(400, targetSize) == targetSize);
    CHECK(coverArtSideForWidth(180, 0) == 0);
  }

  TEST_CASE("DetailFieldEditor - coordinates edit sessions", "[gtk][unit][track]")
  {
    auto const appPtr = Gtk::Application::create("io.github.aobus.detail_editor_test");
    auto window = Gtk::Window{};
    auto coordinator = track_field_grid::DetailEditCoordinator{window};
    auto first = track_field_grid::DetailFieldEditor{};
    auto second = track_field_grid::DetailFieldEditor{};

    first.setEditable(true);
    first.setText("first");
    second.setEditable(true);
    second.setText("second");
    coordinator.registerEditor(first);
    coordinator.registerEditor(second);

    auto& firstButton = first.editButtonForTest();
    auto& secondButton = second.editButtonForTest();
    auto& firstEntry = first.entryForTest();
    auto& secondEntry = second.entryForTest();
    CHECK(first.has_css_class("ao-detail-field-editable"));
    CHECK(firstButton.has_css_class("ao-detail-field-edit-hint"));

    emitClicked(firstButton);
    REQUIRE(first.getEditing());
    firstEntry.set_text("committed first");

    emitClicked(secondButton);
    CHECK_FALSE(first.getEditing());
    CHECK(first.getText() == "committed first");
    CHECK(second.getEditing());
    CHECK_FALSE(firstEntry.get_child_visible());
    CHECK(secondEntry.get_child_visible());

    REQUIRE(emitGesturePressed(window, 1, 1.0, 1.0));
    CHECK_FALSE(second.getEditing());
    CHECK_FALSE(secondEntry.get_child_visible());
  }

  TEST_CASE("AddCustomMetadataButton - submits popover values", "[gtk][unit][track]")
  {
    auto windowFixture = ao::gtk::test::GtkWindowFixture{};
    auto addButton = track_field_grid::AddCustomMetadataButton{};
    auto submissions = std::vector<std::pair<std::string, std::string>>{};
    addButton.signalAddRequested().connect([&submissions](std::string key, std::string value)
                                           { submissions.emplace_back(std::move(key), std::move(value)); });

    windowFixture.mount(addButton.button());
    windowFixture.present();

    emitClicked(addButton.button());
    ao::gtk::test::drainGtkEvents();

    auto* const popover = findWidget<Gtk::Popover>(addButton.button());
    REQUIRE(popover != nullptr);
    auto entries = collectAll<Gtk::Entry>(*popover);
    REQUIRE(entries.size() == 2);
    entries[0]->set_text("  Mood  ");
    entries[1]->set_text("Energetic");

    auto* const submitButton = findButtonByLabel(*popover, "Add");
    REQUIRE(submitButton != nullptr);
    emitClicked(*submitButton);

    REQUIRE(submissions.size() == 1);
    CHECK(submissions[0].first == "Mood");
    CHECK(submissions[0].second == "Energetic");

    windowFixture.unmount();
  }

  TEST_CASE("TrackFieldGrid - maintains constrained value-column geometry", "[gtk][unit][geometry]")
  {
    auto fixture = LayoutRuntimeFixture{};
    auto& registry = fixture.components();
    auto& window = fixture.window();
    auto& ctx = fixture.context();

    auto const node = LayoutNode{.type = "track.fieldGrid"};
    auto const compPtr = registry.create(ctx, node);

    REQUIRE(compPtr != nullptr);
    auto& root = compPtr->widget();

    auto* grid = findWidget<Gtk::Grid>(root);
    REQUIRE(grid != nullptr);
    auto* const wrapper = &root;
    REQUIRE(wrapper != nullptr);

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
      root.measure(Gtk::Orientation::HORIZONTAL, -1, minWidth, natWidth, minHeight, natHeight);
      root.measure(Gtk::Orientation::VERTICAL, w, minHeight, natHeight, minWidth, natWidth);
      root.size_allocate(Gtk::Allocation{0, 0, w, h}, -1);
    };

    auto measureWidth = [&]
    {
      std::int32_t minWidth = 0;
      std::int32_t natWidth = 0;
      std::int32_t minBaseline = -1;
      std::int32_t natBaseline = -1;
      root.measure(Gtk::Orientation::HORIZONTAL, -1, minWidth, natWidth, minBaseline, natBaseline);
      return std::make_pair(minWidth, natWidth);
    };

    auto measureHeight = [&](Gtk::Widget& widget, std::int32_t width)
    {
      std::int32_t minHeight = 0;
      std::int32_t natHeight = 0;
      std::int32_t minBaseline = -1;
      std::int32_t natBaseline = -1;
      widget.measure(Gtk::Orientation::VERTICAL, width, minHeight, natHeight, minBaseline, natBaseline);
      return std::make_pair(minHeight, natHeight);
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

    auto findHeaderByClass = [&](std::string_view className) -> Gtk::Button*
    { return findWidgetByClass<Gtk::Button>(*grid, className); };

    SECTION("Horizontal measure is clamped to panel allocation")
    {
      CHECK(root.get_request_mode() == Gtk::SizeRequestMode::HEIGHT_FOR_WIDTH);
      CHECK(wrapper->get_overflow() == Gtk::Overflow::HIDDEN);

      auto const [initialMinWidth, initialNatWidth] = measureWidth();
      CHECK(initialMinWidth == 0);
      CHECK(initialNatWidth == 0);

      allocate(320, 2000);
      auto const [allocatedMinWidth, allocatedNatWidth] = measureWidth();
      auto const gridMinWidth = measureGridMinWidth();
      CHECK(allocatedMinWidth == 0);
      CHECK(allocatedNatWidth == 0);
      CHECK(root.get_width() == 320);
      CHECK(gridMinWidth <= 320);
      CHECK(grid->get_width() <= 320);
    }

    SECTION("Section header rule is full-bleed with the chevron overlaid")
    {
      allocate(320, 2000);

      Gtk::Widget* headerButton = nullptr;

      for (auto* child = grid->get_first_child(); child != nullptr; child = child->get_next_sibling())
      {
        if (child->has_css_class("ao-track-detail-section-header"))
        {
          headerButton = child;
          break;
        }
      }

      REQUIRE(headerButton != nullptr);

      Gtk::Widget* line = nullptr;
      Gtk::Image* chevron = nullptr;
      Gtk::Label* headerLabel = nullptr;

      walkWidgets(*headerButton,
                  [&](Gtk::Widget& widget)
                  {
                    if (line == nullptr && widget.has_css_class("ao-track-detail-section-line"))
                    {
                      line = &widget;
                    }

                    if (chevron == nullptr)
                    {
                      chevron = dynamic_cast<Gtk::Image*>(&widget);
                    }

                    if (headerLabel == nullptr)
                    {
                      headerLabel = dynamic_cast<Gtk::Label*>(&widget);
                    }
                  });

      REQUIRE(line != nullptr);
      REQUIRE(chevron != nullptr);
      REQUIRE(headerLabel != nullptr);

      // The chevron is an overlay child, so it must not push the rule rightward: the line
      // begins at or before the chevron and spans the full header width. A regression that
      // returns the chevron to the box flow would start the line after it instead.
      auto const optLinePoint = line->compute_point(*headerButton, Gdk::Graphene::Point{0.0F, 0.0F});
      auto const optChevronPoint = chevron->compute_point(*headerButton, Gdk::Graphene::Point{0.0F, 0.0F});
      REQUIRE(optLinePoint);
      REQUIRE(optChevronPoint);
      CHECK(optLinePoint->get_x() <= optChevronPoint->get_x());
      CHECK(line->get_width() > chevron->get_width());
      CHECK(headerButton->get_height() >= headerLabel->get_height());
    }

    SECTION("Metadata collapse works without a detail scope")
    {
      auto* const metaHeader = findHeaderByClass("ao-track-detail-section-meta");
      auto* const propertySlot = findFirstPropertySlot(*grid);
      REQUIRE(metaHeader != nullptr);
      REQUIRE(propertySlot != nullptr);
      CHECK(propertySlot->get_visible());

      emitClicked(*metaHeader);
      ao::gtk::test::drainGtkEvents();

      CHECK_FALSE(propertySlot->get_visible());

      emitClicked(*metaHeader);
      ao::gtk::test::drainGtkEvents();

      CHECK(propertySlot->get_visible());
    }

    SECTION("Show empty fields control uses a fixed-height row slot")
    {
      auto* const showButton = findWidgetByClass<Gtk::Button>(*grid, "ao-detail-show-all-button");
      REQUIRE(showButton != nullptr);
      auto* const actionBox = showButton->get_parent();
      REQUIRE(actionBox != nullptr);
      auto* const showSlot = actionBox->get_parent();
      REQUIRE(showSlot != nullptr);
      CHECK(dynamic_cast<Gtk::Button*>(showSlot) == nullptr);
      CHECK(showSlot->get_overflow() == Gtk::Overflow::HIDDEN);

      allocate(320, 2000);

      CHECK(showSlot->get_height() == 28);
    }

    SECTION("Inner grid can be allocated to the panel width")
    {
      auto const gridMinWidth = measureGridMinWidth();
      CHECK(gridMinWidth <= 66);

      int const panelWidth = 66;
      allocate(panelWidth, 2000);

      CHECK(root.get_width() == panelWidth);
      CHECK(grid->get_width() <= panelWidth);
      CHECK(grid->get_width() > 0);
    }

    SECTION("Field grid leaves scrolling to the layout parent")
    {
      CHECK(dynamic_cast<Gtk::ScrolledWindow*>(&root) == nullptr);
      CHECK(dynamic_cast<Gtk::ScrolledWindow*>(grid->get_parent()) == nullptr);
      CHECK(root.get_vexpand());
      CHECK(wrapper->get_vexpand());
      CHECK(grid->get_vexpand());

      auto const [rootMinHeight, rootNatHeight] = measureHeight(root, 320);
      CHECK(rootMinHeight >= 0);
      CHECK(rootNatHeight > 0);

      auto const [wrapperOverallMinHeight, wrapperOverallNatHeight] = measureHeight(*wrapper, -1);
      CHECK(wrapperOverallMinHeight == 0);
      CHECK(wrapperOverallNatHeight == 0);

      int const expandedHeight = 600;
      allocate(320, expandedHeight);
      CHECK(root.get_height() == expandedHeight);
      CHECK(grid->get_height() > 0);
      CHECK(grid->get_height() <= expandedHeight);
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
                        CHECK(label->get_halign() == Gtk::Align::END);
                        CHECK(label->get_overflow() == Gtk::Overflow::HIDDEN);
                        CHECK(label->get_ellipsize() == Pango::EllipsizeMode::NONE);
                      }
                    }
                    else if (widget.has_css_class("ao-property-value"))
                    {
                      sawValueEditable = true;
                      std::int32_t width = -1;
                      std::int32_t height = -1;
                      widget.get_size_request(width, height);

                      CHECK(width == 0);
                      CHECK(height == -1);
                      CHECK((widget.get_halign() == Gtk::Align::FILL || widget.get_halign() == Gtk::Align::START));
                      CHECK(widget.get_overflow() == Gtk::Overflow::HIDDEN);

                      auto* const editor = dynamic_cast<track_field_grid::DetailFieldEditor*>(&widget);
                      REQUIRE(editor != nullptr);
                      auto& displayLabel = editor->displayLabelForTest();
                      CHECK(displayLabel.get_ellipsize() == Pango::EllipsizeMode::END);
                      CHECK((displayLabel.get_width_chars() == 0 || displayLabel.get_width_chars() == 2));
                      CHECK((displayLabel.get_max_width_chars() == 1 || displayLabel.get_max_width_chars() == -1));
                    }
                  });

      CHECK(sawKeyLabel);
      CHECK(sawValueEditable);
    }

    SECTION("Key column does not expand with the value column")
    {
      Gtk::Widget const* keySlot = nullptr;
      Gtk::Widget const* valueSlot = nullptr;
      std::int32_t keyRow = -1;
      bool sawSeparator = false;

      for (auto* child = grid->get_first_child(); child != nullptr; child = child->get_next_sibling())
      {
        std::int32_t left = 0;
        std::int32_t top = 0;
        std::int32_t width = 0;
        std::int32_t height = 0;
        grid->query_child(*child, left, top, width, height);

        bool const isWidthAnchor =
          child->has_css_class("ao-key-column-width-anchor") || child->has_css_class("ao-value-column-width-anchor");

        if (child->has_css_class("ao-track-detail-section-header"))
        {
          sawSeparator = true;
          CHECK_FALSE(child->get_hexpand());
        }

        if (keySlot == nullptr && left == 0 && width == 1 && !child->has_css_class("ao-track-detail-section-header") &&
            !isWidthAnchor)
        {
          keySlot = child;
          keyRow = top;
        }
      }

      REQUIRE(keySlot != nullptr);
      REQUIRE(keyRow >= 0);

      for (auto* child = grid->get_first_child(); child != nullptr; child = child->get_next_sibling())
      {
        std::int32_t left = 0;
        std::int32_t top = 0;
        std::int32_t width = 0;
        std::int32_t height = 0;
        grid->query_child(*child, left, top, width, height);

        if (left == 1 && top == keyRow)
        {
          valueSlot = child;
          break;
        }
      }

      REQUIRE(valueSlot != nullptr);
      CHECK_FALSE(keySlot->get_hexpand());
      CHECK(valueSlot->get_hexpand());
      CHECK(sawSeparator);

      allocate(320, 2000);
      auto const narrowKeyWidth = keySlot->get_width();
      auto const narrowValueWidth = valueSlot->get_width();

      allocate(800, 2000);
      CHECK(keySlot->get_width() == narrowKeyWidth);
      CHECK(valueSlot->get_width() > narrowValueWidth);
    }

    SECTION("Custom row actions stay inside the panel and values ellipsize")
    {
      auto const customValue =
        std::string{"A very long custom metadata value that must end-ellipsize inside the panel"};
      auto snap = rt::TrackDetailSnapshot{};
      snap.selectionKind = rt::SelectionKind::Single;
      snap.trackIds = {TrackId{1}};
      rt::trackFieldArrayAt(snap.fields, rt::TrackField::Title).optValue =
        rt::TrackFieldRawValue{std::in_place_type<std::string>, "reference title"};
      snap.customMetadata.push_back(rt::CustomMetadataItem{
        .key = "very long custom metadata key",
        .value = {.optValue = customValue, .mixed = false},
        .presentOnAll = true,
        .presentOnAny = true,
      });

      fixture.attachTrackDetailScope(std::move(snap));
      auto const scopedCompPtr = registry.create(ctx, node);

      REQUIRE(scopedCompPtr != nullptr);
      auto& scopedRoot = scopedCompPtr->widget();
      auto* const scopedGrid = findWidget<Gtk::Grid>(scopedRoot);
      REQUIRE(scopedGrid != nullptr);

      std::int32_t minWidth = 0;
      std::int32_t natWidth = 0;
      std::int32_t minHeight = 0;
      std::int32_t natHeight = 0;
      scopedRoot.measure(Gtk::Orientation::HORIZONTAL, -1, minWidth, natWidth, minHeight, natHeight);
      scopedRoot.measure(Gtk::Orientation::VERTICAL, 1000, minHeight, natHeight, minWidth, natWidth);
      scopedRoot.size_allocate(Gtk::Allocation{0, 0, 1000, 2000}, -1);

      auto* const deleteButton = findWidgetByClass<Gtk::Button>(*scopedGrid, "ao-detail-field-delete");
      REQUIRE(deleteButton != nullptr);
      REQUIRE(deleteButton->get_parent() != nullptr);
      auto* const customValueBox = deleteButton->get_parent();
      REQUIRE(customValueBox != nullptr);
      auto* const customValueEditor = findWidget<track_field_grid::DetailFieldEditor>(*customValueBox);
      REQUIRE(customValueEditor != nullptr);
      auto& customValueLabel = customValueEditor->displayLabelForTest();

      auto* const customValueSlot = customValueBox->get_parent();
      REQUIRE(customValueSlot != nullptr);
      std::int32_t customValueLeft = 0;
      std::int32_t customValueTop = 0;
      std::int32_t customValueWidth = 0;
      std::int32_t customValueHeight = 0;
      scopedGrid->query_child(*customValueSlot, customValueLeft, customValueTop, customValueWidth, customValueHeight);
      // Match the custom key slot by grid row so this stays independent of display text.
      auto* const customKeySlot = gridChildAt(*scopedGrid, 0, customValueTop);
      REQUIRE(customKeySlot != nullptr);
      auto* const customKeyLabel = findWidgetByClass<Gtk::Label>(*customKeySlot, "ao-property-label");
      REQUIRE(customKeyLabel != nullptr);

      auto* const builtInValueEditor = findEditableBuiltInEditor(*scopedGrid);
      REQUIRE(builtInValueEditor != nullptr);
      auto* const builtInValueBox = builtInValueEditor->get_parent();
      REQUIRE(builtInValueBox != nullptr);
      auto& builtInValueLabel = builtInValueEditor->displayLabelForTest();
      CHECK(scopedRoot.get_width() == 1000);
      CHECK(scopedGrid->get_width() <= 1000);
      CHECK(scopedGrid->get_width() > 0);
      CHECK(customKeyLabel->get_ellipsize() == Pango::EllipsizeMode::END);
      CHECK(customKeyLabel->get_max_width_chars() == 24);
      CHECK(customValueEditor->get_overflow() == Gtk::Overflow::HIDDEN);
      CHECK(customValueLabel.get_ellipsize() == Pango::EllipsizeMode::END);
      CHECK(customValueLabel.get_max_width_chars() == 1);
      REQUIRE(customValueEditor->get_parent() != nullptr);
      CHECK(customValueEditor->get_parent()->get_width() > 0);

      auto const optBuiltInPoint = builtInValueBox->compute_point(*scopedGrid, Gdk::Graphene::Point{0.0F, 0.0F});
      auto const optCustomPoint = customValueBox->compute_point(*scopedGrid, Gdk::Graphene::Point{0.0F, 0.0F});
      auto const optDeletePoint = deleteButton->compute_point(*scopedGrid, Gdk::Graphene::Point{0.0F, 0.0F});
      REQUIRE(optBuiltInPoint);
      REQUIRE(optCustomPoint);
      REQUIRE(optDeletePoint);
      CHECK(optCustomPoint->get_x() == optBuiltInPoint->get_x());
      CHECK(optDeletePoint->get_x() > optCustomPoint->get_x());
      CHECK(optDeletePoint->get_x() + deleteButton->get_width() <=
            optCustomPoint->get_x() + customValueBox->get_width());
      CHECK(customValueBox->get_width() > deleteButton->get_width());

      int const narrowPanelWidth = 120;
      scopedRoot.measure(Gtk::Orientation::VERTICAL, narrowPanelWidth, minHeight, natHeight, minWidth, natWidth);
      scopedRoot.size_allocate(Gtk::Allocation{0, 0, narrowPanelWidth, 2000}, -1);

      REQUIRE(customValueEditor->get_parent() != nullptr);
      CHECK(scopedGrid->get_width() <= narrowPanelWidth);
      CHECK(scopedGrid->get_width() > 0);
      CHECK(customValueEditor->get_width() <= customValueEditor->get_parent()->get_width());
      CHECK(builtInValueEditor->get_width() <= builtInValueEditor->get_parent()->get_width());
      CHECK(builtInValueLabel.get_width() <= builtInValueEditor->get_width());

      auto& editButton = builtInValueEditor->editButtonForTest();
      auto& entry = builtInValueEditor->entryForTest();
      emitClicked(editButton);
      scopedRoot.measure(Gtk::Orientation::VERTICAL, narrowPanelWidth, minHeight, natHeight, minWidth, natWidth);
      scopedRoot.size_allocate(Gtk::Allocation{0, 0, narrowPanelWidth, 2000}, -1);

      CHECK(entry.get_child_visible());
      CHECK(entry.get_width_chars() == 0);
      CHECK(entry.has_css_class("ao-detail-field-entry"));
      CHECK_FALSE(editButton.get_visible());
      CHECK(entry.get_width() <= builtInValueEditor->get_width());
      CHECK(entry.get_height() <= builtInValueEditor->get_height());

      REQUIRE(emitGesturePressed(window, 1, 1.0, 1.0));
      CHECK_FALSE(entry.get_child_visible());
      CHECK(editButton.get_visible());
    }

    SECTION("Resize keeps a single row per field")
    {
      allocate(200, 2000);
      auto const [narrowMaxRow, narrowCount] = getGridStats();

      allocate(400, 2000);
      auto const [mediumMaxRow, mediumCount] = getGridStats();

      allocate(800, 2000);
      auto const [largeMaxRow, largeCount] = getGridStats();

      CHECK(narrowMaxRow == mediumMaxRow);
      CHECK(narrowMaxRow == largeMaxRow);
      CHECK(narrowCount == mediumCount);
      CHECK(narrowCount == largeCount);
    }

    SECTION("Grid rows keep a fixed allocation height")
    {
      int const expectedRowHeight = 28;
      allocate(320, 2000);

      bool sawGridChild = false;

      for (auto* child = grid->get_first_child(); child != nullptr; child = child->get_next_sibling())
      {
        if (!child->get_visible())
        {
          continue;
        }

        // Skip non-standard rows (headers and zero-height column width anchors)
        bool const isSpecial = child->has_css_class("ao-track-detail-section-header") ||
                               child->has_css_class("ao-key-column-width-anchor") ||
                               child->has_css_class("ao-value-column-width-anchor") ||
                               dynamic_cast<Gtk::Button*>(child) != nullptr;

        if (isSpecial)
        {
          continue;
        }

        sawGridChild = true;
        CHECK(child->get_height() == expectedRowHeight);
      }

      CHECK(sawGridChild);
    }
  }
} // namespace ao::gtk::layout::test
