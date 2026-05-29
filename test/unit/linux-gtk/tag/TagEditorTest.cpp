// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "tag/TagEditor.h"

#include "test/unit/linux-gtk/GtkTestSupport.h"
#include <ao/Type.h>
#include <ao/library/MusicLibrary.h>
#include <ao/library/TrackBuilder.h>
#include <ao/library/TrackStore.h>
#include <ao/lmdb/Transaction.h>

#include <catch2/catch_test_macros.hpp>

namespace ao::gtk::test
{
  TEST_CASE("TagEditor - smoke test", "[gtk][tag]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto fixture = GtkRuntimeFixture{};
    auto& library = fixture.runtime().musicLibrary();

    auto trackId = TrackId{kInvalidTrackId};

    // 1. Add a track with tags
    {
      auto txn = library.writeTransaction();
      auto writer = library.tracks().writer(txn);

      auto builder = library::TrackBuilder::createNew();
      builder.metadata().title("Tagged Track");
      builder.tags().add("Rock");
      builder.tags().add("90s");

      auto const [hot, cold] = builder.serialize(txn, library.dictionary(), library.resources());
      auto [id, _] = writer.createHotCold(hot, cold);
      trackId = id;

      // Add another track with a different tag to show up in "available"
      auto builder2 = library::TrackBuilder::createNew();
      builder2.metadata().title("Other Track");
      builder2.tags().add("Jazz");
      auto const [hot2, cold2] = builder2.serialize(txn, library.dictionary(), library.resources());
      writer.createHotCold(hot2, cold2);

      txn.commit();
    }

    auto editor = TagEditor{};
    editor.setup(library, {trackId});
    drainGtkEvents();
  }
} // namespace ao::gtk::test
