// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "preferences/PreferencesWindow.h"

#include "app/FormBuilder.h"
#include "playback/OutputDeviceSelector.h"
#include "preferences/ShortcutEditorWidget.h"
#include <ao/rt/AppPrefsState.h>
#include <ao/rt/PlaybackService.h>
#include <ao/rt/PlaybackState.h>
#include <ao/uimodel/input/KeymapModel.h>
#include <ao/uimodel/layout/action/LayoutActionCatalog.h>
#include <ao/uimodel/playback/output/OutputDeviceViewModel.h>
#include <ao/uimodel/preferences/PreferencesModel.h>

#include <gtkmm/application.h> // NOLINT(misc-include-cleaner)
#include <gtkmm/box.h>
#include <gtkmm/button.h>
#include <gtkmm/comboboxtext.h>
#include <gtkmm/enums.h>
#include <gtkmm/label.h>
#include <gtkmm/menubutton.h>
#include <gtkmm/object.h>
#include <gtkmm/stack.h>
#include <gtkmm/stacksidebar.h>
#include <gtkmm/window.h>

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

namespace ao::gtk
{
  namespace
  {
    constexpr auto kDefaultWindowWidth = 760;
    constexpr auto kDefaultWindowHeight = 560;
    constexpr auto kPageMargin = 16;
    constexpr auto kSidebarWidth = 180;
    constexpr auto kClassicLayoutPresetId = std::string_view{"classic"};
    constexpr auto kModernLayoutPresetId = std::string_view{"modern"};

    Gtk::Label& placeholderLabel(std::string_view const text)
    {
      auto* const label = Gtk::make_managed<Gtk::Label>(std::string{text});
      label->set_xalign(0.0F);
      label->add_css_class("title-4");
      return *label;
    }

    std::string normalizedLayoutPresetId(std::string_view const presetId)
    {
      if (presetId == kModernLayoutPresetId)
      {
        return std::string{kModernLayoutPresetId};
      }

      return std::string{kClassicLayoutPresetId};
    }
  } // namespace

  PreferencesWindow::PreferencesWindow(Callbacks callbacks)
    : _callbacks{std::move(callbacks)}
  {
    set_title("Preferences");
    set_default_size(kDefaultWindowWidth, kDefaultWindowHeight);

    _sidebar.set_stack(_stack);
    _sidebar.set_size_request(kSidebarWidth, -1);
    _root.append(_sidebar);

    _stack.set_hexpand(true);
    _stack.set_vexpand(true);
    _root.append(_stack);

    set_child(_root);

    addPage("general", "General").append(placeholderLabel("General"));
    addPage("appearance", "Appearance");
    addPage("playback", "Playback/Output");
    addPage("layout", "Layout");
    addPage("keyboard", "Keyboard");

    buildAppearancePage();
    buildPlaybackPage();
    buildLayoutPage();

    signal_close_request().connect(
      [this]
      {
        dismiss();
        return true;
      },
      false);
  }

  PreferencesWindow::~PreferencesWindow()
  {
    clearKeyboardPage();
    clearWindowScopedState();
  }

  void PreferencesWindow::setThemeForTest(std::string_view const themeId)
  {
    _themeCombo.set_active_id(std::string{themeId});
  }

  void PreferencesWindow::setLayoutPresetForTest(std::string_view const presetId)
  {
    _layoutPresetCombo.set_active_id(std::string{presetId});
  }

  Gtk::Box& PreferencesWindow::addPage(std::string_view const name, std::string_view const title)
  {
    Gtk::Box* page = nullptr;

    if (name == "general")
    {
      page = &_generalPage;
    }
    else if (name == "appearance")
    {
      page = &_appearancePage;
    }
    else if (name == "playback")
    {
      page = &_playbackPage;
    }
    else if (name == "layout")
    {
      page = &_layoutPage;
    }
    else
    {
      page = &_keyboardPage;
    }

    page->set_margin(kPageMargin);
    page->set_vexpand(true);
    _stack.add(*page, std::string{name}, std::string{title});
    return *page;
  }

  void PreferencesWindow::buildAppearancePage()
  {
    _themeCombo.append(std::string{rt::themePresetToString(rt::ThemePresetId::Classic)}, "Classic");
    _themeCombo.append(std::string{rt::themePresetToString(rt::ThemePresetId::Modern)}, "Modern");
    _themeCombo.signal_changed().connect([this] { onThemeChanged(); });

    auto* const list = Gtk::make_managed<FormBoxedList>();
    list->addRow("Theme", _themeCombo);
    _appearancePage.append(*list);
  }

  void PreferencesWindow::buildPlaybackPage()
  {
    _outputDeviceLabel.set_text("Choose Output Device...");
    _outputDeviceButton.set_child(_outputDeviceLabel);

    auto* const list = Gtk::make_managed<FormBoxedList>();
    list->addRow("Output device", _outputDeviceButton);
    _playbackPage.append(*list);
  }

