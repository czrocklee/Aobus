// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "../../GtkTestSupport.h"
#include "test/unit/linux-gtk/layout/LayoutTestSupport.h"
#include <ao/rt/TrackField.h>
#include <ao/rt/projection/ProjectionTypes.h>
#include <ao/uimodel/layout/document/LayoutNode.h>

#include <catch2/catch_test_macros.hpp>
#include <gtkmm/button.h>
#include <gtkmm/enums.h>
#include <gtkmm/grid.h>
#include <gtkmm/widget.h>

#include <cstdint>
#include <string>
#include <string_view>

namespace ao::gtk::layout::test
{
  using namespace uimodel;
  using ao::gtk::test::emitClicked;
  using ao::gtk::test::findWidget;
  using ao::gtk::test::walkWidgets;

  enum class TrackFieldGridSection : std::uint8_t
  {
    Metadata,
    Custom,
    Technical
  };

  struct TrackFieldGridRowSlots final
  {
    Gtk::Widget* label = nullptr;
    Gtk::Widget* value = nullptr;
  };

  class TrackFieldGridProbe final
  {
  public:
    explicit TrackFieldGridProbe(Gtk::Grid& grid)
      : _grid{grid}
    {
    }

    Gtk::Button* header(TrackFieldGridSection const section) const
    {
      switch (section)
      {
        case TrackFieldGridSection::Metadata: return findWidgetByClass<Gtk::Button>("ao-track-detail-section-meta");
        case TrackFieldGridSection::Custom: return findWidgetByClass<Gtk::Button>("ao-track-detail-section-custom");
        case TrackFieldGridSection::Technical: return findWidgetByClass<Gtk::Button>("ao-track-detail-section-tech");
      }

      return nullptr;
    }

    Gtk::Button* showEmptyFieldsButton() const { return findWidgetByClass<Gtk::Button>("ao-detail-show-all-button"); }

    TrackFieldGridRowSlots fieldRow(rt::TrackField const field) const
    {
      return {.label = findWidgetByClass<Gtk::Widget>(fieldSlotClass(field, "label")),
              .value = findWidgetByClass<Gtk::Widget>(fieldSlotClass(field, "value"))};
    }

    TrackFieldGridRowSlots customRow() const
    {
      return {.label = findWidgetByClass<Gtk::Widget>("ao-track-field-grid-custom-label-slot"),
              .value = findWidgetByClass<Gtk::Widget>("ao-track-field-grid-custom-value-slot")};
    }

  private:
    static std::string fieldSlotClass(rt::TrackField const field, std::string_view const slotName)
    {
      auto className = std::string{"ao-track-field-grid-field-"};
      className += std::string{rt::trackFieldId(field)};
      className += "-";
      className += std::string{slotName};
      className += "-slot";
      return className;
    }

    template<typename Widget>
    Widget* findWidgetByClass(std::string_view const className) const
    {
      Widget* found = nullptr;
      walkWidgets(_grid,
                  [&](Gtk::Widget& widget)
                  {
                    if (found != nullptr || !widget.has_css_class(std::string{className}))
                    {
                      return;
                    }

                    found = dynamic_cast<Widget*>(&widget);
                  });
      return found;
    }

    Gtk::Grid& _grid;
  };

  TEST_CASE("TrackFieldGrid lays out collapsible metadata sections", "[gtk][unit][geometry]")
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
    auto probe = TrackFieldGridProbe{*grid};

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

    SECTION("Default states: Metadata expanded, Technical collapsed")
    {
      auto* metaHeader = probe.header(TrackFieldGridSection::Metadata);
      auto* techHeader = probe.header(TrackFieldGridSection::Technical);
      REQUIRE(metaHeader != nullptr);
      REQUIRE(techHeader != nullptr);

      auto const titleRow = probe.fieldRow(rt::TrackField::Title);
      auto const sampleRateRow = probe.fieldRow(rt::TrackField::SampleRate);
      REQUIRE(titleRow.label != nullptr);
      REQUIRE(sampleRateRow.label != nullptr);
      CHECK(titleRow.label->get_visible());
      CHECK_FALSE(sampleRateRow.label->get_visible());
    }

    SECTION("Empty metadata rows are hidden until requested")
    {
      auto const genreRow = probe.fieldRow(rt::TrackField::Genre);
      auto const albumRow = probe.fieldRow(rt::TrackField::Album);
      auto* const showButton = probe.showEmptyFieldsButton();
      REQUIRE(genreRow.label != nullptr);
      REQUIRE(albumRow.label != nullptr);
      REQUIRE(showButton != nullptr);
      REQUIRE(showButton->get_parent() != nullptr);

      CHECK_FALSE(genreRow.label->get_visible());
      CHECK_FALSE(albumRow.label->get_visible());
      CHECK(showButton->get_parent()->get_visible());

      emitClicked(*showButton);
      ao::gtk::test::drainGtkEvents();

      CHECK(genreRow.label->get_visible());
      CHECK(albumRow.label->get_visible());
      CHECK(showButton->get_label() == "Hide empty fields");

      emitClicked(*showButton);
      ao::gtk::test::drainGtkEvents();

      CHECK_FALSE(genreRow.label->get_visible());
      CHECK_FALSE(albumRow.label->get_visible());
      CHECK(showButton->get_label() == "Show empty fields");
    }

