// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include "TuiPreferences.h"
#include "TuiTextFieldModel.h"
#include <ao/Error.h>
#include <ao/uimodel/input/KeymapModel.h>

#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>

namespace ao::i18n
{
  class MessageCatalog;
}

namespace ao::tui
{
  enum class SettingsPage : std::uint8_t
  {
    General,
    Appearance,
    Interaction,
    Keyboard
  };

  /// Concrete modal Settings surface. App owns persistence and live publication.
  class SettingsEditor final
  {
  public:
    struct Outputs final
    {
      std::function<Result<>(TuiPreferences const&)> applyPreferences;
      std::function<Result<>(uimodel::KeymapModel const&)> applyKeymap;
      /// Effective renderer, including an explicit command-line override.
      std::function<std::string()> coverMode;
    };

    SettingsEditor(i18n::MessageCatalog const& textCatalog,
                   TuiPreferences const& preferences,
                   uimodel::KeymapModel const& keymap,
                   Outputs outputs);

    void open();
    void retire();
    bool isActive() const noexcept { return _active; }
    SettingsPage page() const noexcept { return _page; }
    bool tryHandleEvent(ftxui::Event const& event);
    ftxui::Element renderModal(std::int32_t columns, std::int32_t rows) const;

  private:
    std::size_t rowCount() const;
    void moveRow(std::int32_t delta);
    void changePreference(std::int32_t delta);
    void applyPreferences(TuiPreferences candidate);
    void applyKeymap(uimodel::KeymapModel candidate);
    void retry();
    bool tryHandlePrompt(ftxui::Event const& event);
    void editChordText(ftxui::Event const& event);
    void handleChordEditing(ftxui::Event const& event);
    void submitKeymap(uimodel::KeymapModel candidate);
    void handleKeyboard(ftxui::Event const& event);
    std::string actionLabel(std::size_t index) const;
    std::string preferenceLabel(std::size_t index) const;
    std::string preferenceValue(std::size_t index) const;
    ftxui::Element renderKeyboard(std::int32_t columns) const;
    ftxui::Element renderBody(std::int32_t columns) const;
    ftxui::Element renderFooter() const;

    i18n::MessageCatalog const& _textCatalog;
    TuiPreferences const& _preferences;
    uimodel::KeymapModel const& _keymap;
    Outputs _outputs;
    bool _active = false;
    SettingsPage _page = SettingsPage::General;
    std::size_t _row = 0;
    std::size_t _chord = 0;
    std::optional<TuiPreferences> _optPreferenceCandidate;
    std::optional<uimodel::KeymapModel> _optKeymapCandidate;
    std::string _diagnostic;
    bool _editingChord = false;
    bool _addingChord = false;
    TuiTextFieldModel _chordInput;
    bool _choosingLanguage = false;
    std::size_t _language = 0;
    bool _confirmClose = false;
  };
} // namespace ao::tui
