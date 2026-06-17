// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "../../GtkTestSupport.h"
#include "app/linux-gtk/layout/runtime/ActionRegistry.h"
#include "app/linux-gtk/layout/runtime/ComponentRegistry.h"
#include "app/linux-gtk/layout/runtime/LayoutRuntime.h"
#include "layout/component/track/TrackDetailScope.h"
#include <ao/rt/ProjectionTypes.h>
#include <ao/rt/TrackField.h>
#include <ao/uimodel/layout/LayoutNode.h>

#include <catch2/catch_test_macros.hpp>
#include <gtkmm/application.h>
#include <gtkmm/button.h>
#include <gtkmm/enums.h>
#include <gtkmm/grid.h>
#include <gtkmm/label.h>
#include <gtkmm/window.h>
#include <sigc++/signal.h>

#include <cstdint>
#include <memory>
#include <string_view>
#include <utility>
#include <vector>

namespace ao::gtk::layout::test
{
  using namespace uimodel::layout;

  namespace
  {
    void walkWidgets(Gtk::Widget& root, auto const& visit)
    {
      visit(root);

      for (auto* child = root.get_first_child(); child != nullptr; child = child->get_next_sibling())
      {
        walkWidgets(*child, visit);
      }
    }

    template<typename WidgetT>
    WidgetT* findWidget(Gtk::Widget& root)
    {
      WidgetT* found = nullptr;

      walkWidgets(root,
                  [&](Gtk::Widget& widget)
                  {
                    if (found != nullptr)
                    {
                      return;
                    }

                    if (auto* const typed = dynamic_cast<WidgetT*>(&widget); typed != nullptr)
                    {
                      found = typed;
                    }
                  });

      return found;
    }

    class MockDetailScope final : public ITrackDetailScope
    {
    public:
      rt::TrackDetailSnapshot const& snapshot() const override { return _snapshot; }
      sigc::signal<void(rt::TrackDetailSnapshot const&)>& signalSnapshotChanged() override { return _signal; }

      void setSnapshot(rt::TrackDetailSnapshot snap)
      {
        _snapshot = std::move(snap);
        _signal.emit(_snapshot);
      }

    private:
      rt::TrackDetailSnapshot _snapshot;
      sigc::signal<void(rt::TrackDetailSnapshot const&)> _signal;
    };
  }

