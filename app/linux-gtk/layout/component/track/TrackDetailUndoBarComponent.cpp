// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "common/UiWorkflow.h"
#include "i18n/GtkTextCatalog.h"
#include "layout/component/ComponentRegistrations.h"
#include "layout/component/track/TrackDetailUndo.h"
#include "layout/runtime/ComponentRegistry.h"
#include "layout/runtime/LayoutBuildContext.h"
#include "layout/runtime/LayoutComponent.h"
#include <ao/Error.h>
#include <ao/async/LifetimeScope.h>
#include <ao/i18n/MessageCatalog.h>
#include <ao/rt/AppRuntime.h>
#include <ao/rt/NotificationService.h>
#include <ao/rt/NotificationState.h>
#include <ao/uimodel/layout/component/LayoutSchema.h>
#include <ao/uimodel/layout/document/LayoutNode.h>

#include <gtkmm/box.h>
#include <gtkmm/button.h>
#include <gtkmm/enums.h>
#include <gtkmm/label.h>
#include <gtkmm/widget.h>
#include <pangomm/layout.h>
#include <sigc++/connection.h>

#include <memory>
#include <string>
#include <utility>

namespace ao::gtk::layout
{
  using namespace uimodel;
  namespace
  {
    class TrackDetailUndoBarComponent final : public LayoutComponent
    {
    public:
      TrackDetailUndoBarComponent(rt::AppRuntime& runtime,
                                  i18n::MessageCatalog textCatalog,
                                  LayoutBuildContext const& ctx,
                                  LayoutNode const& /*node*/)
        : _undoController{ctx.detailUndo}
        , _runtime{runtime}
        , _notifications{runtime.notifications()}
        , _textCatalog{std::move(textCatalog)}
      {
        _undoButton.set_label(gtkText(_textCatalog, i18n::MessageId::GtkCommonUndo));
        _bar.set_orientation(Gtk::Orientation::HORIZONTAL);
        _bar.set_spacing(8);
        _bar.set_margin(8);
        _bar.add_css_class("ao-undo-bar");
        _bar.set_visible(false);

        _label.set_halign(Gtk::Align::START);
        _label.set_hexpand(true);
        _label.set_ellipsize(Pango::EllipsizeMode::END);
        _label.set_single_line_mode(true);
        _bar.append(_label);

        _undoButton.add_css_class("flat");
        _undoButton.add_css_class("ao-undo-button");
        _undoButton.signal_clicked().connect(
          [this]
          {
            if (_undoController != nullptr)
            {
              spawnUiTask(
                _runtime.async(),
                _tasks,
                *this,
                "metadata undo",
                _undoController->undo(),
                [](TrackDetailUndoBarComponent* owner, Result<> result)
                {
                  if (result)
                  {
                    return;
                  }

                  if (result.error().code == Error::Code::ResourceBusy)
                  {
                    owner->_notifications.post(rt::NotificationSeverity::Warning,
                                               gtkText(owner->_textCatalog, i18n::MessageId::LibraryBusyTryAgain),
                                               rt::NotificationLifetime::transient());
                  }
                  else
                  {
                    owner->_notifications.post(
                      rt::NotificationSeverity::Error, result.error().message, rt::NotificationLifetime::history());
                  }
                });
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
        _tasks.cancelAll();

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
        auto const text =
          i18n::requiredFormat(_textCatalog, i18n::MessageId::GtkCustomMetadataDeleted, {{"key", pending.key}});
        _label.set_text(text);
        _label.set_tooltip_text(text);
        _bar.set_visible(true);
      }

      TrackDetailUndoController* _undoController = nullptr;
      rt::AppRuntime& _runtime;
      rt::NotificationService& _notifications;
      i18n::MessageCatalog _textCatalog;
      Gtk::Box _bar{Gtk::Orientation::HORIZONTAL, 0};
      Gtk::Label _label;
      Gtk::Button _undoButton;
      sigc::connection _changedConn;
      async::LifetimeScope _tasks;
    };
  } // namespace

  void registerTrackDetailUndoBarComponent(ComponentRegistry& registry,
                                           rt::AppRuntime& runtime,
                                           i18n::MessageCatalog const& textCatalog)
  {
    registry.registerComponent(
      {.id = "track.detailUndoBar",
       .displayName = "Detail Undo Bar",
       .category = ComponentCategory::Track,
       .minChildren = 0,
       .optMaxChildren = 0},
      [&runtime, textCatalog](LayoutBuildContext const& ctx, LayoutNode const& node)
      { return std::make_unique<TrackDetailUndoBarComponent>(runtime, textCatalog, ctx, node); });
  }
} // namespace ao::gtk::layout
