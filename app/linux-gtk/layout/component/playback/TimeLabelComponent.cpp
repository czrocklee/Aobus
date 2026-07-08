// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "PlaybackComponentRegistrations.h"
#include "layout/runtime/ComponentRegistry.h"
#include "layout/runtime/LayoutComponent.h"
#include "layout/runtime/LayoutContext.h"
#include "playback/TimeLabel.h"
#include <ao/rt/AppRuntime.h>
#include <ao/uimodel/layout/component/LayoutComponentCatalog.h>
#include <ao/uimodel/layout/document/LayoutNode.h>

#include <gtkmm/widget.h>

#include <memory>
#include <string>

namespace ao::gtk::layout
{
  using namespace uimodel;
  namespace
  {
    /**
     * @brief playback.timeLabel
     */
    class TimeLabelComponent final : public LayoutComponent
    {
    public:
      TimeLabelComponent(LayoutContext& ctx, LayoutNode const& node)
        : _label{ctx.runtime.playback(),
                 [mode = node.propertyOr<std::string>("mode", "default")]
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

    std::unique_ptr<LayoutComponent> createTimeLabel(LayoutContext& ctx, LayoutNode const& node)
    {
      return std::make_unique<TimeLabelComponent>(ctx, node);
    }
  } // namespace

  void registerTimeLabelComponent(ComponentRegistry& registry)
  {
    registry.registerComponent({.type = "playback.timeLabel",
                                .displayName = "Time Label",
                                .category = LayoutComponentCategory::Playback,
                                .props = {{.name = "mode",
                                           .kind = LayoutPropertyKind::Enum,
                                           .label = "Mode",
                                           .defaultValue = LayoutValue{"default"},
                                           .enumValues = {"default", "elapsed", "duration"}}},
                                .layoutProps = {},
                                .minChildren = 0,
                                .optMaxChildren = 0},
                               createTimeLabel);
  }
} // namespace ao::gtk::layout
