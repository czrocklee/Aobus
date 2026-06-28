// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "../../GtkTestSupport.h"
#include "test/unit/linux-gtk/layout/LayoutTestSupport.h"
#include <ao/rt/TrackField.h>
#include <ao/rt/projection/ProjectionTypes.h>
#include <ao/uimodel/layout/LayoutNode.h>

#include <catch2/catch_test_macros.hpp>
#include <gtkmm/button.h>
#include <gtkmm/enums.h>
#include <gtkmm/grid.h>
#include <gtkmm/label.h>

#include <cstdint>
#include <string_view>
#include <utility>
#include <vector>

namespace ao::gtk::layout::test
{
  using namespace uimodel::layout;
  using ao::gtk::test::emitClicked;
  using ao::gtk::test::findWidget;
  using ao::gtk::test::walkWidgets;

  TEST_CASE("TrackFieldGrid lays out collapsible metadata sections", "[gtk][unit][layout][components][track][geometry]")
  {
    auto fixture = LayoutRuntimeFixture{"io.github.aobus.collapsible_test"};
    auto& scope = fixture.attachTrackDetailScope();

    // Set up a snapshot with some metadata and technical fields
    auto snap = rt::TrackDetailSnapshot{};
    snap.trackIds = {TrackId{1}};
    rt::trackFieldArrayAt(snap.fields, rt::TrackField::Title).optValue = std::string{"Test Title"};
    rt::trackFieldArrayAt(snap.fields, rt::TrackField::SampleRate).optValue = std::uint32_t{44100};
    scope.setSnapshot(snap);

    auto const node = LayoutNode{.type = "track.fieldGrid"};
    auto const compPtr = fixture.create(node);
    REQUIRE(compPtr != nullptr);
    auto& root = compPtr->widget();
    auto* const grid = findWidget<Gtk::Grid>(root);
    REQUIRE(grid != nullptr);

    auto allocate = [&](std::int32_t const width, std::int32_t const height)
    {
      std::int32_t minWidth = 0;
      std::int32_t natWidth = 0;
      std::int32_t minHeight = 0;
      std::int32_t natHeight = 0;
      root.measure(Gtk::Orientation::HORIZONTAL, -1, minWidth, natWidth, minHeight, natHeight);
      root.measure(Gtk::Orientation::VERTICAL, width, minHeight, natHeight, minWidth, natWidth);
      root.size_allocate(Gtk::Allocation{0, 0, width, height}, -1);
    };

    auto findHeaderByClass = [&](std::string_view className) -> Gtk::Button*
    {
      Gtk::Button* found = nullptr;
      walkWidgets(*grid,
                  [&](Gtk::Widget& w)
                  {
                    if (auto* btn = dynamic_cast<Gtk::Button*>(&w); btn != nullptr)
                    {
                      if (btn->has_css_class(std::string{className}))
                      {
                        found = btn;
                      }
                    }
                  });
      return found;
    };

    auto findButtonByLabel = [&](std::string_view labelText) -> Gtk::Button*
    {
      Gtk::Button* found = nullptr;
      walkWidgets(*grid,
                  [&](Gtk::Widget& w)
                  {
                    if (found != nullptr)
                    {
                      return;
                    }

                    auto* const button = dynamic_cast<Gtk::Button*>(&w);

                    if (button == nullptr)
                    {
                      return;
                    }

                    if (auto const label = button->get_label().raw();
                        std::string_view{label.data(), label.size()} == labelText)
                    {
                      found = button;
                    }
                  });
      return found;
    };

    auto findPropertySlot = [&](std::string_view labelText) -> Gtk::Widget*
    {
      Gtk::Widget* found = nullptr;
      walkWidgets(*grid,
                  [&](Gtk::Widget& w)
                  {
                    if (found != nullptr)
                    {
                      return;
                    }

                    auto* const label = dynamic_cast<Gtk::Label*>(&w);

                    if (label == nullptr || !label->has_css_class("ao-property-label"))
                    {
                      return;
                    }

                    if (auto const text = label->get_text().raw();
                        std::string_view{text.data(), text.size()} == labelText)
                    {
                      found = label->get_parent();
                    }
                  });
      return found;
    };

    SECTION("Default states: Metadata expanded, Technical collapsed")
    {
      auto* metaHeader = findHeaderByClass("ao-track-detail-section-meta");
      auto* techHeader = findHeaderByClass("ao-track-detail-section-tech");
      REQUIRE(metaHeader != nullptr);
      REQUIRE(techHeader != nullptr);

      // Verify a metadata row is visible
      bool foundVisibleMeta = false;
      bool foundHiddenTech = false;

      walkWidgets(*grid,
                  [&](Gtk::Widget& w)
                  {
                    if (w.has_css_class("ao-property-label"))
                    {
                      if (auto* label = dynamic_cast<Gtk::Label*>(&w); label && label->get_text().raw() == "Title")
                      {
                        foundVisibleMeta = w.get_parent() && w.get_parent()->get_visible();
                      }

                      if (auto* label = dynamic_cast<Gtk::Label*>(&w);
                          label && label->get_text().raw() == "Sample Rate")
                      {
                        foundHiddenTech = w.get_parent() && !w.get_parent()->get_visible();
                      }
                    }
                  });

      CHECK(foundVisibleMeta);
      CHECK(foundHiddenTech);
    }

    SECTION("Empty metadata rows are hidden until requested")
    {
      auto* const genreSlot = findPropertySlot("Genre");
      auto* const albumSlot = findPropertySlot("Album");
      auto* const showButton = findButtonByLabel("Show empty fields");
      REQUIRE(genreSlot != nullptr);
      REQUIRE(albumSlot != nullptr);
      REQUIRE(showButton != nullptr);
      REQUIRE(showButton->get_parent() != nullptr);

      CHECK_FALSE(genreSlot->get_visible());
      CHECK_FALSE(albumSlot->get_visible());
      CHECK(showButton->get_parent()->get_visible());

      emitClicked(*showButton);
      ao::gtk::test::drainGtkEvents();

      CHECK(genreSlot->get_visible());
      CHECK(albumSlot->get_visible());
      CHECK(showButton->get_label() == "Hide empty fields");

      emitClicked(*showButton);
      ao::gtk::test::drainGtkEvents();

      CHECK_FALSE(genreSlot->get_visible());
      CHECK_FALSE(albumSlot->get_visible());
      CHECK(showButton->get_label() == "Show empty fields");
    }

    SECTION("Toggling sections changes visibility")
    {
      auto* techHeader = findHeaderByClass("ao-track-detail-section-tech");
      REQUIRE(techHeader != nullptr);

      // Expand technical
      emitClicked(*techHeader);

      bool foundVisibleTech = false;
      walkWidgets(*grid,
                  [&](Gtk::Widget& w)
                  {
                    if (w.has_css_class("ao-property-label"))
                    {
                      if (auto* label = dynamic_cast<Gtk::Label*>(&w);
                          label && label->get_text().raw() == "Sample Rate")
                      {
                        foundVisibleTech = w.get_parent() && w.get_parent()->get_visible();
                      }
                    }
                  });
      CHECK(foundVisibleTech);

      // Collapse metadata
      auto* metaHeader = findHeaderByClass("ao-track-detail-section-meta");
      REQUIRE(metaHeader != nullptr);
      emitClicked(*metaHeader);

      bool foundHiddenMeta = false;
      walkWidgets(*grid,
                  [&](Gtk::Widget& w)
                  {
                    if (w.has_css_class("ao-property-label"))
                    {
                      if (auto* label = dynamic_cast<Gtk::Label*>(&w); label && label->get_text().raw() == "Title")
                      {
                        foundHiddenMeta = w.get_parent() && !w.get_parent()->get_visible();
                      }
                    }
                  });
      CHECK(foundHiddenMeta);
    }

    SECTION("Toggling technical rows keeps the value column stable")
    {
      auto findValueSlotForLabel = [&](std::string_view text) -> Gtk::Widget*
      {
        Gtk::Widget* labelSlot = nullptr;

        walkWidgets(*grid,
                    [&](Gtk::Widget& w)
                    {
                      if (labelSlot != nullptr)
                      {
                        return;
                      }

                      auto* const label = dynamic_cast<Gtk::Label*>(&w);

                      if (label == nullptr || !label->has_css_class("ao-property-label"))
                      {
                        return;
                      }

                      if (auto const labelText = label->get_text().raw();
                          std::string_view{labelText.data(), labelText.size()} != text)
                      {
                        return;
                      }

                      labelSlot = label->get_parent();
                    });

        REQUIRE(labelSlot != nullptr);

        std::int32_t keyLeft = 0;
        std::int32_t keyTop = 0;
        std::int32_t keyWidth = 0;
        std::int32_t keyHeight = 0;
        grid->query_child(*labelSlot, keyLeft, keyTop, keyWidth, keyHeight);

        for (auto* child = grid->get_first_child(); child != nullptr; child = child->get_next_sibling())
        {
          std::int32_t left = 0;
          std::int32_t top = 0;
          std::int32_t width = 0;
          std::int32_t height = 0;
          grid->query_child(*child, left, top, width, height);

          if (left == 1 && top == keyTop)
          {
            return child;
          }
        }

        return nullptr;
      };

      allocate(320, 1000);
      auto* const titleValueSlot = findValueSlotForLabel("Title");
      REQUIRE(titleValueSlot != nullptr);
      auto const collapsedValueWidth = titleValueSlot->get_width();

      auto* const techHeader = findHeaderByClass("ao-track-detail-section-tech");
      REQUIRE(techHeader != nullptr);
      emitClicked(*techHeader);
      ao::gtk::test::drainGtkEvents();

      allocate(320, 1000);
      CHECK(titleValueSlot->get_width() == collapsedValueWidth);
    }

    SECTION("Section separators keep the panel width when all sections are collapsed")
    {
      auto* const metaHeader = findHeaderByClass("ao-track-detail-section-meta");
      auto* const customHeader = findHeaderByClass("ao-track-detail-section-custom");
      auto* const techHeader = findHeaderByClass("ao-track-detail-section-tech");
      REQUIRE(metaHeader != nullptr);
      REQUIRE(customHeader != nullptr);
      REQUIRE(techHeader != nullptr);

      allocate(320, 1000);
      auto const expandedMetaWidth = metaHeader->get_width();
      auto const expandedCustomWidth = customHeader->get_width();
      auto const expandedTechWidth = techHeader->get_width();
      REQUIRE(expandedMetaWidth > 0);
      REQUIRE(expandedCustomWidth > 0);
      REQUIRE(expandedTechWidth > 0);

      emitClicked(*metaHeader);
      emitClicked(*customHeader);
      ao::gtk::test::drainGtkEvents();
      allocate(320, 1000);

      CHECK(metaHeader->get_width() == expandedMetaWidth);
      CHECK(customHeader->get_width() == expandedCustomWidth);
      CHECK(techHeader->get_width() == expandedTechWidth);
    }

    SECTION("Custom section behavior")
    {
      auto customSnap = rt::TrackDetailSnapshot{};
      customSnap.trackIds = {TrackId{1}};
      customSnap.customMetadata.push_back({.key = "A Much Longer Custom Metadata Key", .presentOnAll = true});
      scope.setSnapshot(customSnap);

      auto findSlotsForLabel = [&](std::string_view text) -> std::pair<Gtk::Widget*, Gtk::Widget*>
      {
        Gtk::Widget* labelSlot = nullptr;

        walkWidgets(*grid,
                    [&](Gtk::Widget& w)
                    {
                      auto* const label = dynamic_cast<Gtk::Label*>(&w);

                      if (labelSlot != nullptr || label == nullptr || !label->has_css_class("ao-property-label"))
                      {
                        return;
                      }

                      if (auto const labelText = label->get_text().raw();
                          std::string_view{labelText.data(), labelText.size()} == text)
                      {
                        labelSlot = label->get_parent();
                      }
                    });

        REQUIRE(labelSlot != nullptr);

        std::int32_t keyLeft = 0;
        std::int32_t keyTop = 0;
        std::int32_t keyWidth = 0;
        std::int32_t keyHeight = 0;
        grid->query_child(*labelSlot, keyLeft, keyTop, keyWidth, keyHeight);

        for (auto* child = grid->get_first_child(); child != nullptr; child = child->get_next_sibling())
        {
          std::int32_t left = 0;
          std::int32_t top = 0;
          std::int32_t width = 0;
          std::int32_t height = 0;
          grid->query_child(*child, left, top, width, height);

          if (left == 1 && top == keyTop)
          {
            return {labelSlot, child};
          }
        }

        return {labelSlot, nullptr};
      };

      auto* const wrapper = grid->get_parent();
      REQUIRE(wrapper != nullptr);

      auto measureMinimumHeight = [](Gtk::Widget& widget, std::int32_t const width)
      {
        std::int32_t minimum = 0;
        std::int32_t natural = 0;
        std::int32_t minimumBaseline = -1;
        std::int32_t naturalBaseline = -1;
        widget.measure(Gtk::Orientation::VERTICAL, width, minimum, natural, minimumBaseline, naturalBaseline);
        return minimum;
      };

      allocate(316, 320);
      CHECK(measureMinimumHeight(*wrapper, -1) == 0);
      auto const [titleKeySlot, titleValueSlot] = findSlotsForLabel("Title");
      REQUIRE(titleValueSlot != nullptr);
      auto const keyWidthBeforeCollapse = titleKeySlot->get_width();
      double valueLeftBeforeCollapse = 0.0;
      double valueTopBeforeCollapse = 0.0;
      REQUIRE(titleValueSlot->translate_coordinates(*grid, 0.0, 0.0, valueLeftBeforeCollapse, valueTopBeforeCollapse));
      auto const valueWidthBeforeCollapse = titleValueSlot->get_width();

      auto* customHeader = findHeaderByClass("ao-track-detail-section-custom");
      REQUIRE(customHeader != nullptr);
      CHECK(customHeader->get_visible());

      bool foundCustom = false;
      walkWidgets(*grid,
                  [&](Gtk::Widget& w)
                  {
                    if (w.has_css_class("ao-property-label"))
                    {
                      if (auto* label = dynamic_cast<Gtk::Label*>(&w);
                          label && label->get_text().raw() == "A Much Longer Custom Metadata Key")
                      {
                        foundCustom = w.get_parent() && w.get_parent()->get_visible();
                      }
                    }
                  });
      CHECK(foundCustom);

      // Collapse custom
      emitClicked(*customHeader);
      ao::gtk::test::drainGtkEvents();
      allocate(316, 320);
      CHECK(measureMinimumHeight(*wrapper, -1) == 0);

      bool foundHiddenCustom = false;
      walkWidgets(*grid,
                  [&](Gtk::Widget& w)
                  {
                    if (w.has_css_class("ao-property-label"))
                    {
                      if (auto* label = dynamic_cast<Gtk::Label*>(&w);
                          label && label->get_text().raw() == "A Much Longer Custom Metadata Key")
                      {
                        foundHiddenCustom = w.get_parent() && !w.get_parent()->get_visible();
                      }
                    }
                  });
      CHECK(foundHiddenCustom);
      CHECK(titleKeySlot->get_width() == keyWidthBeforeCollapse);
      double valueLeftAfterCollapse = 0.0;
      double valueTopAfterCollapse = 0.0;
      REQUIRE(titleValueSlot->translate_coordinates(*grid, 0.0, 0.0, valueLeftAfterCollapse, valueTopAfterCollapse));
      CHECK(valueLeftAfterCollapse == valueLeftBeforeCollapse);
      CHECK(titleValueSlot->get_width() == valueWidthBeforeCollapse);
    }
  }

