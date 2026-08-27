// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "PlaybackComponentRegistrations.h"
#include "layout/runtime/ComponentRegistry.h"
#include "layout/runtime/LayoutBuildContext.h"
#include "layout/runtime/LayoutComponent.h"
#include "playback/SeekControlWidget.h"
#include <ao/rt/playback/PlaybackService.h>
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
     * @brief playback.seekSlider
     */
    class SeekSliderComponent final : public LayoutComponent
    {
    public:
      explicit SeekSliderComponent(rt::PlaybackService& playback)
        : _control{playback}
      {
      }

      Gtk::Widget& widget() override { return _control.widget(); }

    private:
      SeekControlWidget _control;
    };
  } // namespace

  void registerSeekSliderComponent(ComponentRegistry& registry, rt::PlaybackService& playback)
  {
    registry.registerComponent(sharedComponentDescriptor(SharedLayoutComponentType::PlaybackSeekSlider),
                               [&playback](LayoutBuildContext const& /*ctx*/, LayoutNode const& /*node*/)
                               { return std::make_unique<SeekSliderComponent>(playback); });
  }
} // namespace ao::gtk::layout
