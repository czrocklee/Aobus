// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "StatusComponentRegistrations.h"
#include "layout/runtime/ComponentRegistry.h"
#include "layout/runtime/ILayoutComponent.h"
#include "layout/runtime/LayoutContext.h"
#include "track/LibraryTrackCountLabel.h"
#include <ao/rt/AppRuntime.h>
#include <ao/rt/ListSourceStore.h>
#include <ao/uimodel/layout/ComponentCatalog.h>
#include <ao/uimodel/layout/LayoutNode.h>

#include <gtkmm/widget.h>

#include <memory>

namespace ao::gtk::layout
{
  using namespace uimodel::layout;
  namespace
  {
    class LibraryTrackCountComponent final : public ILayoutComponent
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

    std::unique_ptr<ILayoutComponent> createTrackCount(LayoutContext& ctx, LayoutNode const& node)
    {
      return std::make_unique<LibraryTrackCountComponent>(ctx, node);
    }
  } // namespace

  void registerLibraryTrackCountComponent(ComponentRegistry& registry)
  {
    registry.registerComponent(
      {.type = "status.trackCount", .displayName = "Library Track Count", .category = ComponentCategory::Status},
      createTrackCount);
  }
} // namespace ao::gtk::layout
