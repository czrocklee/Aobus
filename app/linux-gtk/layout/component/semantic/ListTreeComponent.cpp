// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "layout/component/ComponentRegistrations.h"
#include "layout/runtime/ComponentRegistry.h"
#include "layout/runtime/LayoutBuildContext.h"
#include "layout/runtime/LayoutComponent.h"
#include "list/ListNavigationController.h"
#include <ao/uimodel/layout/component/LayoutSchema.h>
#include <ao/uimodel/layout/document/LayoutNode.h>

#include <gtkmm/label.h>
#include <gtkmm/object.h>
#include <gtkmm/widget.h>

#include <memory>

namespace ao::gtk::layout
{
  using namespace uimodel;
  namespace
  {
    /**
     * @brief library.listTree
     */
    class ListTreeComponent final : public LayoutComponent
    {
    public:
      ListTreeComponent(TrackRowCache* trackRowCache, ListNavigationController* listNavigationController)
      {
        if (trackRowCache == nullptr)
        {
          _error = Gtk::make_managed<Gtk::Label>("Error: trackRowCache missing");
          return;
        }

        if (listNavigationController == nullptr)
        {
          _error = Gtk::make_managed<Gtk::Label>("Error: listNavigationController missing");
          return;
        }

        _controller = listNavigationController;

        // Initial rebuild
        _controller->rebuildTree(*trackRowCache);
      }

      Gtk::Widget& widget() override
      {
        return (_error != nullptr) ? static_cast<Gtk::Widget&>(*_error) : _controller->widget();
      }

    private:
      ListNavigationController* _controller = nullptr;
      Gtk::Label* _error = nullptr;
    };
  } // namespace

  void registerListTreeComponent(ComponentRegistry& registry,
                                 TrackRowCache* trackRowCache,
                                 ListNavigationController* listNavigationController)
  {
    registry.registerComponent(
      {.id = "library.listTree",
       .displayName = "Library Tree",
       .category = ComponentCategory::Library,
       .minChildren = 0,
       .optMaxChildren = 0},
      [trackRowCache, listNavigationController](LayoutBuildContext const& /*ctx*/, LayoutNode const& /*node*/)
      { return std::make_unique<ListTreeComponent>(trackRowCache, listNavigationController); });
  }
} // namespace ao::gtk::layout