  TEST_CASE("TrackFieldGrid shows custom section when an empty custom selection appears",
            "[gtk][unit][layout][components][track][regression]")
  {
    auto fixture = LayoutRuntimeFixture{"io.github.aobus.custom_section_regression_test"};
    auto& scope = fixture.attachTrackDetailScope();

    auto const node = LayoutNode{.type = "track.fieldGrid"};
    auto const compPtr = fixture.create(node);
    REQUIRE(compPtr != nullptr);
    auto& root = compPtr->widget();
    auto* const grid = findWidget<Gtk::Grid>(root);
    REQUIRE(grid != nullptr);

    auto findCustomHeader = [&] -> Gtk::Button*
    {
      Gtk::Button* found = nullptr;
      walkWidgets(*grid,
                  [&](Gtk::Widget& widget)
                  {
                    if (auto* const button = dynamic_cast<Gtk::Button*>(&widget);
                        button != nullptr && button->has_css_class("ao-track-detail-section-custom"))
                    {
                      found = button;
                    }
                  });
      return found;
    };

    CHECK(findCustomHeader() == nullptr);

    auto snap = rt::TrackDetailSnapshot{};
    snap.trackIds = {TrackId{1}};
    scope.setSnapshot(snap);

    auto* const customHeader = findCustomHeader();
    REQUIRE(customHeader != nullptr);
    CHECK(customHeader->get_visible());
  }
} // namespace ao::gtk::layout::test
