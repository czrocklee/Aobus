// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "preference/ShortcutEditorWidget.h"
#include <ao/rt/AppPrefsState.h>
#include <ao/uimodel/input/KeymapModel.h>
#include <ao/uimodel/preference/PreferencesEditorModel.h>

#include <gtkmm/box.h>
#include <gtkmm/comboboxtext.h>
#include <gtkmm/enums.h>
#include <gtkmm/label.h>
#include <gtkmm/menubutton.h>
#include <gtkmm/popover.h>
#include <gtkmm/stack.h>
#include <gtkmm/stacksidebar.h>
#include <gtkmm/window.h>
#include <sigc++/connection.h>
#include <sigc++/scoped_connection.h>

#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace ao::uimodel
{
  class LayoutActionCatalog;
  class OutputDeviceViewModel;
}
namespace ao::rt
{
  class PlaybackService;
}

namespace ao::gtk
{
  class PreferencesWindow final : public Gtk::Window
  {
  public:
    struct Callbacks final
    {
      std::function<void()> onEditLayout{};
      std::function<void()> onResetRuntimeLayoutState{};
      std::function<void()> onSaveCurrentPanelSizesAsLayoutDefaults{};
      uimodel::PreferencesEditorModel::PersistCallback onPersistPreferences{};
      uimodel::PreferencesEditorModel::ThemeApplyCallback onApplyTheme{};
    };

    explicit PreferencesWindow(Callbacks callbacks);
    ~PreferencesWindow() override;

    PreferencesWindow(PreferencesWindow const&) = delete;
    PreferencesWindow& operator=(PreferencesWindow const&) = delete;
    PreferencesWindow(PreferencesWindow&&) = delete;
    PreferencesWindow& operator=(PreferencesWindow&&) = delete;

    void refreshKeyboardPage(uimodel::LayoutActionCatalog const& catalog,
                             uimodel::KeymapModel keymap,
                             ShortcutEditorWidget::ChangedCallback onChanged);
    void refreshPreferences(rt::AppPrefsState prefs,
                            rt::PlaybackService* playback,
                            Gtk::Window* targetWindow = nullptr);

    bool hasPage(std::string_view name) const;
    std::string selectedThemeId() const { return _themeCombo.get_active_id(); }
    void setSelectedThemeId(std::string_view themeId);
    std::string selectedLayoutPresetId() const { return _layoutPresetCombo.get_active_id(); }
    void setSelectedLayoutPresetId(std::string_view presetId);
    std::string outputDeviceLabelText() const { return _outputDeviceLabel.get_text(); }
    bool hasOutputSelector() const;
    Gtk::Popover* outputSelector() { return _outputDeviceButton.get_popover(); }
    Gtk::Popover const* outputSelector() const { return _outputDeviceButton.get_popover(); }

  private:
    Gtk::Box& addPage(std::string_view name, std::string_view title);
    void buildAppearancePage();
    void buildPlaybackPage();
    void buildLayoutPage();
    void dismiss();
    void clearWindowScopedState();
    void clearKeyboardPage();
    void handleLayoutPresetChanged();
    void handleThemeChanged();
    void refreshOutputSummary(rt::PlaybackService& playback);
    void rebuildOutputSelector(rt::PlaybackService* playback, Gtk::Window* targetWindow);

    Callbacks _callbacks;
    std::unique_ptr<uimodel::PreferencesEditorModel> _modelPtr;
    sigc::scoped_connection _targetHideConn;
    sigc::connection _themeComboConn;
    sigc::connection _layoutPresetComboConn;

    static constexpr int kPageSpacing = 12;

    Gtk::Box _root{Gtk::Orientation::HORIZONTAL, 0};
    Gtk::StackSidebar _sidebar;
    Gtk::Stack _stack;

    Gtk::Box _generalPage{Gtk::Orientation::VERTICAL, kPageSpacing};
    Gtk::Box _appearancePage{Gtk::Orientation::VERTICAL, kPageSpacing};
    Gtk::Box _playbackPage{Gtk::Orientation::VERTICAL, kPageSpacing};
    Gtk::Box _layoutPage{Gtk::Orientation::VERTICAL, kPageSpacing};
    Gtk::Box _keyboardPage{Gtk::Orientation::VERTICAL, kPageSpacing};
    Gtk::ComboBoxText _themeCombo;
    Gtk::ComboBoxText _layoutPresetCombo;
    Gtk::MenuButton _outputDeviceButton;
    Gtk::Label _outputDeviceLabel;
    std::unique_ptr<ShortcutEditorWidget> _shortcutEditorPtr;
    std::unique_ptr<uimodel::OutputDeviceViewModel> _outputDeviceViewModelPtr;
  };
} // namespace ao::gtk
