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
#include <ao/Type.h>
#include <ao/library/MusicLibrary.h>
#include <ao/library/TrackBuilder.h>
#include <ao/library/TrackStore.h>
#include <ao/rt/ProjectionTypes.h>
#include <ao/rt/TrackField.h>

#include <catch2/catch_test_macros.hpp>
#include <glibmm/ustring.h>
#include <gtk/gtk.h>
#include <gtkmm/application.h>
#include <gtkmm/button.h>
#include <gtkmm/entry.h>
#include <gtkmm/enums.h>
#include <gtkmm/grid.h>
#include <gtkmm/label.h>
#include <gtkmm/scrolledwindow.h>
#include <gtkmm/separator.h>
#include <gtkmm/window.h>
#include <pangomm/layout.h>
#include <sigc++/signal.h>

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
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

      sigc::signal<void(rt::TrackDetailSnapshot const&)>& signalSnapshotChanged() override
      {
        return _signalSnapshotChanged;
      }

    private:
      rt::TrackDetailSnapshot _snap;
      sigc::signal<void(rt::TrackDetailSnapshot const&)> _signalSnapshotChanged;
    };

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
      WidgetT* result = nullptr;

      walkWidgets(root,
                  [&](Gtk::Widget& widget)
                  {
                    if (result == nullptr)
                    {
                      result = dynamic_cast<WidgetT*>(&widget);
                    }
                  });

      return result;
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

    TrackId addTrackWithNumber(rt::AppRuntime& runtime, std::string_view const title, std::uint16_t const trackNumber)
    {
      auto txn = runtime.musicLibrary().writeTransaction();
      auto writer = runtime.musicLibrary().tracks().writer(txn);

      auto builder = library::TrackBuilder::createNew();
      builder.metadata().title(std::string{title});
      builder.metadata().trackNumber(trackNumber);
      builder.metadata().totalTracks(12);
      auto const [hot, cold] =
        builder.serialize(txn, runtime.musicLibrary().dictionary(), runtime.musicLibrary().resources());
      auto const trackId = writer.createHotCold(hot, cold).first;

      txn.commit();
      return trackId;
    }

    std::optional<std::uint16_t> trackNumberFor(rt::AppRuntime& runtime, TrackId const trackId)
    {
      auto const txn = runtime.musicLibrary().readTransaction();
      auto const reader = runtime.musicLibrary().tracks().reader(txn);
      auto const optView = reader.get(trackId, library::TrackStore::Reader::LoadMode::Both);

      if (!optView)
      {
        return std::nullopt;
      }

      return optView->metadata().trackNumber();
    }

    Gtk::Widget* findEditableWithDisplayText(Gtk::Widget& root, std::string_view const text)
    {
      Gtk::Widget* result = nullptr;

      walkWidgets(root,
                  [&](Gtk::Widget& widget)
                  {
                    if (result != nullptr)
                    {
                      return;
                    }

                    auto* const label = dynamic_cast<Gtk::Label*>(&widget);

                    if (label == nullptr)
                    {
                      return;
                    }

                    auto const labelText = label->get_text();

                    if (auto const& rawText = labelText.raw(); std::string_view{rawText.data(), rawText.size()} != text)
                    {
                      return;
                    }

                    if (auto* const editor = label->get_parent();
                        editor != nullptr && editor->has_css_class("ao-property-editable"))
                    {
                      result = editor;
                    }
                  });

      return result;
    }

    void pressEditable(Gtk::Widget& editor)
    {
      auto const controllersPtr = editor.observe_controllers();
      REQUIRE(controllersPtr);

      auto emitted = false;
      auto const count = controllersPtr->get_n_items();

      for (auto i = 0U; i < count; ++i)
      {
        auto* const object = ::g_list_model_get_object(controllersPtr->gobj(), i);

        if (object == nullptr)
        {
          continue;
        }

        if (::g_type_check_instance_is_a(reinterpret_cast<GTypeInstance*>(object), ::gtk_gesture_click_get_type()) !=
            FALSE)
        {
          ::g_signal_emit_by_name(object, "pressed", 1, 1.0, 1.0);
          emitted = true;
        }

        ::g_object_unref(object);

        if (emitted)
        {
          break;
        }
      }

      REQUIRE(emitted);
    }

    bool trackHasCustomKey(rt::AppRuntime& runtime, TrackId const trackId, std::string_view const key)
    {
      auto const txn = runtime.musicLibrary().readTransaction();
      auto const reader = runtime.musicLibrary().tracks().reader(txn);
      auto const optView = reader.get(trackId, library::TrackStore::Reader::LoadMode::Both);

      if (!optView)
      {
        return false;
      }

      for (auto const& [dictId, /*value*/ _] : optView->custom())
      {
        if (runtime.musicLibrary().dictionary().get(dictId) == key)
        {
          return true;
        }
      }

      return false;
    }
  } // namespace

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

  TEST_CASE("TrackFieldGrid constrained layout behavior", "[layout][components][constrained]")
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
    auto& root = compPtr->widget();

    auto* const viewport = root.get_first_child();
    REQUIRE(viewport != nullptr);
    auto* const scrolled = findWidget<Gtk::ScrolledWindow>(root);
    REQUIRE(scrolled != nullptr);
    auto* grid = findWidget<Gtk::Grid>(root);
    REQUIRE(grid != nullptr);
    auto* wrapper = grid->get_parent();
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

    SECTION("Horizontal measure is clamped to panel allocation")
    {
      CHECK(root.get_request_mode() == Gtk::SizeRequestMode::HEIGHT_FOR_WIDTH);
      CHECK(viewport->get_request_mode() == Gtk::SizeRequestMode::HEIGHT_FOR_WIDTH);
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

    SECTION("Inner grid can be allocated to the panel width")
    {
      auto const gridMinWidth = measureGridMinWidth();
      CHECK(gridMinWidth <= 66);

      auto const panelWidth = 66;
      allocate(panelWidth, 2000);

      CHECK(root.get_width() == panelWidth);
      CHECK(grid->get_width() <= panelWidth);
      CHECK(grid->get_width() > 0);
    }

    SECTION("Field grid uses a fixed-height scroll viewport")
    {
      auto hpolicy = Gtk::PolicyType::AUTOMATIC;
      auto vpolicy = Gtk::PolicyType::NEVER;
      scrolled->get_policy(hpolicy, vpolicy);

      CHECK(hpolicy == Gtk::PolicyType::NEVER);
      CHECK(vpolicy == Gtk::PolicyType::AUTOMATIC);
      CHECK(root.get_vexpand());
      CHECK(viewport->get_vexpand());
      CHECK(scrolled->get_vexpand());
      CHECK(wrapper->get_vexpand());
      CHECK(grid->get_vexpand());
      CHECK(scrolled->get_propagate_natural_width() == false);
      CHECK(scrolled->get_propagate_natural_height() == false);

      auto const expectedViewportHeight = 280;
      auto const [viewportMinHeight, viewportNatHeight] = measureHeight(*viewport, 320);
      CHECK(viewportMinHeight == 0);
      CHECK(viewportNatHeight == expectedViewportHeight);

      auto const [rootMinHeight, rootNatHeight] = measureHeight(root, 320);
      CHECK(rootMinHeight == 0);
      CHECK(rootNatHeight == expectedViewportHeight);

      auto const expandedHeight = 600;
      allocate(320, expandedHeight);
      CHECK(viewport->get_height() == expandedHeight);
      CHECK(scrolled->get_height() == expandedHeight);
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
                      CHECK(widget.get_halign() == Gtk::Align::FILL);
                      CHECK(widget.get_overflow() == Gtk::Overflow::HIDDEN);

                      auto* const displayLabel = dynamic_cast<Gtk::Label*>(widget.get_first_child());
                      REQUIRE(displayLabel != nullptr);
                      CHECK(displayLabel->get_ellipsize() == Pango::EllipsizeMode::END);
                      CHECK((displayLabel->get_width_chars() == 0 || displayLabel->get_width_chars() == 2));
                      CHECK((displayLabel->get_max_width_chars() == 1 || displayLabel->get_max_width_chars() == -1));
                    }
                  });

      CHECK(sawKeyLabel);
      CHECK(sawValueEditable);
    }

    SECTION("Composite disc and track rows replace total rows")
    {
      auto labels = std::vector<std::string>{};

      walkWidgets(*grid,
                  [&](Gtk::Widget& widget)
                  {
                    if (auto* const label = dynamic_cast<Gtk::Label*>(&widget);
                        label != nullptr && label->has_css_class("ao-property-label"))
                    {
                      labels.push_back(label->get_text().raw());
                    }
                  });

      CHECK(std::ranges::contains(labels, std::string{"Disc"}));
      CHECK(std::ranges::contains(labels, std::string{"Track"}));
      CHECK(!std::ranges::contains(labels, std::string{"Total Discs"}));
      CHECK(!std::ranges::contains(labels, std::string{"Total Tracks"}));
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

        if (dynamic_cast<Gtk::Separator*>(child) != nullptr)
        {
          sawSeparator = true;
          CHECK_FALSE(child->get_hexpand());
        }

        if (keySlot == nullptr && left == 0 && width == 1 && dynamic_cast<Gtk::Separator*>(child) == nullptr)
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

      auto scope = FakeTrackDetailScope{std::move(snap)};
      auto scopedCtx = LayoutContext{.registry = registry,
                                     .actionRegistry = actionRegistry,
                                     .runtime = runtime,
                                     .parentWindow = window,
                                     .track = {.detailScope = &scope}};
      auto const scopedCompPtr = registry.create(scopedCtx, node);

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

      Gtk::Button* deleteButton = nullptr;
      Gtk::Label* customKeyLabel = nullptr;
      Gtk::Widget* builtInValueBox = nullptr;
      Gtk::Widget* builtInValueEditor = nullptr;
      Gtk::Label* builtInValueLabel = nullptr;
      Gtk::Widget const* customControlsBox = nullptr;
      Gtk::Widget* customValueBox = nullptr;
      Gtk::Widget* customValueEditor = nullptr;
      Gtk::Label* customValueLabel = nullptr;
      walkWidgets(
        *scopedGrid,
        [&](Gtk::Widget& widget)
        {
          if (auto* button = dynamic_cast<Gtk::Button*>(&widget);
              button != nullptr && button->get_tooltip_text() == "Delete Property")
          {
            deleteButton = button;
          }

          if (auto* label = dynamic_cast<Gtk::Label*>(&widget); label != nullptr &&
                                                                label->has_css_class("ao-property-label") &&
                                                                label->get_text() == "very long custom metadata key")
          {
            customKeyLabel = label;
          }

          if (auto* label = dynamic_cast<Gtk::Label*>(&widget); label != nullptr && label->get_text() == customValue)
          {
            if (auto* const editor = label->get_parent();
                editor != nullptr && editor->has_css_class("ao-property-value"))
            {
              customValueLabel = label;
              customValueEditor = editor;

              if (auto* const valueClip = editor->get_parent(); valueClip != nullptr)
              {
                customValueBox = valueClip->get_parent();
              }
            }
          }

          if (auto* label = dynamic_cast<Gtk::Label*>(&widget);
              label != nullptr && label->get_text() == "reference title")
          {
            if (auto* const editor = label->get_parent();
                editor != nullptr && editor->has_css_class("ao-property-value"))
            {
              builtInValueLabel = label;
              builtInValueEditor = editor;

              if (auto* const valueClip = editor->get_parent(); valueClip != nullptr)
              {
                builtInValueBox = valueClip->get_parent();
              }
            }
          }
        });

      REQUIRE(deleteButton != nullptr);
      REQUIRE(deleteButton->get_parent() != nullptr);
      REQUIRE(customKeyLabel != nullptr);
      REQUIRE(builtInValueBox != nullptr);
      REQUIRE(builtInValueEditor != nullptr);
      REQUIRE(builtInValueLabel != nullptr);
      customControlsBox = deleteButton->get_parent();
      REQUIRE(customControlsBox != nullptr);
      REQUIRE(customValueBox != nullptr);
      REQUIRE(customValueEditor != nullptr);
      REQUIRE(customValueLabel != nullptr);
      CHECK(scopedRoot.get_width() == 1000);
      CHECK(scopedGrid->get_width() <= 1000);
      CHECK(scopedGrid->get_width() > 0);
      CHECK(customKeyLabel->get_ellipsize() == Pango::EllipsizeMode::END);
      CHECK(customKeyLabel->get_max_width_chars() == 24);
      CHECK(customValueEditor->get_overflow() == Gtk::Overflow::HIDDEN);
      CHECK(customValueLabel->get_ellipsize() == Pango::EllipsizeMode::END);
      CHECK(customValueLabel->get_max_width_chars() == 1);
      REQUIRE(customValueEditor->get_parent() != nullptr);
      CHECK(customValueEditor->get_parent()->get_width() > 0);

      auto const optBuiltInPoint = builtInValueBox->compute_point(*scopedGrid, Gdk::Graphene::Point{0.0F, 0.0F});
      auto const optControlsPoint = customControlsBox->compute_point(*scopedGrid, Gdk::Graphene::Point{0.0F, 0.0F});
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

      auto const narrowPanelWidth = 120;
      scopedRoot.measure(Gtk::Orientation::VERTICAL, narrowPanelWidth, minHeight, natHeight, minWidth, natWidth);
      scopedRoot.size_allocate(Gtk::Allocation{0, 0, narrowPanelWidth, 2000}, -1);

      REQUIRE(customValueEditor->get_parent() != nullptr);
      CHECK(scopedGrid->get_width() <= narrowPanelWidth);
      CHECK(scopedGrid->get_width() > 0);
      CHECK(customValueEditor->get_width() <= customValueEditor->get_parent()->get_width());
      CHECK(builtInValueEditor->get_width() <= builtInValueEditor->get_parent()->get_width());
      CHECK(builtInValueLabel->get_width() <= builtInValueEditor->get_width());
    }

    SECTION("Partial custom metadata delete does not offer single-value undo")
    {
      auto trackId1 = TrackId{kInvalidTrackId};
      auto trackId2 = TrackId{kInvalidTrackId};

      {
        auto txn = runtime.musicLibrary().writeTransaction();
        auto writer = runtime.musicLibrary().tracks().writer(txn);

        auto builder1 = library::TrackBuilder::createNew();
        builder1.metadata().title("Track With Partial Custom");
        builder1.custom().add("partial", "value");
        auto const [hot1, cold1] =
          builder1.serialize(txn, runtime.musicLibrary().dictionary(), runtime.musicLibrary().resources());
        trackId1 = writer.createHotCold(hot1, cold1).first;

        auto builder2 = library::TrackBuilder::createNew();
        builder2.metadata().title("Track Without Partial Custom");
        auto const [hot2, cold2] =
          builder2.serialize(txn, runtime.musicLibrary().dictionary(), runtime.musicLibrary().resources());
        trackId2 = writer.createHotCold(hot2, cold2).first;

        txn.commit();
      }

      REQUIRE(trackId1 != kInvalidTrackId);
      REQUIRE(trackId2 != kInvalidTrackId);

      auto snap = rt::TrackDetailSnapshot{};
      snap.selectionKind = rt::SelectionKind::Multiple;
      snap.trackIds = {trackId1, trackId2};
      snap.customMetadata.push_back(rt::CustomMetadataItem{
        .key = "partial",
        .value = {.optValue = std::string{"value"}, .mixed = false},
        .presentOnAll = false,
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

      Gtk::Button* deleteButton = nullptr;
      Gtk::Widget* undoBar = nullptr;

      walkWidgets(scopedCompPtr->widget(),
                  [&](Gtk::Widget& widget)
                  {
                    if (auto* button = dynamic_cast<Gtk::Button*>(&widget);
                        button != nullptr && button->get_tooltip_text() == "Delete Property")
                    {
                      deleteButton = button;
                    }

                    if (widget.has_css_class("ao-undo-bar"))
                    {
                      undoBar = &widget;
                    }
                  });

      REQUIRE(deleteButton != nullptr);
      REQUIRE(undoBar != nullptr);
      REQUIRE(trackHasCustomKey(runtime, trackId1, "partial"));

      ::g_signal_emit_by_name(deleteButton->gobj(), "clicked");
      ao::gtk::test::drainGtkEvents();

      CHECK_FALSE(trackHasCustomKey(runtime, trackId1, "partial"));
      CHECK_FALSE(trackHasCustomKey(runtime, trackId2, "partial"));
      CHECK_FALSE(undoBar->get_visible());
    }

    SECTION("Editable metadata rows are field-editable without a global lock")
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

      auto editors = std::vector<Gtk::Widget*>{};
      walkWidgets(scopedCompPtr->widget(),
                  [&](Gtk::Widget& widget)
                  {
                    if (widget.has_css_class("ao-property-editable"))
                    {
                      editors.push_back(&widget);
                    }
                  });

      REQUIRE(editors.size() >= 2);

      for (auto* const editor : editors)
      {
        CHECK(editor->has_css_class("ao-property-value"));
      }

      // Verify technical fields are NOT editable
      auto technicalEditors = std::vector<Gtk::Widget*>{};
      walkWidgets(scopedCompPtr->widget(),
                  [&](Gtk::Widget& widget)
                  {
                    if (!widget.has_css_class("ao-property-editable") && widget.has_css_class("ao-property-value"))
                    {
                      technicalEditors.push_back(&widget);
                    }
                  });

      REQUIRE_FALSE(technicalEditors.empty());

      for (auto* const editor : technicalEditors)
      {
        CHECK_FALSE(editor->has_css_class("ao-property-editable"));
      }
    }

    SECTION("Composite mixed numeric fields stay compact and ignore unchanged sentinel commits")
    {
      auto const trackId1 = addTrackWithNumber(runtime, "Mixed Track 1", 1);
      auto const trackId2 = addTrackWithNumber(runtime, "Mixed Track 2", 2);
      auto snap = rt::TrackDetailSnapshot{};
      snap.selectionKind = rt::SelectionKind::Multiple;
      snap.trackIds = {trackId1, trackId2};
      rt::trackFieldArrayAt(snap.fields, rt::TrackField::TrackNumber).mixed = true;
      rt::trackFieldArrayAt(snap.fields, rt::TrackField::TotalTracks).optValue =
        rt::TrackFieldRawValue{std::in_place_type<std::uint16_t>, 12};

      auto scope = FakeTrackDetailScope{std::move(snap)};
      auto scopedCtx = LayoutContext{.registry = registry,
                                     .actionRegistry = actionRegistry,
                                     .runtime = runtime,
                                     .parentWindow = window,
                                     .track = {.detailScope = &scope}};
      auto const scopedCompPtr = registry.create(scopedCtx, node);

      REQUIRE(scopedCompPtr != nullptr);

      auto* const mixedEditor = findEditableWithDisplayText(scopedCompPtr->widget(), "-");
      REQUIRE(mixedEditor != nullptr);
      auto* const entry = findWidget<Gtk::Entry>(*mixedEditor);
      REQUIRE(entry != nullptr);

      pressEditable(*mixedEditor);
      CHECK(entry->get_visible());
      ::g_signal_emit_by_name(entry->gobj(), "activate");
      ao::gtk::test::drainGtkEvents();

      CHECK(trackNumberFor(runtime, trackId1) == std::optional<std::uint16_t>{1});
      CHECK(trackNumberFor(runtime, trackId2) == std::optional<std::uint16_t>{2});
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

                    if (auto* label = dynamic_cast<Gtk::Label*>(&widget); label != nullptr)
                    {
                      auto const text = label->get_text();

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
      auto const expectedRowHeight = 28;
      allocate(320, 2000);

      auto sawGridChild = false;

      for (auto* child = grid->get_first_child(); child != nullptr; child = child->get_next_sibling())
      {
        sawGridChild = true;

        if (dynamic_cast<Gtk::Separator*>(child) != nullptr)
        {
          continue;
        }

        CHECK(child->get_height() == expectedRowHeight);
      }

      CHECK(sawGridChild);
    }
  }
} // namespace ao::gtk::layout::test
