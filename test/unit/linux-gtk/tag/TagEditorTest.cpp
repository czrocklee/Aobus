// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "tag/TagEditor.h"

#include "../../TestUtils.h"
#include "gtkmm/button.h"
#include "gtkmm/entry.h"
#include "gtkmm/enums.h"
#include "gtkmm/label.h"
#include "gtkmm/widget.h"
#include "gtkmm/window.h"
#include "test/unit/linux-gtk/GtkTestSupport.h"
#include <ao/Type.h>
#include <ao/library/MusicLibrary.h>
#include <ao/library/TrackBuilder.h>
#include <ao/library/TrackStore.h>
#include <ao/lmdb/Transaction.h>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string>
#include <string_view>

namespace ao::gtk::test
{
  namespace
  {
    // Counts the editor's direct chip children carrying the given CSS class. Chips are direct
    // children of the editor (the flow is hand-rolled, not a GtkFlowBox), so no cell wrapper.
    std::int32_t countChipsByClass(Gtk::Widget& container, std::string_view const className)
    {
      std::int32_t count = 0;

      for (auto* child = container.get_first_child(); child != nullptr; child = child->get_next_sibling())
      {
        if (child->has_css_class(Glib::ustring{std::string{className}}))
        {
          ++count;
        }
      }

      return count;
    }

    bool emitWindowOutsideClick(Gtk::Window& window, double const x = 1.0, double const y = 1.0)
    {
      return emitGesturePressed(window, 1, x, y);
    }

    // Returns the first suggested chip among the editor's direct children, if present.
    Gtk::Widget* firstSuggestedChip(Gtk::Widget& container)
    {
      for (auto* child = container.get_first_child(); child != nullptr; child = child->get_next_sibling())
      {
        if (child->has_css_class("ao-tag-chip-suggested"))
        {
          return child;
        }
      }

      return nullptr;
    }

    // Returns the current chip whose label matches `text`, searching the editor's direct children.
    Gtk::Widget* currentChipWithLabel(Gtk::Widget& container, std::string_view const text)
    {
      for (auto* child = container.get_first_child(); child != nullptr; child = child->get_next_sibling())
      {
        if (!child->has_css_class("ao-tag-chip-current"))
        {
          continue;
        }

        auto* const label = findWidgetByClass<Gtk::Label>(*child, "ao-tag-chip-label");

        if (label != nullptr && label->get_text().raw() == text)
        {
          return child;
        }
      }

      return nullptr;
    }
  } // namespace

