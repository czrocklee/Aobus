// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "tag/TagEditor.h"

#include "gtkmm/button.h"
#include "gtkmm/enums.h"
#include "gtkmm/flowbox.h"
#include "gtkmm/flowboxchild.h"
#include "gtkmm/widget.h"
#include "test/unit/linux-gtk/GtkTestSupport.h"
#include <ao/Type.h>
#include <ao/library/MusicLibrary.h>
#include <ao/library/TrackBuilder.h>
#include <ao/library/TrackStore.h>
#include <ao/lmdb/Transaction.h>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string_view>

namespace ao::gtk::test
{
  namespace
  {
    template<typename T>
    T* findWidgetByClass(Gtk::Widget& root, std::string_view const className)
    {
      if (root.has_css_class(Glib::ustring{std::string{className}}))
      {
        if (auto* const target = dynamic_cast<T*>(&root); target != nullptr)
        {
          return target;
        }
      }

      for (auto* child = root.get_first_child(); child != nullptr; child = child->get_next_sibling())
      {
        if (auto* const result = findWidgetByClass<T>(*child, className); result != nullptr)
        {
          return result;
        }
      }

      return nullptr;
    }
  } // namespace

  TEST_CASE("TagEditor - chip interaction", "[gtk][tag]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto fixture = GtkRuntimeFixture{};
    auto& library = fixture.runtime().musicLibrary();

    auto trackId = TrackId{kInvalidTrackId};

    {
      auto txn = library.writeTransaction();
      auto writer = library.tracks().writer(txn);

      auto builder = library::TrackBuilder::createNew();
      builder.tags().add("Rock");
      builder.tags().add("90s");

      auto const [hot, cold] = builder.serialize(txn, library.dictionary(), library.resources());
      auto [id, _] = writer.createHotCold(hot, cold);
      trackId = id;

      auto builder2 = library::TrackBuilder::createNew();
      builder2.tags().add("Jazz");
      auto const [hot2, cold2] = builder2.serialize(txn, library.dictionary(), library.resources());
      writer.createHotCold(hot2, cold2);

      txn.commit();
    }

    auto editor = TagEditor{};
    auto window = Gtk::Window{};
    window.set_child(editor);

    editor.setup(library, {trackId});
    drainGtkEvents();

    auto* currentBox = findWidgetByClass<Gtk::FlowBox>(editor, "ao-tag-editor-current-box");
    auto* availableBox = findWidgetByClass<Gtk::FlowBox>(editor, "ao-tag-editor-available-box");

    REQUIRE(currentBox != nullptr);
    REQUIRE(availableBox != nullptr);

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
      CHECK(editor.get_first_child()->get_width() >= 66);
    }

    SECTION("Initial tags are displayed")
    {
      auto count = 0;

      for (auto* child = currentBox->get_first_child(); child != nullptr; child = child->get_next_sibling())
      {
        count++;
      }

      CHECK(count == 2);

      auto availCount = 0;

      for (auto* child = availableBox->get_first_child(); child != nullptr; child = child->get_next_sibling())
      {
        availCount++;
      }

      CHECK(availCount == 1); // Jazz
    }

    SECTION("Clicking available tag adds it to current")
    {
      auto* firstChild = dynamic_cast<Gtk::FlowBoxChild*>(availableBox->get_first_child());
      REQUIRE(firstChild != nullptr);
      auto* btn = dynamic_cast<Gtk::Button*>(firstChild->get_child());
      REQUIRE(btn != nullptr);

      // Programmatically trigger a click on the native GTK widget
      ::g_signal_emit_by_name(btn->gobj(), "clicked");
      drainGtkEvents();

      auto count = 0;

      for (auto* child = currentBox->get_first_child(); child != nullptr; child = child->get_next_sibling())
      {
        count++;
      }

      CHECK(count == 3);
    }

    window.unset_child();
  }
} // namespace ao::gtk::test
