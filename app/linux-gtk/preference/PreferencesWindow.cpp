// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "preference/PreferencesWindow.h"

#include "app/AppDialog.h"
#include "app/FormBuilder.h"
#include "i18n/GtkText.h"
#include "layout/document/LayoutPresets.h"
#include "playback/OutputDevicePopover.h"
#include "preference/ShortcutEditorWidget.h"
#include <ao/audio/OutputDeviceSelection.h>
#include <ao/i18n/MessageCatalog.h>
#include <ao/rt/AppState.h>
#include <ao/rt/playback/PlaybackService.h>
#include <ao/uimodel/input/KeymapModel.h>
#include <ao/uimodel/layout/component/LayoutSchema.h>
#include <ao/uimodel/layout/shell/LayoutSession.h>
#include <ao/uimodel/playback/output/OutputDeviceIntent.h>
#include <ao/uimodel/playback/output/OutputDeviceViewModel.h>
#include <ao/uimodel/preference/PreferencesEditorModel.h>
#include <ao/uimodel/preference/ThemePreset.h>

// Gtk::Window forward-declares Application, but remove_window requires the complete type.
// NOLINTNEXTLINE(misc-include-cleaner)
#include <gtkmm/application.h>
#include <gtkmm/box.h>
#include <gtkmm/button.h>
#include <gtkmm/comboboxtext.h>
#include <gtkmm/dialog.h>
#include <gtkmm/enums.h>
#include <gtkmm/label.h>
#include <gtkmm/menubutton.h>
#include <gtkmm/object.h>
#include <gtkmm/scrolledwindow.h>
#include <gtkmm/stack.h>
#include <gtkmm/stacksidebar.h>
#include <gtkmm/window.h>
#include <sigc++/connection.h>
#include <sigc++/functors/mem_fun.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

namespace ao::gtk
{
  namespace
  {
    using i18n::MessageId;

    constexpr auto kDefaultWindowWidth = 760;
    constexpr auto kDefaultWindowHeight = 560;
    constexpr auto kPageMargin = 16;
    constexpr auto kSidebarWidth = 180;
    Gtk::Label& placeholderLabel(std::string_view const text)
    {
      auto* const label = Gtk::make_managed<Gtk::Label>(std::string{text});
      label->set_xalign(0.0F);
      label->add_css_class("title-4");
      return *label;
    }

    std::string normalizedLayoutPresetId(std::string_view const presetId)
    {
      return std::string{layout::presetIdToString(layout::presetIdFromString(presetId))};
    }

    struct [[nodiscard]] ConnectionBlocker final
    {
      explicit ConnectionBlocker(sigc::connection& targetConn)
        : conn{targetConn}
      {
        conn.block();
      }
      ~ConnectionBlocker() { conn.unblock(); }

      ConnectionBlocker(ConnectionBlocker const&) = delete;
      ConnectionBlocker& operator=(ConnectionBlocker const&) = delete;
      ConnectionBlocker(ConnectionBlocker&&) = delete;
      ConnectionBlocker& operator=(ConnectionBlocker&&) = delete;

      sigc::connection& conn;
    };
  } // namespace

  PreferencesWindow::PreferencesWindow(i18n::MessageCatalog textCatalog, Callbacks callbacks)
    : _callbacks{std::move(callbacks)}, _textCatalog{std::move(textCatalog)}
  {
    set_title(gtkText(_textCatalog, MessageId::GtkPreferencesTitle));
    set_default_size(kDefaultWindowWidth, kDefaultWindowHeight);

    _sidebar.set_stack(_stack);
    _sidebar.set_size_request(kSidebarWidth, -1);
    _root.append(_sidebar);

    _stack.set_hexpand(true);
    _stack.set_vexpand(true);
    _root.append(_stack);

    set_child(_root);

    auto const general = gtkText(_textCatalog, MessageId::GtkPreferencesPageGeneral);
    addPage("general", general).append(placeholderLabel(general));
    addPage("appearance", gtkText(_textCatalog, MessageId::GtkPreferencesPageAppearance));
    addPage("playback", gtkText(_textCatalog, MessageId::GtkPreferencesPagePlaybackOutput));
    addPage("layout", gtkText(_textCatalog, MessageId::GtkPreferencesPageLayout));
    addPage("keyboard", gtkText(_textCatalog, MessageId::GtkPreferencesPageKeyboard));

    buildAppearancePage();
    buildPlaybackPage();
    buildLayoutPage();

    signal_close_request().connect(
      [this]
      {
        if (_shortcutEditorPtr != nullptr && _shortcutEditorPtr->hasPendingCandidate())
        {
          promptPendingShortcutClose();
          return true;
        }

        dismiss();
        return true;
      },
      false);
  }

