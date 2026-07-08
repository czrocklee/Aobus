// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "StatusComponentRegistrations.h"
#include "layout/runtime/ComponentRegistry.h"
#include "layout/runtime/LayoutComponent.h"
#include "layout/runtime/LayoutContext.h"
#include "track/LibraryTrackCountLabel.h"
#include <ao/rt/AppRuntime.h>
#include <ao/rt/source/TrackSourceCache.h>
#include <ao/uimodel/layout/component/LayoutComponentCatalog.h>
#include <ao/uimodel/layout/document/LayoutNode.h>

#include <gtkmm/widget.h>

#include <memory>

namespace ao::gtk::layout
{
  using namespace uimodel;
  namespace
  {
    class LibraryTrackCountComponent final : public LayoutComponent
    {
    public:
      LibraryTrackCountComponent(LayoutContext& ctx, LayoutNode const& /*node*/)
        : _widget{ctx.runtime.sources().allTracks()}
      {
      }

      Gtk::Widget& widget() override { return _widget.widget(); }

    private:
      LibraryTrackCountLabel _widget;
    };

    std::unique_ptr<LayoutComponent> createTrackCount(LayoutContext& ctx, LayoutNode const& node)
    {
      return std::make_unique<LibraryTrackCountComponent>(ctx, node);
    }
  } // namespace

  void registerLibraryTrackCountComponent(ComponentRegistry& registry)
  {
    registry.registerComponent(
      {.type = "status.trackCount", .displayName = "Library Track Count", .category = LayoutComponentCategory::Status},
      createTrackCount);
  }
} // namespace ao::gtk::layout
