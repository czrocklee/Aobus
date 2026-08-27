// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "PlaybackComponentRegistrations.h"
#include "layout/runtime/ComponentRegistry.h"
#include "layout/runtime/LayoutBuildContext.h"
#include "layout/runtime/LayoutComponent.h"
#include "playback/TimeLabel.h"
#include <ao/rt/playback/PlaybackService.h>
#include <ao/uimodel/layout/component/SharedLayoutComponentType.h>
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
      TimeLabelComponent(rt::PlaybackService& playback, LayoutNode const& node)
        : _label{playback,
                 [mode = node.propertyOr<std::string>(kModeProp, "combined")]
                 {
                   if (mode == "elapsed")
                   {
                     return TimeLabel::Mode::Elapsed;
                   }

                   if (mode == "duration")
                   {
                     return TimeLabel::Mode::Duration;
                   }

                   return TimeLabel::Mode::Combined;
                 }()}
      {
      }

      Gtk::Widget& widget() override { return _label.widget(); }

    private:
      TimeLabel _label;
    };
  } // namespace

  void registerTimeLabelComponent(ComponentRegistry& registry, rt::PlaybackService& playback)
  {
    registry.registerComponent(sharedComponentDescriptor(SharedLayoutComponentType::PlaybackTimeLabel),
                               [&playback](LayoutBuildContext const& /*ctx*/, LayoutNode const& node)
                               { return std::make_unique<TimeLabelComponent>(playback, node); });
  }
} // namespace ao::gtk::layout
