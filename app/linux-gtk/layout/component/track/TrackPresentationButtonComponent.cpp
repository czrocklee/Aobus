// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "TrackComponentRegistrations.h"
#include "app/GtkUiDependencies.h"
#include "layout/runtime/ComponentRegistry.h"
#include "layout/runtime/LayoutBuildContext.h"
#include "layout/runtime/LayoutComponent.h"
#include "track/TrackPresentationButton.h"
#include <ao/uimodel/layout/component/SharedLayoutComponentType.h>
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
        : _widget{ctx.runtime, ctx.dependencies.textCatalog}
      {
        if (ctx.dependencies.trackPresentationCatalog != nullptr &&
            ctx.dependencies.trackPresentationPreferences != nullptr)
        {
          _widget.setPresentationServices(ctx.dependencies.trackPresentationCatalog,
                                          ctx.dependencies.trackPresentationPreferences,
                                          ctx.dependencies.themeCoordinator);
        }

        if (auto const it = node.props.find(kVariantProp); it != node.props.end())
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
    registry.registerComponent(
      sharedComponentDescriptor(SharedLayoutComponentType::TrackPresentationButton), createTrackPresentationButton);
  }
} // namespace ao::gtk::layout