  PreferencesWindow::~PreferencesWindow()
  {
    _callbackScope.close();
    clearKeyboardPage();
    clearWindowScopedState();
  }

  void PreferencesWindow::setSelectedThemeId(std::string_view const themeId)
  {
    _themeCombo.set_active_id(std::string{themeId});
  }

  void PreferencesWindow::setSelectedLayoutPresetId(std::string_view const presetId)
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

    auto* const scroller = Gtk::make_managed<Gtk::ScrolledWindow>();
    scroller->set_child(*page);
    scroller->set_policy(Gtk::PolicyType::NEVER, Gtk::PolicyType::AUTOMATIC);
    scroller->set_propagate_natural_width(true);

    _stack.add(*scroller, std::string{name}, std::string{title});
    return *page;
  }

  void PreferencesWindow::buildAppearancePage()
  {
    _themeCombo.append(std::string{uimodel::themePresetId(uimodel::ThemePreset::Classic)},
                       gtkText(_textCatalog, MessageId::GtkPreferencesThemeClassic));
    _themeCombo.append(std::string{uimodel::themePresetId(uimodel::ThemePreset::Modern)},
                       gtkText(_textCatalog, MessageId::GtkPreferencesThemeModern));
    _themeComboConn = _themeCombo.signal_changed().connect([this] { handleThemeChanged(); });

    auto* const list = Gtk::make_managed<FormBoxedList>();
    list->addRow(gtkText(_textCatalog, MessageId::GtkPreferencesTheme), _themeCombo);
    _appearancePage.append(*list);
  }

  void PreferencesWindow::buildPlaybackPage()
  {
    _outputDeviceLabel.set_text(gtkText(_textCatalog, MessageId::GtkPreferencesChooseOutputDevice));
    _outputDeviceButton.set_child(_outputDeviceLabel);

    auto* const list = Gtk::make_managed<FormBoxedList>();
    list->addRow(gtkText(_textCatalog, MessageId::GtkPreferencesOutputDevice), _outputDeviceButton);
    _playbackPage.append(*list);
  }

  void PreferencesWindow::buildLayoutPage()
  {
    auto* const list = Gtk::make_managed<FormBoxedList>();

    _layoutPresetCombo.append(std::string{uimodel::LayoutSession::kDefaultPresetId},
                              gtkText(_textCatalog, MessageId::GtkPreferencesLayoutPresetClassic));
    _layoutPresetCombo.append(std::string{layout::presetIdToString(layout::LayoutPresetId::Modern)},
                              gtkText(_textCatalog, MessageId::GtkPreferencesLayoutPresetModern));
    _layoutPresetComboConn = _layoutPresetCombo.signal_changed().connect([this] { handleLayoutPresetChanged(); });
    list->addRow(gtkText(_textCatalog, MessageId::GtkPreferencesDefaultLayoutPreset), _layoutPresetCombo);

    _layoutPage.append(*list);

    auto* const actionsLabel = Gtk::make_managed<Gtk::Label>(gtkText(_textCatalog, MessageId::GtkPreferencesActions));
    actionsLabel->set_xalign(0.0F);
    actionsLabel->add_css_class("title-4");
    actionsLabel->set_margin_top(16);
    _layoutPage.append(*actionsLabel);

    auto* const actionsBox = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 8);

    auto* const editLayoutButton = Gtk::make_managed<Gtk::Button>(gtkText(_textCatalog, MessageId::GtkShellEditLayout));
    editLayoutButton->set_halign(Gtk::Align::START);
    editLayoutButton->signal_clicked().connect(
      [this]
      {
        if (_callbacks.onEditLayout)
        {
          _callbacks.onEditLayout();
        }
      });
    actionsBox->append(*editLayoutButton);

    auto* const savePanelsButton =
      Gtk::make_managed<Gtk::Button>(gtkText(_textCatalog, MessageId::GtkShellSavePanelSizesAsLayoutDefaults));
    savePanelsButton->set_halign(Gtk::Align::START);
    savePanelsButton->signal_clicked().connect(
      [this]
      {
        if (_callbacks.onSaveCurrentPanelSizesAsLayoutDefaults)
        {
          _callbacks.onSaveCurrentPanelSizesAsLayoutDefaults();
        }
      });
    actionsBox->append(*savePanelsButton);

    auto* const resetRuntimeButton =
      Gtk::make_managed<Gtk::Button>(gtkText(_textCatalog, MessageId::GtkShellResetRuntimeLayoutState));
    resetRuntimeButton->set_halign(Gtk::Align::START);
    resetRuntimeButton->signal_clicked().connect(
      [this]
      {
        if (_callbacks.onResetRuntimeLayoutState)
        {
          _callbacks.onResetRuntimeLayoutState();
        }
      });
    actionsBox->append(*resetRuntimeButton);

    _layoutPage.append(*actionsBox);
  }

  void PreferencesWindow::refreshKeyboardPage(uimodel::LayoutSchema const& schema,
                                              uimodel::KeymapModel keymap,
                                              ShortcutEditorWidget::ChangedCallback onChanged)
  {
    if (_shortcutEditorPtr != nullptr && _shortcutEditorPtr->hasPendingCandidate())
    {
      return;
    }

    clearKeyboardPage();
    _shortcutEditorPtr =
      std::make_unique<ShortcutEditorWidget>(_textCatalog, schema, std::move(keymap), std::move(onChanged), *this);
    _shortcutEditorPtr->set_hexpand(true);
    _shortcutEditorPtr->set_vexpand(true);
    _keyboardPage.append(*_shortcutEditorPtr);
  }

  void PreferencesWindow::refreshPreferences(rt::AppPrefsState prefs,
                                             rt::PlaybackService* playback,
                                             Gtk::Window* targetWindow)
  {
    // The output popover applies through Playback.
    // This model persists the exact requested preference independently.
    _modelPtr =
      std::make_unique<uimodel::PreferencesEditorModel>(std::move(prefs),
                                                        _callbacks.onPersistPreferences,
                                                        _callbacks.onApplyTheme,
                                                        uimodel::PreferencesEditorModel::OutputApplyCallback{});

    auto const blockTheme = ConnectionBlocker{_themeComboConn};
    auto const blockLayout = ConnectionBlocker{_layoutPresetComboConn};

    _themeCombo.set_active_id(
      std::string{uimodel::themePresetId(uimodel::themePresetFromId(_modelPtr->preferences().lastThemePreset))});
    _layoutPresetCombo.set_active_id(normalizedLayoutPresetId(_modelPtr->preferences().lastLayoutPreset));

    rebuildOutputSelector(playback, targetWindow);
  }

  void PreferencesWindow::dismiss()
  {
    retirePendingClosePrompt();
    clearKeyboardPage();
    clearWindowScopedState();
    set_visible(false);

    if (auto const appPtr = get_application(); appPtr)
    {
      appPtr->remove_window(*this);
    }
  }

  void PreferencesWindow::retirePendingClosePrompt()
  {
    // Hiding rather than close(): close() re-enters the dialog's own response protocol, which would
    // deliver a synthetic response back into the handler below. Clearing the pointer is what makes
    // the retired prompt inert.
    if (auto* const prompt = std::exchange(_pendingClosePrompt, nullptr); prompt != nullptr)
    {
      prompt->set_visible(false);
    }
  }

  void PreferencesWindow::promptPendingShortcutClose()
  {
    // A second close request must not stack another modal on top of the live one.
    if (_pendingClosePrompt != nullptr)
    {
      _pendingClosePrompt->present();
      return;
    }

    // The failure banner lives on the Keyboard page; a retry that fails again has to be visible.
    _stack.set_visible_child("keyboard");

    _pendingClosePrompt =
      AppDialog::presentMessage(*this,
                                gtkText(_textCatalog, MessageId::GtkShortcutPendingCloseTitle),
                                gtkText(_textCatalog, MessageId::GtkShortcutPendingCloseMessage),
                                {AppDialogAction{.label = gtkText(_textCatalog, MessageId::GtkCommonCancel),
                                                 .responseId = Gtk::ResponseType::CANCEL,
                                                 .role = AppDialogActionRole::Cancel},
                                 AppDialogAction{.label = gtkText(_textCatalog, MessageId::GtkShortcutDiscard),
                                                 .responseId = Gtk::ResponseType::REJECT,
                                                 .role = AppDialogActionRole::Cancel},
                                 AppDialogAction{.label = gtkText(_textCatalog, MessageId::GtkShortcutRetry),
                                                 .responseId = Gtk::ResponseType::OK,
                                                 .role = AppDialogActionRole::Primary}},
                                Gtk::ResponseType::CANCEL,
                                _callbackScope.guard(
                                  [this](std::int32_t const responseId)
                                  {
                                    // A retired prompt (dismiss() already ran) must never steer the
                                    // session that replaced it. Only the live prompt owns this handler.
                                    if (_pendingClosePrompt == nullptr)
                                    {
                                      return;
                                    }

                                    _pendingClosePrompt = nullptr;

                                    if (_shortcutEditorPtr == nullptr)
                                    {
                                      return;
                                    }

                                    if (responseId == Gtk::ResponseType::REJECT)
                                    {
                                      _shortcutEditorPtr->discardPending();
                                      dismiss();
                                      return;
                                    }

                                    if (responseId == Gtk::ResponseType::OK && _shortcutEditorPtr->retryPending())
                                    {
                                      dismiss();
                                    }
                                  }));
  }

  void PreferencesWindow::clearWindowScopedState()
  {
    _targetHideConn.disconnect();
    _outputDeviceViewModelPtr.reset();
    _outputDeviceButton.unset_popover();
    _outputDeviceLabel.set_text(gtkText(_textCatalog, MessageId::GtkPreferencesOutputUnavailable));
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

  bool PreferencesWindow::hasPage(std::string_view const name) const
  {
    return _stack.get_child_by_name(std::string{name}) != nullptr;
  }

  bool PreferencesWindow::hasOutputSelector() const
  {
    return _outputDeviceButton.get_popover() != nullptr;
  }

  void PreferencesWindow::handleLayoutPresetChanged()
  {
    if (!_modelPtr)
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

  void PreferencesWindow::handleThemeChanged()
  {
    if (!_modelPtr)
    {
      return;
    }

    auto const themeId = _themeCombo.get_active_id();

    if (themeId.empty())
    {
      return;
    }

    _modelPtr->setTheme(uimodel::themePresetFromId(themeId.raw()));
  }

  void PreferencesWindow::refreshOutputSummary(rt::PlaybackService& playback)
  {
    _outputDeviceViewModelPtr = std::make_unique<uimodel::OutputDeviceViewModel>(
      playback,
      _textCatalog,
      [this](uimodel::OutputDeviceViewState const& view)
      {
        _outputDeviceLabel.set_text(view.outputBackendSummary.empty()
                                      ? gtkText(_textCatalog, MessageId::GtkPreferencesChooseOutputDevice)
                                      : view.outputBackendSummary);
        _outputDeviceButton.set_tooltip_text(view.outputDeviceStatus);
      },
      // The summary only reports the active route; the selector popover records requests.
      uimodel::OutputDeviceIntent::discarded());
    _outputDeviceViewModelPtr->refresh();
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
      _targetHideConn = targetWindow->signal_hide().connect(sigc::mem_fun(*this, &PreferencesWindow::dismiss));
    }

    refreshOutputSummary(*playback);

    auto* const selector =
      Gtk::make_managed<OutputDevicePopover>(*playback,
                                             _textCatalog,
                                             uimodel::OutputDeviceIntent::recordedBy(
                                               [this, playback](audio::OutputDeviceSelection const& selection)
                                               {
                                                 if (_modelPtr)
                                                 {
                                                   _modelPtr->setPreferredOutputDevice(selection);
                                                 }

                                                 refreshOutputSummary(*playback);
                                               }),
                                             Gtk::PositionType::BOTTOM);
    _outputDeviceButton.set_popover(*selector);
  }
} // namespace ao::gtk
