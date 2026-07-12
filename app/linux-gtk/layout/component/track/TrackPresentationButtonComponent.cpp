// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "TrackComponentRegistrations.h"
#include "layout/runtime/ComponentRegistry.h"
#include "layout/runtime/LayoutBuildContext.h"
#include "layout/runtime/LayoutComponent.h"
#include "track/TrackPresentationButton.h"
#include <ao/uimodel/layout/component/LayoutComponentCatalog.h>
#include <ao/uimodel/layout/document/LayoutNode.h>

#include <gtkmm/widget.h>

#include <memory>

namespace ao::gtk::layout
{
  using namespace uimodel;
  namespace
  {
    /**
     * @brief track.presentationButton component wrapper
     */
    class TrackPresentationButtonComponent final : public LayoutComponent
    {
    public:
      TrackPresentationButtonComponent(LayoutBuildContext& ctx, LayoutNode const& node)
        : _widget{ctx.runtime}
      {
        if (ctx.dependencies.trackPresentationCatalog != nullptr &&
            ctx.dependencies.trackPresentationPreferences != nullptr)
        {
          _widget.setPresentationServices(ctx.dependencies.trackPresentationCatalog,
                                          ctx.dependencies.trackPresentationPreferences,
                                          ctx.dependencies.themeCoordinator);
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

    std::unique_ptr<LayoutComponent> createTrackPresentationButton(LayoutBuildContext& ctx, LayoutNode const& node)
    {
      return std::make_unique<TrackPresentationButtonComponent>(ctx, node);
    }
  } // namespace

  void registerTrackPresentationButtonComponent(ComponentRegistry& registry)
  {
    registry.registerComponent({.type = "track.presentationButton",
                                .displayName = "Presentation",
                                .category = LayoutComponentCategory::Track,
                                .props = {{.name = "variant",
                                           .kind = LayoutPropertyKind::String,
                                           .label = "Variant",
                                           .defaultValue = LayoutValue{"default"}}},
                                .layoutProps = {},
                                .minChildren = 0,
                                .optMaxChildren = 0},
                               createTrackPresentationButton);
  }
} // namespace ao::gtk::layout
