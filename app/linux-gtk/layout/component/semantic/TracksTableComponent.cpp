// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "layout/component/ComponentRegistrations.h"
#include "layout/runtime/ComponentRegistry.h"
#include "layout/runtime/LayoutBuildContext.h"
#include "layout/runtime/LayoutComponent.h"
#include "track/TrackPageHost.h"
#include <ao/Contract.h>
#include <ao/rt/Log.h>
#include <ao/uimodel/layout/component/LayoutSchema.h>
#include <ao/uimodel/layout/document/LayoutNode.h>
#include <ao/uimodel/presentation/CoverArtPlaceholder.h>

#include <gtkmm/box.h>
#include <gtkmm/enums.h>
#include <gtkmm/label.h>
#include <gtkmm/object.h>
#include <gtkmm/stack.h>
#include <gtkmm/widget.h>

#include <memory>
#include <string>

namespace ao::gtk::layout
{
  using namespace uimodel;
  namespace
  {
    /**
     * @brief track.table
     */
    class TracksTableComponent final : public LayoutComponent
    {
    public:
      TracksTableComponent(TrackPageHost* trackPageHost, LayoutBuildContext& ctx, LayoutNode const& node)
      {
        if (trackPageHost == nullptr)
        {
          _container.append(*Gtk::make_managed<Gtk::Label>("Error: trackPageHost missing"));
          return;
        }

        auto const defaultStyle =
          uimodel::defaultCoverArtPlaceholderStyle(uimodel::CoverArtPlaceholderSlot::GroupHeading);
        auto const styleId = node.propertyOr<std::string>(
          "groupCoverPlaceholderStyle", std::string{uimodel::coverArtPlaceholderStyleId(defaultStyle)});
        auto const optParsedStyle = uimodel::parseCoverArtPlaceholderStyle(styleId);
        auto const style = optParsedStyle.value_or(defaultStyle);

        if (!optParsedStyle)
        {
          APP_LOG_WARN("track.table: unknown groupCoverPlaceholderStyle '{}'; using '{}'",
                       styleId,
                       uimodel::coverArtPlaceholderStyleId(defaultStyle));
        }

        trackPageHost->setGroupCoverPlaceholderStyle(style);

        _stack = &trackPageHost->stack();

        if (ctx.sharedWidgetHandoff != nullptr)
        {
          ctx.sharedWidgetHandoff->transfer(*_stack, _container);
        }
        else
        {
          AO_EXPECTS(_stack->get_parent() == nullptr,
                     "track.table cannot attach an already-parented TrackPageHost stack outside a build");
          _container.append(*_stack);
        }

        _container.set_hexpand(true);
        _container.set_vexpand(true);
      }

      ~TracksTableComponent() override
      {
        if (_stack != nullptr && _stack->get_parent() == &_container)
        {
          _container.remove(*_stack);
        }
      }

      TracksTableComponent(TracksTableComponent const&) = delete;
      TracksTableComponent& operator=(TracksTableComponent const&) = delete;
      TracksTableComponent(TracksTableComponent&&) = delete;
      TracksTableComponent& operator=(TracksTableComponent&&) = delete;

      Gtk::Widget& widget() override { return _container; }

    private:
      Gtk::Box _container{Gtk::Orientation::VERTICAL};
      Gtk::Stack* _stack = nullptr;
    };
  } // namespace

  void registerTracksTableComponent(ComponentRegistry& registry, TrackPageHost* trackPageHost)
  {
    registry.registerSharedComponent("track.table",
                                     {.properties = {{.name = "view",
                                                      .kind = PropertyKind::String,
                                                      .label = "View Source",
                                                      .defaultValue = LayoutValue{"workspace.focused"}},
                                                     {.name = "groupCoverPlaceholderStyle",
                                                      .kind = PropertyKind::Enum,
                                                      .label = "Group Cover Placeholder",
                                                      .defaultValue = LayoutValue{"monogram"},
                                                      .enumValues = coverArtPlaceholderStyleIds()}}},
                                     [trackPageHost](LayoutBuildContext& ctx, LayoutNode const& node)
                                     { return std::make_unique<TracksTableComponent>(trackPageHost, ctx, node); });
  }
} // namespace ao::gtk::layout
