// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "StatusComponentRegistrations.h"
#include "layout/runtime/ComponentRegistry.h"
#include "layout/runtime/LayoutBuildContext.h"
#include "layout/runtime/LayoutComponent.h"
#include "track/LibraryTrackCountLabel.h"
#include <ao/Contract.h>
#include <ao/rt/AppRuntime.h>
#include <ao/rt/VirtualListIds.h>
#include <ao/rt/source/TrackSourceCache.h>
#include <ao/rt/source/TrackSourceLease.h>
#include <ao/uimodel/layout/document/LayoutNode.h>

#include <gtkmm/widget.h>

#include <memory>
#include <utility>

namespace ao::gtk::layout
{
  using namespace uimodel;
  namespace
  {
    rt::TrackSourceLease acquireAllTracks(rt::AppRuntime& runtime)
    {
      auto result = runtime.sources().acquire(rt::kAllTracksListId);

      AO_INVARIANT(result, "Failed to acquire All Tracks list");

      return std::move(*result);
    }

    class LibraryTrackCountComponent final : public LayoutComponent
    {
    public:
      LibraryTrackCountComponent(rt::AppRuntime& runtime, i18n::MessageCatalog const& textCatalog)
        : _widget{acquireAllTracks(runtime), textCatalog}
      {
      }

      Gtk::Widget& widget() override { return _widget.widget(); }

    private:
      LibraryTrackCountLabel _widget;
    };
  } // namespace

  void registerLibraryTrackCountComponent(ComponentRegistry& registry,
                                          rt::AppRuntime& runtime,
                                          i18n::MessageCatalog const& textCatalog)
  {
    registry.registerSharedComponent(
      "status.trackCount",
      [&runtime, textCatalog](LayoutBuildContext const& /*ctx*/, LayoutNode const& /*node*/)
      { return std::make_unique<LibraryTrackCountComponent>(runtime, textCatalog); });
  }
} // namespace ao::gtk::layout