  void PreferencesWindow::buildLayoutPage()
  {
    auto* const list = Gtk::make_managed<FormBoxedList>();

    _layoutPresetCombo.append(std::string{kClassicLayoutPresetId}, "Classic");
    _layoutPresetCombo.append(std::string{kModernLayoutPresetId}, "Modern");
    _layoutPresetCombo.signal_changed().connect([this] { onLayoutPresetChanged(); });
    list->addRow("Default preset", _layoutPresetCombo);

    auto* const editLayoutButton = Gtk::make_managed<Gtk::Button>("Edit Layout...");
    editLayoutButton->signal_clicked().connect(
      [this]
      {
        if (_callbacks.onEditLayout)
        {
          _callbacks.onEditLayout();
        }
      });
    list->addRow("Editor", *editLayoutButton);

    auto* const savePanelsButton = Gtk::make_managed<Gtk::Button>("Save Current Panel Sizes as Layout Defaults");
    savePanelsButton->signal_clicked().connect(
      [this]
      {
        if (_callbacks.onSaveCurrentPanelSizesAsLayoutDefaults)
        {
          _callbacks.onSaveCurrentPanelSizesAsLayoutDefaults();
        }
      });
    list->addRow("Panel sizes", *savePanelsButton);

    auto* const resetRuntimeButton = Gtk::make_managed<Gtk::Button>("Reset Runtime Layout State");
    resetRuntimeButton->signal_clicked().connect(
      [this]
      {
        if (_callbacks.onResetRuntimeLayoutState)
        {
          _callbacks.onResetRuntimeLayoutState();
        }
      });
    list->addRow("Runtime state", *resetRuntimeButton);

    _layoutPage.append(*list);
  }

  void PreferencesWindow::refreshKeyboardPage(uimodel::LayoutActionCatalog const& catalog,
                                              uimodel::KeymapModel keymap,
                                              ShortcutEditorWidget::ChangedCallback onChanged)
  {
    clearKeyboardPage();
    _shortcutEditorPtr =
      std::make_unique<ShortcutEditorWidget>(catalog, std::move(keymap), std::move(onChanged), *this);
    _shortcutEditorPtr->set_hexpand(true);
    _shortcutEditorPtr->set_vexpand(true);
    _keyboardPage.append(*_shortcutEditorPtr);
  }

  void PreferencesWindow::refreshPreferences(rt::AppPrefsState prefs,
                                             rt::PlaybackService* playback,
                                             Gtk::Window* targetWindow)
  {
    _modelPtr = std::make_unique<uimodel::PreferencesModel>(std::move(prefs),
                                                            _callbacks.onPersistPreferences,
                                                            _callbacks.onApplyTheme,
                                                            uimodel::PreferencesModel::OutputApplyCallback{});

    _refreshing = true;
    _themeCombo.set_active_id(
      std::string{rt::themePresetToString(rt::themePresetFromString(_modelPtr->preferences().lastThemePreset))});
    _layoutPresetCombo.set_active_id(normalizedLayoutPresetId(_modelPtr->preferences().lastLayoutPreset));
    _refreshing = false;

    rebuildOutputSelector(playback, targetWindow);
  }

  void PreferencesWindow::dismiss()
  {
    clearKeyboardPage();
    clearWindowScopedState();
    set_visible(false);

    if (auto const appPtr = get_application(); appPtr)
    {
      appPtr->remove_window(*this);
    }
  }

  void PreferencesWindow::clearWindowScopedState()
  {
    _targetHideConn.disconnect();
    _outputDeviceButton.unset_popover();
    _outputDeviceLabel.set_text("Unavailable");
    _outputDeviceButton.set_tooltip_text({});
  }

  void PreferencesWindow::clearKeyboardPage()
  {
    if (_shortcutEditorPtr)
    {
      _keyboardPage.remove(*_shortcutEditorPtr);
      _shortcutEditorPtr.reset();
    }
  }

  bool PreferencesWindow::hasPageForTest(std::string_view const name) const
  {
    return _stack.get_child_by_name(std::string{name}) != nullptr;
  }

  bool PreferencesWindow::hasOutputSelectorForTest() const
  {
    return _outputDeviceButton.get_popover() != nullptr;
  }

  void PreferencesWindow::onLayoutPresetChanged()
  {
    if (_refreshing || !_modelPtr)
    {
      return;
    }

    auto const presetId = _layoutPresetCombo.get_active_id();

    if (presetId.empty())
    {
      return;
    }

    _modelPtr->setLayoutPreset(presetId.raw());
  }

  void PreferencesWindow::onThemeChanged()
  {
    if (_refreshing || !_modelPtr)
    {
      return;
    }

    auto const themeId = _themeCombo.get_active_id();

    if (themeId.empty())
    {
      return;
    }

    _modelPtr->setTheme(rt::themePresetFromString(themeId.raw()));
  }

  void PreferencesWindow::refreshOutputSummary(rt::PlaybackService& playback)
  {
    auto viewModel = uimodel::OutputDeviceViewModel{playback,
                                                    [this](uimodel::OutputDeviceViewState const& view)
                                                    {
                                                      _outputDeviceLabel.set_text(view.outputBackendSummary.empty()
                                                                                    ? "Choose Output Device..."
                                                                                    : view.outputBackendSummary);
                                                      _outputDeviceButton.set_tooltip_text(view.outputDeviceStatus);
                                                    }};
    viewModel.refresh();
  }

  void PreferencesWindow::rebuildOutputSelector(rt::PlaybackService* playback, Gtk::Window* targetWindow)
  {
    clearWindowScopedState();

    if (playback == nullptr || !_modelPtr)
    {
      return;
    }

    if (targetWindow != nullptr)
    {
      _targetHideConn = targetWindow->signal_hide().connect([this] { dismiss(); });
    }

    refreshOutputSummary(*playback);

    auto* const selector =
      Gtk::make_managed<OutputDeviceSelector>(*playback,
                                              Gtk::PositionType::BOTTOM,
                                              [this, playback](rt::OutputDeviceSelection const& selection)
                                              {
                                                if (_modelPtr)
                                                {
                                                  _modelPtr->setOutputDeviceConfirmed(selection);
                                                }

                                                refreshOutputSummary(*playback);
                                              });
    _outputDeviceButton.set_popover(*selector);
  }
} // namespace ao::gtk
