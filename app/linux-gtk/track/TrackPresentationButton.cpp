// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "track/TrackPresentationButton.h"

#include "app/AppDialog.h"
#include "app/ThemeCoordinator.h"
#include "track/TrackCustomViewDialog.h"
#include <ao/rt/AppRuntime.h>
#include <ao/rt/Log.h>
#include <ao/rt/TrackPresentation.h>
#include <ao/rt/ViewService.h>
#include <ao/rt/WorkspaceService.h>
#include <ao/uimodel/library/presentation/ListPresentationPreferenceStore.h>
#include <ao/uimodel/library/presentation/TrackPresentationCatalog.h>
#include <ao/uimodel/library/presentation/TrackPresentationPickerViewModel.h>

#include <glibmm/main.h>
#include <gtkmm/button.h>
#include <gtkmm/dialog.h>
#include <gtkmm/enums.h>
#include <gtkmm/object.h>
#include <gtkmm/separator.h>
#include <gtkmm/window.h>

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace ao::gtk
{
  TrackPresentationButton::TrackPresentationButton(rt::AppRuntime& runtime)
    : _runtime{runtime}
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
                                                        uimodel::ListPresentationPreferenceStore* preferences,
                                                        ThemeCoordinator* themeCoordinator)
  {
    _catalog = catalog;
    _themeCoordinator = themeCoordinator;

    if (_catalog == nullptr || preferences == nullptr)
    {
      _viewModelPtr.reset();
      render({});
      return;
    }

    _viewModelPtr = std::make_unique<uimodel::TrackPresentationPickerViewModel>(
      _runtime.views(),
      _runtime.workspace(),
      *_catalog,
      *preferences,
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

    auto optSpec = _viewModelPtr->selectPresentation(presentationId);

    if (!optSpec)
    {
      return;
    }

    auto const targetViewId = _state.activeViewId;
    _applyPresentationConn.disconnect();
    _applyPresentationConn = Glib::signal_idle().connect(
      [this, targetViewId, spec = std::move(*optSpec)]
      {
        if (_runtime.workspace().snapshot().activeViewId != targetViewId)
        {
          auto const message = std::string{"The selected track view is no longer active."};
          APP_LOG_ERROR("Failed to apply track presentation: {}", message);
          showPresentationError(message);
          return false;
        }

        if (auto const result = _runtime.workspace().setActivePresentation(spec); !result)
        {
          APP_LOG_ERROR("Failed to apply track presentation: {}", result.error().message);
          showPresentationError(result.error().message);
        }

        return false;
      });
  }

  void TrackPresentationButton::showPresentationError(std::string_view message)
  {
    auto* const parentWindow = dynamic_cast<Gtk::Window*>(get_root());

    if (parentWindow == nullptr)
    {
      return;
    }

    auto* const dialog = AppDialog::presentMessage(
      *parentWindow,
      "Unable to Change Track View",
      std::string{message},
      {AppDialogAction{.label = "Close", .responseId = Gtk::ResponseType::CLOSE, .role = AppDialogActionRole::Cancel}},
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

    if (!_state.enabled || _catalog == nullptr || _viewModelPtr == nullptr)
    {
      return;
    }

    auto* parentWindow = dynamic_cast<Gtk::Window*>(_button.get_root());

    if (parentWindow == nullptr)
    {
      return;
    }

    auto const state = _runtime.views().trackListState(_state.activeViewId);
    auto const& spec = state.presentation;

    auto const label = std::string{_button.get_label()} + " Copy";
    auto dialog = TrackCustomViewDialog{*parentWindow, spec, label};
    auto optToken = std::optional<ThemeRegistrationToken>{};

    if (_themeCoordinator != nullptr)
    {
      optToken = _themeCoordinator->registerToplevel(dialog);
    }

    if (auto const optPreset = dialog.runDialog(); optPreset)
    {
      _catalog->addCustomPresentation(*optPreset);
      handlePresentationSelected(optPreset->spec.id);
    }
  }
} // namespace ao::gtk
