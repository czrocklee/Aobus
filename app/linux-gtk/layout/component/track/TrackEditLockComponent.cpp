// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "TrackComponentRegistrations.h"
#include "layout/component/track/TrackDetailScope.h"
#include "layout/document/LayoutNode.h"
#include "layout/runtime/ComponentRegistry.h"
#include "layout/runtime/ILayoutComponent.h"
#include "layout/runtime/LayoutContext.h"

#include <gtkmm/button.h>
#include <gtkmm/widget.h>

#include <memory>
#include <string>

namespace ao::gtk::layout
{
  namespace
  {
    class TrackEditLockComponent final : public ILayoutComponent
    {
    public:
      TrackEditLockComponent(LayoutContext& ctx, LayoutNode const& node)
        : _scope{ctx.track.detailScope}
      {
        _lockedIcon = node.getProp<std::string>("lockedIcon", "changes-prevent-symbolic");
        _unlockedIcon = node.getProp<std::string>("unlockedIcon", "changes-allow-symbolic");

        _button.set_has_frame(false);
        _button.add_css_class("ao-icon-button");
        _button.signal_clicked().connect(
          [this]
          {
            if (_scope != nullptr)
            {
              _scope->setEditLocked(!_scope->isEditLocked());
            }
          });

        if (_scope != nullptr)
        {
          _lockConn = _scope->signalEditLockChanged().connect([this](bool locked) { updateIcon(locked); });
          updateIcon(_scope->isEditLocked());
        }
      }

      Gtk::Widget& widget() override { return _button; }

    private:
      void updateIcon(bool locked)
      {
        _button.set_icon_name(locked ? _lockedIcon : _unlockedIcon);
        _button.set_tooltip_text(locked ? "Unlock to Edit" : "Lock Editing");
      }

      Gtk::Button _button;
      ITrackDetailScope* _scope;
      std::string _lockedIcon;
      std::string _unlockedIcon;
      sigc::connection _lockConn;
    };

    std::unique_ptr<ILayoutComponent> createTrackEditLock(LayoutContext& ctx, LayoutNode const& node)
    {
      return std::make_unique<TrackEditLockComponent>(ctx, node);
    }
  } // namespace

  void registerTrackEditLockComponent(ComponentRegistry& registry)
  {
    registry.registerComponent(
      {.type = "track.editLock",
       .displayName = "Edit Lock",
       .category = "Tracks",
       .container = false,
       .props = {{.name = "lockedIcon", .kind = PropertyKind::String, .label = "Locked Icon"},
                 {.name = "unlockedIcon", .kind = PropertyKind::String, .label = "Unlocked Icon"}},
       .layoutProps = {},
       .minChildren = 0,
       .optMaxChildren = 0},
      createTrackEditLock);
  }
} // namespace ao::gtk::layout
