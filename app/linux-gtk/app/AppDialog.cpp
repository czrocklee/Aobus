// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "app/AppDialog.h"

#include <glib-object.h>
#include <gtkmm/box.h>
#include <gtkmm/button.h>
#include <gtkmm/enums.h>
#include <gtkmm/label.h>
#include <gtkmm/object.h>
#include <gtkmm/widget.h>
#include <gtkmm/window.h>
#include <sigc++/signal.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace ao::gtk
{
  namespace
  {
    constexpr int kMessageSpacing = 12;
    constexpr int kMessageMinWidth = 360;
    constexpr int kMessageMaxWidth = 520;
    constexpr int kActionSpacing = 8;

    void markFinalized(void* const data, GObject* /*whereTheObjectWas*/)
    {
      *static_cast<bool*>(data) = true;
    }
  } // namespace

  AppDialog::AppDialog()
  {
    _headerBar.set_show_title_buttons(false);
    set_titlebar(_headerBar);
    set_child(_rootBox);

    _rootBox.append(_contentWrapper);
    _contentWrapper.add_css_class("ao-dialog-content");

    _startActions.set_spacing(kActionSpacing);
    _endActions.set_spacing(kActionSpacing);
    _endActions.set_hexpand(true);
    _endActions.set_halign(Gtk::Align::END);

    _actionBar.set_spacing(kActionSpacing);
    _actionBar.add_css_class("ao-dialog-actions");
    _actionBar.append(_startActions);
    _actionBar.append(_endActions);
    _actionBar.set_visible(false);
    _rootBox.append(_actionBar);

    // Standard dialogs are usually modal.
    set_modal(true);

    signal_close_request().connect(
      [this]
      {
        if (!_optCloseResponseId || _responseInProgress)
        {
          return false;
        }

        response(*_optCloseResponseId);
        return true;
      },
      false);
  }

  AppDialog::~AppDialog() = default;

  void AppDialog::response(std::int32_t id)
  {
    bool finalized = false;
    auto* const object = G_OBJECT(gobj());
    ::g_object_weak_ref(object, markFinalized, &finalized);
    _responseInProgress = true;
    _signalResponse.emit(id);

    if (finalized)
    {
      return;
    }

    ::g_object_weak_unref(object, markFinalized, &finalized);
    _responseInProgress = false;
  }

  sigc::signal<void(std::int32_t)> AppDialog::signal_response()
  {
    return _signalResponse;
  }

  void AppDialog::configureForParent(Gtk::Window& parent)
  {
    set_transient_for(parent);
    set_destroy_with_parent(true);
    set_modal(true);
  }

  void AppDialog::setDefaultResponse(std::int32_t responseId)
  {
    _optDefaultResponseId = responseId;
    applyDefaultResponse();
  }

  void AppDialog::setCloseResponse(std::int32_t responseId)
  {
    _optCloseResponseId = responseId;
  }

  void AppDialog::setContentWidget(Gtk::Widget& widget)
  {
    // Clear existing content if any
    while (auto* const child = _contentWrapper.get_first_child())
    {
      _contentWrapper.remove(*child);
    }

    _contentWrapper.append(widget);
    widget.set_vexpand(true);
    widget.set_hexpand(true);
  }

  Gtk::Button* AppDialog::addPrimaryAction(std::string const& label, std::int32_t responseId)
  {
    return addAction(label, responseId, AppDialogActionRole::Primary);
  }

  Gtk::Button* AppDialog::addCancelAction(std::string const& label, std::int32_t responseId)
  {
    return addAction(label, responseId, AppDialogActionRole::Cancel);
  }

  std::shared_ptr<AppDialog> AppDialog::createMessage(Gtk::Window& parent,
                                                      std::string const& title,
                                                      std::string const& message,
                                                      std::vector<AppDialogAction> const& actions,
                                                      std::int32_t defaultResponseId)
  {
    auto dialogPtr = std::make_shared<AppDialog>();
    configureMessageDialog(*dialogPtr, parent, title, message, actions, defaultResponseId);
    return dialogPtr;
  }

  AppDialog* AppDialog::presentMessage(Gtk::Window& parent,
                                       std::string const& title,
                                       std::string const& message,
                                       std::vector<AppDialogAction> const& actions,
                                       std::int32_t defaultResponseId,
                                       std::function<void(std::int32_t)> onResponse)
  {
    auto* const dialog = Gtk::make_managed<AppDialog>();
    configureMessageDialog(*dialog, parent, title, message, actions, defaultResponseId);
    dialog->signal_response().connect(
      [dialog, onResponse = std::move(onResponse)](std::int32_t const responseId)
      {
        if (onResponse)
        {
          onResponse(responseId);
        }

        dialog->close();
      });
    dialog->present();
    return dialog;
  }

  Gtk::Button* AppDialog::addAction(std::string const& label, std::int32_t responseId, AppDialogActionRole role)
  {
    auto* const button = Gtk::make_managed<Gtk::Button>(label);
    button->set_receives_default(true);

    if (role == AppDialogActionRole::Primary)
    {
      button->add_css_class("suggested-action");
      _endActions.append(*button);
    }
    else
    {
      _startActions.append(*button);
    }

    _actionBar.set_visible(true);
    button->signal_clicked().connect([this, responseId] { response(responseId); });
    _responseButtons.emplace_back(responseId, button);
    applyDefaultResponse();
    return button;
  }

  void AppDialog::applyDefaultResponse()
  {
    if (!_optDefaultResponseId)
    {
      return;
    }

    for (auto const& [responseId, button] : _responseButtons)
    {
      if (responseId == *_optDefaultResponseId)
      {
        set_default_widget(*button);
        return;
      }
    }
  }

  void AppDialog::configureMessageDialog(AppDialog& dialog,
                                         Gtk::Window& parent,
                                         std::string const& title,
                                         std::string const& message,
                                         std::vector<AppDialogAction> const& actions,
                                         std::int32_t defaultResponseId)
  {
    dialog.set_title(title);
    dialog.configureForParent(parent);
    dialog.set_default_size(-1, -1);

    auto* const box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, kMessageSpacing);
    box->set_size_request(kMessageMinWidth, -1);
    box->set_vexpand(false);

    auto* const messageLabel = Gtk::make_managed<Gtk::Label>(message);
    messageLabel->set_halign(Gtk::Align::START);
    messageLabel->set_xalign(0.0F);
    messageLabel->set_wrap(true);
    messageLabel->set_max_width_chars(kMessageMaxWidth / 8);
    box->append(*messageLabel);

    dialog.setContentWidget(*box);

    auto optCloseResponseId = std::optional<std::int32_t>{};

    for (auto const& action : actions)
    {
      dialog.addAction(action.label, action.responseId, action.role);

      if (action.role == AppDialogActionRole::Cancel && !optCloseResponseId)
      {
        optCloseResponseId = action.responseId;
      }
    }

    dialog.setDefaultResponse(defaultResponseId);
    dialog.setCloseResponse(optCloseResponseId.value_or(defaultResponseId));
  }
} // namespace ao::gtk
