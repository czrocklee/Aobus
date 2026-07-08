// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "TrackComponentRegistrations.h"
#include "layout/component/track/TrackDetailUndo.h"
#include "layout/runtime/ComponentRegistry.h"
#include "layout/runtime/LayoutComponent.h"
#include "layout/runtime/LayoutContext.h"
#include <ao/uimodel/layout/component/LayoutComponentCatalog.h>
#include <ao/uimodel/layout/document/LayoutNode.h>

#include <gtkmm/box.h>
#include <gtkmm/button.h>
#include <gtkmm/enums.h>
#include <gtkmm/label.h>
#include <gtkmm/widget.h>
#include <sigc++/connection.h>

#include <format>
#include <memory>

namespace ao::gtk::layout
{
  using namespace uimodel;
  namespace
  {
    class TrackDetailUndoBarComponent final : public LayoutComponent
    {
    public:
      TrackDetailUndoBarComponent(LayoutContext& ctx, LayoutNode const& /*node*/)
        : _undoController{ctx.track.detailUndo}
      {
        _bar.set_orientation(Gtk::Orientation::HORIZONTAL);
        _bar.set_spacing(8);
        _bar.set_margin(8);
        _bar.add_css_class("ao-undo-bar");
        _bar.set_visible(false);

        _label.set_halign(Gtk::Align::START);
        _label.set_hexpand(true);
        _bar.append(_label);

        _undoButton.add_css_class("flat");
        _undoButton.add_css_class("ao-undo-button");
        _undoButton.signal_clicked().connect(
          [this]
          {
            if (_undoController != nullptr)
            {
              _undoController->undo();
            }
          });
        _bar.append(_undoButton);

        if (_undoController != nullptr)
        {
          _changedConn = _undoController->signalChanged().connect([this] { render(); });
        }

        render();
      }

      TrackDetailUndoBarComponent(TrackDetailUndoBarComponent const&) = delete;
      TrackDetailUndoBarComponent& operator=(TrackDetailUndoBarComponent const&) = delete;
      TrackDetailUndoBarComponent(TrackDetailUndoBarComponent&&) = delete;
      TrackDetailUndoBarComponent& operator=(TrackDetailUndoBarComponent&&) = delete;

      ~TrackDetailUndoBarComponent() override
      {
        if (_changedConn)
        {
          _changedConn.disconnect();
        }
      }

      Gtk::Widget& widget() override { return _bar; }

    private:
      void render()
      {
        if (_undoController == nullptr || !_undoController->pendingCustomMetadataUndo())
        {
          _bar.set_visible(false);
          return;
        }

        auto const& pending = *_undoController->pendingCustomMetadataUndo();
        _label.set_text(std::format("Custom metadata '{}' removed", pending.key));
        _bar.set_visible(true);
      }

      TrackDetailUndoController* _undoController = nullptr;
      Gtk::Box _bar{Gtk::Orientation::HORIZONTAL, 0};
      Gtk::Label _label;
      Gtk::Button _undoButton{"Undo"};
      sigc::connection _changedConn;
    };

    std::unique_ptr<LayoutComponent> createTrackDetailUndoBar(LayoutContext& ctx, LayoutNode const& node)
    {
      return std::make_unique<TrackDetailUndoBarComponent>(ctx, node);
    }
  } // namespace

  void registerTrackDetailUndoBarComponent(ComponentRegistry& registry)
  {
    registry.registerComponent({.type = "track.detailUndoBar",
                                .displayName = "Detail Undo Bar",
                                .category = LayoutComponentCategory::Track,
                                .props = {},
                                .layoutProps = {},
                                .minChildren = 0,
                                .optMaxChildren = 0},
                               createTrackDetailUndoBar);
  }
} // namespace ao::gtk::layout
