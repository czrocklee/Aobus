// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "track/TrackPresentationButton.h"

#include "app/AppDialog.h"
#include "app/ThemeCoordinator.h"
#include "i18n/GtkText.h"
#include "track/TrackCustomViewDialog.h"
#include <ao/Error.h>
#include <ao/i18n/MessageCatalog.h>
#include <ao/rt/Log.h>
#include <ao/rt/TrackPresentation.h>
#include <ao/rt/ViewIds.h>
#include <ao/rt/ViewService.h>
#include <ao/rt/WorkspaceService.h>
#include <ao/uimodel/library/presentation/ListPresentations.h>
#include <ao/uimodel/library/presentation/TrackPresentationCatalog.h>
#include <ao/uimodel/library/presentation/TrackPresentationPickerViewModel.h>

#include <glibmm/main.h>
#include <gtkmm/button.h>
#include <gtkmm/dialog.h>
#include <gtkmm/enums.h>
#include <gtkmm/object.h>
#include <gtkmm/separator.h>
#include <gtkmm/window.h>
#include <sigc++/scoped_connection.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace ao::gtk
{
  using i18n::MessageId;

  struct TrackPresentationButton::CustomViewEditor final
  {
    CustomViewEditor(Gtk::Window& parent,
                     i18n::MessageCatalog const& textCatalog,
                     rt::TrackPresentationSpec const& spec,
                     std::string_view label,
                     rt::ViewId viewId)
      : dialog{parent, textCatalog, spec, label}, targetViewId{viewId}
    {
    }

    TrackCustomViewDialog dialog;
    rt::ViewId targetViewId;
    ThemeRegistrationToken theme;
    sigc::scoped_connection response;
    sigc::scoped_connection hide;
    sigc::scoped_connection pickerRoot;
    sigc::scoped_connection parentUnmap;
    sigc::scoped_connection finish;
  };

  TrackPresentationButton::TrackPresentationButton(rt::ViewService& views,
                                                   rt::WorkspaceService& workspace,
                                                   i18n::MessageCatalog textCatalog)
    : _views{views}, _workspace{workspace}, _textCatalog{std::move(textCatalog)}
  {
    set_valign(Gtk::Align::CENTER);

    _button.set_has_frame(false);
    _button.add_css_class("flat");
    _button.set_valign(Gtk::Align::CENTER);
    _button.add_css_class("ao-presentation-trigger");
    _popover.set_has_arrow(false);
    _popover.add_css_class("ao-presentation-popover");
    _popover.set_child(_menuBox);
    _button.set_popover(_popover);

    _menuBox.add_css_class("ao-presentation-menu-box");

    append(_button);
    render(_state);
  }

  TrackPresentationButton::~TrackPresentationButton() = default;

  void TrackPresentationButton::setPresentationServices(uimodel::TrackPresentationCatalog* catalog,
                                                        uimodel::ListPresentations* listPresentations,
                                                        ThemeCoordinator* themeCoordinator)
  {
    // A pending apply belongs to the outgoing view model's session. Left
    // connected it would apply a stale selection to the runtime and then hand
    // the outgoing list id to whatever preference store is bound by then.
    _applyPresentationConn.disconnect();
    _optBindingCallbacks.reset();
    _customViewEditorPtr.reset();

    _catalog = catalog;
    _themeCoordinator = themeCoordinator;

    if (_catalog == nullptr || listPresentations == nullptr)
    {
      _viewModelPtr.reset();
      render({});
      return;
    }

    _optBindingCallbacks.emplace();
    _viewModelPtr = std::make_unique<uimodel::TrackPresentationPickerViewModel>(
      _views,
      _workspace,
      *_catalog,
      *listPresentations,
      _textCatalog,
      [this](uimodel::TrackPresentationPickerState const& state) { render(state); });
    _viewModelPtr->refresh();
  }

  void TrackPresentationButton::render(uimodel::TrackPresentationPickerState const& state)
  {
    _state = state;
    _button.set_sensitive(_state.enabled);
    _button.set_label(_state.label);
    populatePresentationOptions(_state);
  }

  void TrackPresentationButton::populatePresentationOptions(uimodel::TrackPresentationPickerState const& state)
  {
    auto* child = _menuBox.get_first_child();

    while (child != nullptr)
    {
      auto* const next = child->get_next_sibling();
      _menuBox.remove(*child);
      child = next;
    }

    for (auto const& item : state.menuItems)
    {
      if (item.type == uimodel::TrackPresentationMenuItemType::Separator)
      {
        auto* const sep = Gtk::make_managed<Gtk::Separator>(Gtk::Orientation::HORIZONTAL);
        sep->add_css_class("ao-presentation-menu-separator");
        _menuBox.append(*sep);
        continue;
      }

      auto* const btn = Gtk::make_managed<Gtk::Button>(item.label);
      btn->set_halign(Gtk::Align::FILL);
      btn->set_has_frame(false);
      btn->set_sensitive(item.enabled);

      if (!item.disabledReason.empty())
      {
        btn->set_tooltip_text(item.disabledReason);
      }

      btn->add_css_class("ao-presentation-menu-item");

      if (item.type == uimodel::TrackPresentationMenuItemType::CreateCustomView)
      {
        btn->signal_clicked().connect([this] { handleCreateCustomViewClicked(); });
      }
      else
      {
        auto const id = item.id;
        btn->signal_clicked().connect([this, id] { handlePresentationSelected(id); });
      }

      _menuBox.append(*btn);
    }
  }

  void TrackPresentationButton::handlePresentationSelected(std::string_view presentationId)
  {
    _popover.popdown();

    if (_viewModelPtr == nullptr)
    {
      return;
    }

    auto optSelection = _viewModelPtr->selectPresentation(presentationId);

    if (!optSelection)
    {
      return;
    }

    _applyPresentationConn.disconnect();
    _applyPresentationConn = Glib::signal_idle().connect(
      [this, selection = std::move(*optSelection)]
      {
        applyPresentation(selection);
        return false;
      });
  }

  void TrackPresentationButton::applyPresentation(uimodel::TrackPresentationSelection const& selection)
  {
    if (!_optBindingCallbacks)
    {
      return;
    }

    if (_workspace.snapshot().activeViewId != selection.targetViewId)
    {
      auto const message = gtkText(_textCatalog, MessageId::GtkPresentationViewInactive);
      APP_LOG_ERROR("Failed to apply track presentation: {}", message);
      showPresentationError(message);
      return;
    }

    auto finish = _optBindingCallbacks->guard(
      [this, &selection](Result<> const& res)
      {
        if (!res)
        {
          APP_LOG_ERROR("Failed to apply track presentation: {}", res.error().message);
          showPresentationError(res.error().message);
          return;
        }

        _viewModelPtr->completeSelection(selection);
      });
    finish(_workspace.setActivePresentation(selection.spec));
  }

  void TrackPresentationButton::showPresentationError(std::string_view message)
  {
    auto* const parentWindow = dynamic_cast<Gtk::Window*>(get_root());

    if (parentWindow == nullptr)
    {
      return;
    }

    auto* const dialog =
      AppDialog::presentMessage(*parentWindow,
                                gtkText(_textCatalog, MessageId::GtkPresentationUnableToChange),
                                std::string{message},
                                {AppDialogAction{.label = gtkText(_textCatalog, MessageId::GtkCommonClose),
                                                 .responseId = Gtk::ResponseType::CLOSE,
                                                 .role = AppDialogActionRole::Cancel}},
                                Gtk::ResponseType::CLOSE);

    if (_themeCoordinator != nullptr)
    {
      auto tokenPtr = std::make_shared<ThemeRegistrationToken>(_themeCoordinator->registerToplevel(*dialog));
      dialog->signal_hide().connect([tokenPtr] { (*tokenPtr).reset(); });
    }
  }

  void TrackPresentationButton::handleCreateCustomViewClicked()
  {
    _popover.popdown();

    if (_customViewEditorPtr || !_state.enabled || _viewModelPtr == nullptr)
    {
      return;
    }

    auto* parentWindow = dynamic_cast<Gtk::Window*>(_button.get_root());

    if (parentWindow == nullptr)
    {
      return;
    }

    // _state is a cached render snapshot, so its view id can be stale by the
    // time the user clicks.
    auto const targetViewId = _workspace.snapshot().activeViewId;
    auto const foundRes = _views.findTrackListState(targetViewId);

    if (!foundRes || targetViewId != _state.activeViewId)
    {
      return;
    }

    auto const& spec = foundRes->presentation;

    auto const labelText = _catalog->labelForId(spec.id);
    auto const label = i18n::requiredFormat(_textCatalog, MessageId::GtkPresentationCopyLabel, {{"label", labelText}});
    _customViewEditorPtr = std::make_unique<CustomViewEditor>(*parentWindow, _textCatalog, spec, label, targetViewId);
    auto& editor = *_customViewEditorPtr;

    if (_themeCoordinator != nullptr)
    {
      editor.theme = _themeCoordinator->registerToplevel(editor.dialog);
    }

    editor.response = editor.dialog.signal_response().connect([this](std::int32_t id)
                                                              { queueCustomViewFinish(id == Gtk::ResponseType::OK); });
    editor.hide = editor.dialog.signal_hide().connect([this] { queueCustomViewFinish(false); });
    // The modal dialog belongs to this window, not to the picker's visibility.
    editor.pickerRoot = property_root().signal_changed().connect([this] { queueCustomViewFinish(false); });
    editor.parentUnmap = parentWindow->signal_unmap().connect([this] { queueCustomViewFinish(false); });
    editor.dialog.present();
  }

  void TrackPresentationButton::queueCustomViewFinish(bool const save)
  {
    auto& editor = *_customViewEditorPtr;

    if (save && editor.finish.connected())
    {
      // The first Save owns the draft, but Cancel may still revoke it before the idle commits.
      return;
    }

    if (!save)
    {
      editor.response.disconnect();
    }

    editor.finish.disconnect();
    auto optPreset = save ? std::optional{editor.dialog.collectState()} : std::nullopt;

    // Never destroy the dialog on its own response/hide stack.
    editor.finish = Glib::signal_idle().connect(
      [this, optPreset = std::move(optPreset)]
      {
        auto retiringPtr = std::move(_customViewEditorPtr);
        retiringPtr->response.disconnect();
        retiringPtr->hide.disconnect();
        retiringPtr->pickerRoot.disconnect();
        retiringPtr->parentUnmap.disconnect();

        if (auto const targetViewId = retiringPtr->targetViewId;
            optPreset && _optBindingCallbacks && _workspace.snapshot().activeViewId == targetViewId)
        {
          // A stale draft is discarded before publish. Once it is published,
          // a change of binding or view may skip selection but does not undo the preset.
          auto selectIfCurrent = _optBindingCallbacks->guard(
            [this, targetViewId](std::string_view id)
            {
              if (auto const optSelection = _viewModelPtr->selectPresentation(id);
                  optSelection && optSelection->targetViewId == targetViewId)
              {
                applyPresentation(*optSelection);
              }
            });
          _catalog->addCustomPresentation(*optPreset);
          selectIfCurrent(optPreset->spec.id);
        }

        return false;
      });
  }
} // namespace ao::gtk