  TEST_CASE("TagEditor lays out tag chips and routes chip interactions", "[gtk][unit][tag][geometry]")
  {
    constexpr auto kLongTag = "AnExtremelyLongTagNameForNarrowLayouts";
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto fixture = GtkRuntimeFixture{};
    auto& library = fixture.runtime().musicLibrary();

    auto trackId = TrackId{kInvalidTrackId};
    auto emptyTrackId = TrackId{kInvalidTrackId};

    {
      auto txn = library.writeTransaction();
      auto writer = library.tracks().writer(txn);

      auto builder = library::TrackBuilder::createNew();
      builder.tags().add("Rock");
      builder.tags().add("90s");
      builder.tags().add(kLongTag);

      auto serializeResult = builder.serialize(txn, library.dictionary(), library.resources());
      REQUIRE(serializeResult);
      auto const [hot, cold] = *serializeResult;
      auto [id, _] = ao::test::requireValue(writer.createHotCold(hot, cold));
      trackId = id;

      auto builder2 = library::TrackBuilder::createNew();
      builder2.tags().add("Jazz");
      auto serializeResult2 = builder2.serialize(txn, library.dictionary(), library.resources());
      REQUIRE(serializeResult2);
      auto const [hot2, cold2] = *serializeResult2;
      REQUIRE(writer.createHotCold(hot2, cold2));

      // A track with no tags: selecting it makes every library tag render as a suggestion.
      auto builder3 = library::TrackBuilder::createNew();
      auto serializeResult3 = builder3.serialize(txn, library.dictionary(), library.resources());
      REQUIRE(serializeResult3);
      auto const [hot3, cold3] = *serializeResult3;
      auto [emptyId, _2] = ao::test::requireValue(writer.createHotCold(hot3, cold3));
      emptyTrackId = emptyId;

      REQUIRE(txn.commit());
    }

    auto editor = TagEditor{};
    auto window = Gtk::Window{};
    window.set_child(editor);

    editor.setup(fixture.runtime().library(), {trackId});
    drainGtkEvents();

    SECTION("Minimum width stays compressible for detail pane resize")
    {
      std::int32_t minWidth = 0;
      std::int32_t natWidth = 0;
      std::int32_t minBaseline = -1;
      std::int32_t natBaseline = -1;
      editor.measure(Gtk::Orientation::HORIZONTAL, -1, minWidth, natWidth, minBaseline, natBaseline);

      CHECK(minWidth == 0);
      CHECK(natWidth > 0);

      std::int32_t minHeight = 0;
      std::int32_t natHeight = 0;
      editor.measure(Gtk::Orientation::VERTICAL, 66, minHeight, natHeight, minBaseline, natBaseline);
      editor.size_allocate(Gtk::Allocation{0, 0, 66, natHeight}, -1);

      CHECK(editor.get_width() == 66);
      REQUIRE(editor.get_first_child() != nullptr);
      // Each chip is clamped to the pane width (then ellipsizes); none balloons past it.
      CHECK(editor.get_first_child()->get_width() <= 66);
    }

    SECTION("Overall minimum height remains a lower bound after tags load")
    {
      auto measureMinimumHeight = [&](std::int32_t const width)
      {
        std::int32_t minimum = 0;
        std::int32_t natural = 0;
        std::int32_t minimumBaseline = -1;
        std::int32_t naturalBaseline = -1;
        editor.measure(Gtk::Orientation::VERTICAL, width, minimum, natural, minimumBaseline, naturalBaseline);
        return minimum;
      };

      auto const overallMinimum = measureMinimumHeight(-1);
      CHECK(overallMinimum == 0);
      CHECK(overallMinimum <= measureMinimumHeight(262));
      CHECK(overallMinimum <= measureMinimumHeight(300));
    }

    SECTION("Current and suggested tags share the single flow")
    {
      CHECK(countChipsByClass(editor, "ao-tag-chip-current") == 3);
      CHECK(countChipsByClass(editor, "ao-tag-chip-suggested") == 1); // Jazz

      // The inline add trigger lives in the same flow as a trailing button.
      auto* const addButton = findWidgetByClass<Gtk::Button>(editor, "ao-tag-add-trigger");
      CHECK(addButton != nullptr);
    }

    SECTION("Clicking a suggested tag promotes it to a current chip")
    {
      auto* suggested = findWidgetByClass<Gtk::Button>(editor, "ao-tag-chip-suggested");
      REQUIRE(suggested != nullptr);

      emitClicked(*suggested);
      drainGtkEvents();

      CHECK(countChipsByClass(editor, "ao-tag-chip-current") == 4);
      CHECK(countChipsByClass(editor, "ao-tag-chip-suggested") == 0);
    }

    SECTION("Add trigger expands inline and submits tag names")
    {
      auto* const addButton = findWidgetByClass<Gtk::Button>(editor, "ao-tag-add-trigger");
      auto* const entry = findWidgetByClass<Gtk::Entry>(editor, "ao-tags-entry");
      REQUIRE(addButton != nullptr);
      REQUIRE(entry != nullptr);
      CHECK_FALSE(entry->get_visible());

      emitClicked(*addButton);
      drainGtkEvents();
      CHECK(entry->get_visible());

      entry->set_text("Funk");
      emitActivate(*entry);
      drainGtkEvents();

      CHECK(countChipsByClass(editor, "ao-tag-chip-current") == 4);
      CHECK(entry->get_text().empty()); // cleared for rapid successive adds
      CHECK(entry->get_visible());      // stays open

      std::int32_t position = 0;
      entry->insert_text("123 Mix", -1, position);
      CHECK(entry->get_text() == "123 Mix");

      emitActivate(*entry);
      drainGtkEvents();

      CHECK(countChipsByClass(editor, "ao-tag-chip-current") == 5);
      CHECK(currentChipWithLabel(editor, "123 Mix") != nullptr);
      CHECK(entry->get_text().empty());
      CHECK(entry->get_visible());
    }

    SECTION("Clicking outside the open entry dismisses it without committing")
    {
      auto* const addButton = findWidgetByClass<Gtk::Button>(editor, "ao-tag-add-trigger");
      auto* const entry = findWidgetByClass<Gtk::Entry>(editor, "ao-tags-entry");
      REQUIRE(addButton != nullptr);
      REQUIRE(entry != nullptr);

      emitClicked(*addButton);
      drainGtkEvents();
      REQUIRE(entry->get_visible());

      entry->set_text("Funk"); // pending text must be discarded, never committed on an outside click
      drainGtkEvents();

      // A press landing outside the trigger (here resolving to nothing) collapses the entry.
      REQUIRE(emitWindowOutsideClick(window));
      drainGtkEvents();

      CHECK_FALSE(entry->get_visible());
      CHECK(addButton->get_visible());
      CHECK(countChipsByClass(editor, "ao-tag-chip-current") == 3); // nothing added
    }

    SECTION("Opening the add entry hides current tags and dismissing restores them")
    {
      auto* const addButton = findWidgetByClass<Gtk::Button>(editor, "ao-tag-add-trigger");
      REQUIRE(addButton != nullptr);

      auto* const rockChip = currentChipWithLabel(editor, "Rock");
      REQUIRE(rockChip != nullptr);
      CHECK(rockChip->get_visible()); // shown in the default browse view

      emitClicked(*addButton);
      drainGtkEvents();
      CHECK_FALSE(rockChip->get_visible()); // hidden while adding/searching

      REQUIRE(emitWindowOutsideClick(window));
      drainGtkEvents();
      CHECK(rockChip->get_visible()); // restored once the entry is dismissed
    }

    SECTION("Open add entry leaves the remaining suggested chip at its natural width")
    {
      // Regression: under GtkFlowBox the inline entry shared a grid column with a chip and inflated
      // it, so the lone suggested chip left after the current tags hide ballooned to the entry's
      // width. The custom flow layout sizes every child independently — the chip keeps its own
      // natural width.
      auto* const addButton = findWidgetByClass<Gtk::Button>(editor, "ao-tag-add-trigger");
      REQUIRE(addButton != nullptr);
      emitClicked(*addButton);
      drainGtkEvents();

      auto* const chip = firstSuggestedChip(editor); // only Jazz remains visible
      REQUIRE(chip != nullptr);

      std::int32_t chipMinW = 0;
      std::int32_t chipNatW = 0;
      std::int32_t b1 = -1;
      std::int32_t b2 = -1;
      chip->measure(Gtk::Orientation::HORIZONTAL, -1, chipMinW, chipNatW, b1, b2);

      std::int32_t edMinH = 0;
      std::int32_t edNatH = 0;
      editor.measure(Gtk::Orientation::VERTICAL, 280, edMinH, edNatH, b1, b2);
      editor.size_allocate(Gtk::Allocation{0, 0, 280, edNatH}, -1);

      // The chip stays at (or below) its own natural width; it is never inflated to share the
      // entry's width the way a GtkFlowBox grid column would. The old bug ballooned it to ~198.
      constexpr std::int32_t kSlack = 2;
      CHECK(chip->get_width() <= chipNatW + kSlack);
    }

    SECTION("Suggested chips in a wrapped flow each keep their own natural width")
    {
      // Reproduces the all-suggestions layout (a track with no current tags). Under GtkFlowBox a
      // narrow chip that shared a grid column with a wide chip in another row inherited the wide
      // width; the custom flow sizes every child independently, regardless of row.
      editor.setup(fixture.runtime().library(), {emptyTrackId});
      drainGtkEvents();

      CHECK(countChipsByClass(editor, "ao-tag-chip-current") == 0);
      CHECK(countChipsByClass(editor, "ao-tag-chip-suggested") >= 3);

      std::int32_t edMinH = 0;
      std::int32_t edNatH = 0;
      std::int32_t b1 = -1;
      std::int32_t b2 = -1;
      constexpr std::int32_t kPaneWidth = 180; // narrow enough to wrap onto several rows
      editor.measure(Gtk::Orientation::VERTICAL, kPaneWidth, edMinH, edNatH, b1, b2);
      editor.size_allocate(Gtk::Allocation{0, 0, kPaneWidth, edNatH}, -1);

      std::int32_t checked = 0;

      for (auto* child = editor.get_first_child(); child != nullptr; child = child->get_next_sibling())
      {
        if (!child->has_css_class("ao-tag-chip-suggested"))
        {
          continue;
        }

        std::int32_t cMin = 0;
        std::int32_t cNat = 0;
        child->measure(Gtk::Orientation::HORIZONTAL, -1, cMin, cNat, b1, b2);

        // No chip may be inflated past its own natural width by a column-sharing neighbour.
        CHECK(child->get_width() <= cNat + 2);
        ++checked;
      }

      CHECK(checked >= 3);
    }

    SECTION("Modern theme spaces chips further apart than classic")
    {
      // The inter-chip flow gap is theme-aware (dense classic vs. airy modern). The theme class
      // lives on the toplevel window and is read via the editor's root, so toggling it on the window
      // must widen the single-row natural width by exactly the gap delta on every inter-chip seam.
      // Chip widths are identical between measures (headless loads no per-theme CSS), so the gap is
      // the only variable in the natural width — making this assertion exact and flake-free.
      auto countVisibleChildren = [&]
      {
        std::int32_t n = 0;

        for (auto* c = editor.get_first_child(); c != nullptr; c = c->get_next_sibling())
        {
          if (c->get_visible())
          {
            ++n;
          }
        }

        return n;
      };

      std::int32_t minW = 0;
      std::int32_t classicNat = 0;
      std::int32_t b1 = -1;
      std::int32_t b2 = -1;
      editor.measure(Gtk::Orientation::HORIZONTAL, -1, minW, classicNat, b1, b2);

      auto const seams = countVisibleChildren() - 1;
      CHECK(seams >= 1);

      window.add_css_class("ao-theme-modern");
      editor.queue_resize(); // invalidate the cached measurement so the new gap is recomputed
      drainGtkEvents();

      std::int32_t modernNat = 0;
      editor.measure(Gtk::Orientation::HORIZONTAL, -1, minW, modernNat, b1, b2);

      // Classic gap 4 -> modern gap 8 = +4 per seam (kSpacingLarge - kSpacingSmall).
      constexpr std::int32_t kGapDelta = 4;
      CHECK(modernNat == classicNat + (seams * kGapDelta));

      window.remove_css_class("ao-theme-modern");
    }

    SECTION("Typing in the add entry filters the suggested chips")
    {
      auto* const addButton = findWidgetByClass<Gtk::Button>(editor, "ao-tag-add-trigger");
      auto* const entry = findWidgetByClass<Gtk::Entry>(editor, "ao-tags-entry");
      REQUIRE(addButton != nullptr);
      REQUIRE(entry != nullptr);

      emitClicked(*addButton);
      drainGtkEvents();

      auto* const jazz = firstSuggestedChip(editor); // only Jazz is suggested
      REQUIRE(jazz != nullptr);

      entry->set_text("xyz"); // matches nothing
      drainGtkEvents();
      CHECK_FALSE(jazz->get_visible());

      entry->set_text("ja"); // matches Jazz
      drainGtkEvents();
      CHECK(jazz->get_visible());
    }

    SECTION("Long current tag keeps a usable remove button when narrow")
    {
      auto* const longChip = currentChipWithLabel(editor, kLongTag);
      REQUIRE(longChip != nullptr);

      auto* const removeButton = findWidgetByClass<Gtk::Button>(*longChip, "ao-tag-chip-remove");
      REQUIRE(removeButton != nullptr);

      std::int32_t minWidth = 0;
      std::int32_t natWidth = 0;
      std::int32_t minHeight = 0;
      std::int32_t natHeight = 0;
      std::int32_t minBaseline = -1;
      std::int32_t natBaseline = -1;
      longChip->measure(Gtk::Orientation::HORIZONTAL, -1, minWidth, natWidth, minBaseline, natBaseline);
      longChip->measure(Gtk::Orientation::VERTICAL, minWidth, minHeight, natHeight, minBaseline, natBaseline);
      longChip->size_allocate(Gtk::Allocation{0, 0, minWidth, natHeight}, -1);

      CHECK(minWidth >= 20);
      CHECK(natWidth > minWidth);
      CHECK(removeButton->get_width() >= 20);

      emitClicked(*removeButton);
      drainGtkEvents();

      CHECK(countChipsByClass(editor, "ao-tag-chip-current") == 2);
    }

    window.unset_child();
  }
} // namespace ao::gtk::test
