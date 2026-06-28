// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "PlaybackComponentRegistrations.h"
#include "layout/runtime/ComponentRegistry.h"
#include "layout/runtime/ILayoutComponent.h"
#include "layout/runtime/LayoutContext.h"
#include "playback/AudioDeviceSelector.h"
#include <ao/rt/AppRuntime.h>
#include <ao/uimodel/layout/component/LayoutComponentCatalog.h>
#include <ao/uimodel/layout/document/LayoutNode.h>
#include <ao/uimodel/playback/output/AudioOutputViewModel.h>

#include <gtkmm/button.h>
#include <gtkmm/enums.h>
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
     * @brief playback.outputSelector
     */
    class OutputSelectorComponent final : public ILayoutComponent
    {
    public:
      OutputSelectorComponent(LayoutContext& ctx, LayoutNode const& /*node*/)
        : _playback{ctx.runtime.playback()}
        , _viewModel{_playback,
                     [this](uimodel::AudioOutputViewState const& view)
                     {
                       _label.set_text(view.backendSummary);
                       _button.set_tooltip_text(view.outputStatus);
                     }}
      {
        _button.set_has_frame(false);
        _button.add_css_class("ao-output-selector-modern");
        _button.set_child(_label);

        _button.signal_clicked().connect(
          [this]
          {
            auto* const popover = Gtk::make_managed<AudioDeviceSelector>(_playback, Gtk::PositionType::TOP);
            popover->set_parent(_button);
            popover->signal_closed().connect([popover] { popover->unparent(); });
            popover->popup();
          });

        _viewModel.refresh();
      }

      Gtk::Widget& widget() override { return _button; }

    private:
      rt::PlaybackService& _playback;
      Gtk::Button _button;
      Gtk::Label _label;
      uimodel::AudioOutputViewModel _viewModel;
    };

    std::unique_ptr<ILayoutComponent> createOutputSelector(LayoutContext& ctx, LayoutNode const& node)
    {
      return std::make_unique<OutputSelectorComponent>(ctx, node);
    }
  } // namespace

  void registerOutputSelectorComponent(ComponentRegistry& registry)
  {
    registry.registerComponent({.type = "playback.outputSelector",
                                .displayName = "Output Selector",
                                .category = LayoutComponentCategory::Playback,
                                .props = {},
                                .layoutProps = {},
                                .minChildren = 0,
                                .optMaxChildren = 0},
                               createOutputSelector);
  }
} // namespace ao::gtk::layout