    SECTION("Toggling sections changes visibility")
    {
      auto* techHeader = probe.header(TrackFieldGridSection::Technical);
      REQUIRE(techHeader != nullptr);
      auto const sampleRateRow = probe.fieldRow(rt::TrackField::SampleRate);
      REQUIRE(sampleRateRow.label != nullptr);
      CHECK_FALSE(sampleRateRow.label->get_visible());

      emitClicked(*techHeader);

      CHECK(sampleRateRow.label->get_visible());

      auto* metaHeader = probe.header(TrackFieldGridSection::Metadata);
      REQUIRE(metaHeader != nullptr);
      auto const titleRow = probe.fieldRow(rt::TrackField::Title);
      REQUIRE(titleRow.label != nullptr);
      CHECK(titleRow.label->get_visible());

      emitClicked(*metaHeader);

      CHECK_FALSE(titleRow.label->get_visible());
    }

    SECTION("Toggling technical rows keeps the value column stable")
    {
      allocate(320, 1000);
      auto const titleRow = probe.fieldRow(rt::TrackField::Title);
      REQUIRE(titleRow.value != nullptr);
      auto const collapsedValueWidth = titleRow.value->get_width();

      auto* const techHeader = probe.header(TrackFieldGridSection::Technical);
      REQUIRE(techHeader != nullptr);
      emitClicked(*techHeader);
      ao::gtk::test::drainGtkEvents();

      allocate(320, 1000);
      CHECK(titleRow.value->get_width() == collapsedValueWidth);
    }

    SECTION("Section separators keep the panel width when all sections are collapsed")
    {
      auto* const metaHeader = probe.header(TrackFieldGridSection::Metadata);
      auto* const customHeader = probe.header(TrackFieldGridSection::Custom);
      auto* const techHeader = probe.header(TrackFieldGridSection::Technical);
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
      auto const titleRow = probe.fieldRow(rt::TrackField::Title);
      REQUIRE(titleRow.label != nullptr);
      REQUIRE(titleRow.value != nullptr);
      auto const keyWidthBeforeCollapse = titleRow.label->get_width();
      double valueLeftBeforeCollapse = 0.0;
      double valueTopBeforeCollapse = 0.0;
      REQUIRE(titleRow.value->translate_coordinates(*grid, 0.0, 0.0, valueLeftBeforeCollapse, valueTopBeforeCollapse));
      auto const valueWidthBeforeCollapse = titleRow.value->get_width();

      auto* customHeader = probe.header(TrackFieldGridSection::Custom);
      REQUIRE(customHeader != nullptr);
      CHECK(customHeader->get_visible());

      auto const customRow = probe.customRow();
      REQUIRE(customRow.label != nullptr);
      CHECK(customRow.label->get_visible());

      // Collapse custom
      emitClicked(*customHeader);
      ao::gtk::test::drainGtkEvents();
      allocate(316, 320);
      CHECK(measureMinimumHeight(*wrapper, -1) == 0);

      CHECK_FALSE(customRow.label->get_visible());
      CHECK(titleRow.label->get_width() == keyWidthBeforeCollapse);
      double valueLeftAfterCollapse = 0.0;
      double valueTopAfterCollapse = 0.0;
      REQUIRE(titleRow.value->translate_coordinates(*grid, 0.0, 0.0, valueLeftAfterCollapse, valueTopAfterCollapse));
      CHECK(valueLeftAfterCollapse == valueLeftBeforeCollapse);
      CHECK(titleRow.value->get_width() == valueWidthBeforeCollapse);
    }
  }

  TEST_CASE("TrackFieldGrid shows custom section when an empty custom selection appears", "[gtk][unit][regression]")
  {
    auto fixture = LayoutRuntimeFixture{"io.github.aobus.custom_section_regression_test"};
    auto& scope = fixture.attachTrackDetailScope();

    auto const node = LayoutNode{.type = "track.fieldGrid"};
    auto const compPtr = fixture.create(node);
    REQUIRE(compPtr != nullptr);
    auto& root = compPtr->widget();
    auto* const grid = findWidget<Gtk::Grid>(root);
    REQUIRE(grid != nullptr);
    auto probe = TrackFieldGridProbe{*grid};

    CHECK(probe.header(TrackFieldGridSection::Custom) == nullptr);

    auto snap = rt::TrackDetailSnapshot{};
    snap.trackIds = {TrackId{1}};
    scope.setSnapshot(snap);

    auto* const customHeader = probe.header(TrackFieldGridSection::Custom);
    REQUIRE(customHeader != nullptr);
    CHECK(customHeader->get_visible());
  }
} // namespace ao::gtk::layout::test
