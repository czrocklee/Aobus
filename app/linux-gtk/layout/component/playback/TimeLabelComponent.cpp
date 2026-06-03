// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "PlaybackComponentRegistrations.h"
#include "layout/document/LayoutNode.h"
#include "layout/runtime/ComponentRegistry.h"
#include "layout/runtime/ILayoutComponent.h"
#include "layout/runtime/LayoutContext.h"
#include "playback/TimeLabel.h"
#include <ao/rt/AppRuntime.h>

#include <gtkmm/widget.h>

#include <memory>
#include <string>

namespace ao::gtk::layout
{
  namespace
  {
    /**
     * @brief playback.timeLabel
     */
    class TimeLabelComponent final : public ILayoutComponent
    {
    public:
      TimeLabelComponent(LayoutContext& ctx, LayoutNode const& node)
        : _label{ctx.runtime.playback(),
                 [mode = node.getProp<std::string>("mode", "default")]
                 {
                   if (mode == "elapsed")
                   {
                     return TimeLabel::Mode::Elapsed;
                   }

                   if (mode == "duration")
                   {
                     return TimeLabel::Mode::Duration;
                   }

                   return TimeLabel::Mode::Default;
                 }()}
      {
      }

      Gtk::Widget& widget() override { return _label.widget(); }

    private:
      TimeLabel _label;
    };

    std::unique_ptr<ILayoutComponent> createTimeLabel(LayoutContext& ctx, LayoutNode const& node)
    {
      return std::make_unique<TimeLabelComponent>(ctx, node);
    }
  } // namespace

  void registerTimeLabelComponent(ComponentRegistry& registry)
  {
    registry.registerComponent({.type = "playback.timeLabel",
                                .displayName = "Time Label",
                                .category = "Playback",
                                .container = false,
                                .props = {{.name = "mode",
                                           .kind = PropertyKind::Enum,
                                           .label = "Mode",
                                           .defaultValue = LayoutValue{"default"},
                                           .enumValues = {"default", "elapsed", "duration"}}},
                                .layoutProps = {},
                                .minChildren = 0,
                                .optMaxChildren = 0},
                               createTimeLabel);
  }
} // namespace ao::gtk::layout
