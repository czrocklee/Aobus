// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "TrackComponentRegistrations.h"
#include "layout/document/LayoutNode.h"
#include "layout/runtime/ComponentRegistry.h"
#include "layout/runtime/ILayoutComponent.h"
#include "layout/runtime/LayoutContext.h"
#include "track/TrackPresentationButton.h"

#include <gtkmm/widget.h>

#include <memory>

namespace ao::gtk::layout
{
  namespace
  {
    /**
     * @brief track.presentationButton component wrapper
     */
    class TrackPresentationButtonComponent final : public ILayoutComponent
    {
    public:
      TrackPresentationButtonComponent(LayoutContext& ctx, LayoutNode const& node)
        : _widget{ctx.runtime}
      {
        if (ctx.track.presentationStore != nullptr)
        {
          _widget.setPresentationStore(ctx.track.presentationStore, ctx.theme.themeController);
        }

        if (auto const it = node.props.find("variant"); it != node.props.end())
        {
          if (auto const variant = it->second.asString(); variant == "title")
          {
            _widget.add_css_class("ao-variant-title");
          }
        }
      }

      Gtk::Widget& widget() override { return _widget; }

    private:
      TrackPresentationButton _widget;
    };

    std::unique_ptr<ILayoutComponent> createTrackPresentationButton(LayoutContext& ctx, LayoutNode const& node)
    {
      return std::make_unique<TrackPresentationButtonComponent>(ctx, node);
    }
  } // namespace

  void registerTrackPresentationButtonComponent(ComponentRegistry& registry)
  {
    registry.registerComponent({.type = "track.presentationButton",
                                .displayName = "Presentation",
                                .category = ComponentCategory::Track,
                                .props = {{.name = "variant",
                                           .kind = PropertyKind::String,
                                           .label = "Variant",
                                           .defaultValue = LayoutValue{"default"}}},
                                .layoutProps = {},
                                .minChildren = 0,
                                .optMaxChildren = 0},
                               createTrackPresentationButton);
  }
} // namespace ao::gtk::layout