  TEST_CASE("TrackFieldGrid collapsible sections", "[layout][unit][components][track]")
  {
    auto const appPtr = Gtk::Application::create("io.github.aobus.collapsible_test");
    auto const tempDir = ao::test::TempDir{};
    auto runtime = ao::gtk::test::makeRuntime(tempDir);
    auto registry = ComponentRegistry{};
    LayoutRuntime::registerStandardComponents(registry);

    auto window = Gtk::Window{};
    auto actionRegistry = ActionRegistry{};
    auto ctx =
      LayoutContext{.registry = registry, .actionRegistry = actionRegistry, .runtime = runtime, .parentWindow = window};

    auto scope = MockDetailScope{};
    ctx.track.detailScope = &scope;

    // Set up a snapshot with some metadata and technical fields
    auto snap = rt::TrackDetailSnapshot{};
    snap.trackIds = {TrackId{1}};
    rt::trackFieldArrayAt(snap.fields, rt::TrackField::Title).optValue = std::string{"Test Title"};
    rt::trackFieldArrayAt(snap.fields, rt::TrackField::SampleRate).optValue = std::uint32_t{44100};
    scope.setSnapshot(snap);

    auto const node = LayoutNode{.type = "track.fieldGrid"};
    auto const compPtr = registry.create(ctx, node);
    REQUIRE(compPtr != nullptr);
    auto& root = compPtr->widget();
    auto* const grid = findWidget<Gtk::Grid>(root);
    REQUIRE(grid != nullptr);

    auto allocate = [&](std::int32_t const width, std::int32_t const height)
    {
      auto minWidth = 0;
      auto natWidth = 0;
      auto minHeight = 0;
      auto natHeight = 0;
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

    SECTION("Toggling sections changes visibility")
    {
      auto* techHeader = findHeaderByClass("ao-track-detail-section-tech");
      REQUIRE(techHeader != nullptr);

      // Expand technical
      ::g_signal_emit_by_name(techHeader->gobj(), "clicked");

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
      ::g_signal_emit_by_name(metaHeader->gobj(), "clicked");

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

        auto keyLeft = 0;
        auto keyTop = 0;
        auto keyWidth = 0;
        auto keyHeight = 0;
        grid->query_child(*labelSlot, keyLeft, keyTop, keyWidth, keyHeight);

        for (auto* child = grid->get_first_child(); child != nullptr; child = child->get_next_sibling())
        {
          auto left = 0;
          auto top = 0;
          auto width = 0;
          auto height = 0;
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
      ::g_signal_emit_by_name(techHeader->gobj(), "clicked");
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

      ::g_signal_emit_by_name(metaHeader->gobj(), "clicked");
      ::g_signal_emit_by_name(customHeader->gobj(), "clicked");
      ao::gtk::test::drainGtkEvents();
      allocate(320, 1000);

      CHECK(metaHeader->get_width() == expandedMetaWidth);
      CHECK(customHeader->get_width() == expandedCustomWidth);
      CHECK(techHeader->get_width() == expandedTechWidth);
    }

    SECTION("Empty sections suppress headers")
    {
      // Create a grid with no requested categories
      auto const emptyNode =
        LayoutNode{.type = "track.fieldGrid", .props = {{"categories", LayoutValue{std::vector<std::string>{}}}}};
      auto const emptyCompPtr = registry.create(ctx, emptyNode);
      REQUIRE(emptyCompPtr != nullptr);
      auto& emptyRoot = emptyCompPtr->widget();

      auto findHeaderInByClass = [&](Gtk::Widget& root, std::string_view className) -> Gtk::Button*
      {
        Gtk::Button* found = nullptr;
        walkWidgets(root,
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

      auto* metaHeader = findHeaderInByClass(emptyRoot, "ao-track-detail-section-meta");
      auto* techHeader = findHeaderInByClass(emptyRoot, "ao-track-detail-section-tech");
      CHECK(metaHeader == nullptr);
      CHECK(techHeader == nullptr);
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

        auto keyLeft = 0;
        auto keyTop = 0;
        auto keyWidth = 0;
        auto keyHeight = 0;
        grid->query_child(*labelSlot, keyLeft, keyTop, keyWidth, keyHeight);

        for (auto* child = grid->get_first_child(); child != nullptr; child = child->get_next_sibling())
        {
          auto left = 0;
          auto top = 0;
          auto width = 0;
          auto height = 0;
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
        auto minimum = 0;
        auto natural = 0;
        auto minimumBaseline = -1;
        auto naturalBaseline = -1;
        widget.measure(Gtk::Orientation::VERTICAL, width, minimum, natural, minimumBaseline, naturalBaseline);
        return minimum;
      };

      allocate(316, 320);
      CHECK(measureMinimumHeight(*wrapper, -1) == 0);
      auto const [titleKeySlot, titleValueSlot] = findSlotsForLabel("Title");
      REQUIRE(titleValueSlot != nullptr);
      auto const keyWidthBeforeCollapse = titleKeySlot->get_width();
      auto valueLeftBeforeCollapse = 0.0;
      auto valueTopBeforeCollapse = 0.0;
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
      ::g_signal_emit_by_name(customHeader->gobj(), "clicked");
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
      auto valueLeftAfterCollapse = 0.0;
      auto valueTopAfterCollapse = 0.0;
      REQUIRE(titleValueSlot->translate_coordinates(*grid, 0.0, 0.0, valueLeftAfterCollapse, valueTopAfterCollapse));
      CHECK(valueLeftAfterCollapse == valueLeftBeforeCollapse);
      CHECK(titleValueSlot->get_width() == valueWidthBeforeCollapse);
    }
  }
}
