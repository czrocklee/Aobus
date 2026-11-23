// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "track/TrackListItem.h"

#include "test/linux-gtk/GtkApplicationTestSupport.h"
#include <ao/CoreIds.h>
#include <ao/utility/Raii.h>

#include <catch2/catch_test_macros.hpp>
#include <glib-object.h>
#include <glibmm/refptr.h>

namespace ao::gtk::test
{
  TEST_CASE("TrackListItemPool - identity survives native-only ownership and releases on last unref")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto const id = TrackId{1};
    bool disposed = false;
    auto const pool = TrackListItemPool{};
    auto itemPtr = pool.get(id);
    REQUIRE(itemPtr);
    CHECK(itemPtr->trackId() == id);
    CHECK(G_OBJECT_TYPE(itemPtr->gobj()) == TrackListItem::objectType());
    auto nativePtr = utility::makeUniquePtr<::g_object_unref>(itemPtr->gobj_copy());
    ::g_object_weak_ref(nativePtr.get(), [](void* data, ::GObject*) { *static_cast<bool*>(data) = true; }, &disposed);
    itemPtr.reset();

    auto lookedUpPtr = pool.get(id);
    REQUIRE(lookedUpPtr);
    CHECK(lookedUpPtr->gobj() == nativePtr.get());
    lookedUpPtr.reset();
    CHECK_FALSE(disposed);
    nativePtr.reset();
    CHECK(disposed);

    auto const recreatedPtr = pool.get(id);
    REQUIRE(recreatedPtr);
    CHECK(recreatedPtr->trackId() == id);
  }

  TEST_CASE("TrackListItemPool - releasing a retained item after pool retirement leaves the replacement pool intact")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto const id = TrackId{1};
    auto retiredItemPtr = Glib::RefPtr<TrackListItem>{};

    {
      auto const pool = TrackListItemPool{};
      retiredItemPtr = pool.get(id);
    }

    REQUIRE(retiredItemPtr);
    CHECK(retiredItemPtr->trackId() == id);

    auto const replacementPool = TrackListItemPool{};
    auto const replacementPtr = replacementPool.get(id);
    REQUIRE(replacementPtr);
    CHECK(replacementPtr->gobj() != retiredItemPtr->gobj());
    retiredItemPtr.reset();

    auto const lookedUpPtr = replacementPool.get(id);
    REQUIRE(lookedUpPtr);
    CHECK(lookedUpPtr->gobj() == replacementPtr->gobj());
  }

  TEST_CASE("TrackListItemPool - relookup during native disposal keeps the replacement interned")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto const pool = TrackListItemPool{};

    struct DisposalLookup final
    {
      TrackListItemPool const& pool;
      TrackId id;
      Glib::RefPtr<TrackListItem> replacementPtr;
      bool reusedDisposingObject = false;
    };

    auto lookup = DisposalLookup{.pool = pool, .id = TrackId{1}, .replacementPtr = {}};
    auto itemPtr = pool.get(lookup.id);
    REQUIRE(itemPtr);
    ::g_object_weak_ref(
      itemPtr->gobj(),
      [](void* data, ::GObject* disposingObject)
      {
        auto& disposal = *static_cast<DisposalLookup*>(data);
        disposal.replacementPtr = disposal.pool.get(disposal.id);
        disposal.reusedDisposingObject = disposal.replacementPtr->gobj() == disposingObject;
      },
      &lookup);
    itemPtr.reset();

    REQUIRE(lookup.replacementPtr);
    CHECK_FALSE(lookup.reusedDisposingObject);
    auto const lookedUpPtr = pool.get(lookup.id);
    REQUIRE(lookedUpPtr);
    CHECK(lookedUpPtr->gobj() == lookup.replacementPtr->gobj());
  }
} // namespace ao::gtk::test
