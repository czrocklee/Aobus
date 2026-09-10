// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include "ListSearch.h"
#include "MouseBindings.h"
#include "Preferences.h"
#include "TextFieldModel.h"
#include <ao/Error.h>
#include <ao/uimodel/input/KeymapModel.h>

#include <ftxui/component/event.hpp>
#include <ftxui/component/mouse.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/box.hpp>

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <optional>
#include <string>
#include <vector>

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
      std::function<Result<>(Preferences const&)> applyPreferences;
      std::function<Result<>(uimodel::KeymapModel const&)> applyKeymap;
      /// Effective renderer, including an explicit command-line override.
      std::function<std::string()> coverMode;
    };

    SettingsEditor(i18n::MessageCatalog const& textCatalog,
                   Preferences const& preferences,
                   uimodel::KeymapModel const& keymap,
                   Outputs outputs);

    void open();
    void retire();
    bool isActive() const noexcept { return _active; }
    SettingsPage page() const noexcept { return _page; }
    bool tryHandleEvent(ftxui::Event const& event);
    ftxui::Element renderModal(std::int32_t columns, std::int32_t rows) const;

  private:
    void handleLanguageChoice(ftxui::Event const& event);
    void handleMouse(ftxui::Mouse const& mouse);
    std::size_t rowCount() const;
    std::vector<std::string> keyboardLabels() const;
    void moveRow(std::int32_t delta);
    void changePreference(std::int32_t delta);
    void applyPreferences(Preferences candidate);
    void applyKeymap(uimodel::KeymapModel candidate);
    void retry();
    bool tryHandlePrompt(ftxui::Event const& event);
    void handleChordEditing(ftxui::Event const& event);
    void submitKeymap(uimodel::KeymapModel candidate);
    void handleKeyboard(ftxui::Event const& event);
    std::string actionLabel(std::size_t index) const;
    std::string preferenceLabel(std::size_t index) const;
    std::string preferenceValue(std::size_t index) const;
    ftxui::Element renderKeyboard(std::int32_t columns) const;
    ftxui::Element renderBody(std::int32_t columns) const;
    ftxui::Element renderFooterActions(std::int32_t columns) const;
    ftxui::Element renderFooter(std::int32_t columns) const;

    i18n::MessageCatalog const& _textCatalog;
    Preferences const& _preferences;
    uimodel::KeymapModel const& _keymap;
    Outputs _outputs;
    bool _active = false;
    SettingsPage _page = SettingsPage::General;
    ListSearch _search{};
    std::size_t _row = 0;
    std::size_t _chord = 0;
    std::optional<Preferences> _optPreferenceCandidate;
    std::optional<uimodel::KeymapModel> _optKeymapCandidate;
    std::string _diagnostic;
    bool _editingChord = false;
    bool _addingChord = false;
    TextFieldModel _chordInput;
    bool _choosingLanguage = false;
    std::size_t _language = 0;
    bool _confirmClose = false;
    struct ChordHit final
    {
      std::size_t row = 0;
      std::size_t chord = 0;
      ftxui::Box box = kEmptyMouseBox;
    };
    mutable bool _mouseReady = false;
    mutable MouseBindings _mouseBindings;
    mutable std::vector<ftxui::Box> _tabBoxes;
    mutable std::vector<ftxui::Box> _rowBoxes;
    mutable std::vector<ftxui::Box> _decreaseBoxes;
    mutable std::vector<ftxui::Box> _valueBoxes;
    mutable std::deque<ChordHit> _chordBoxes;
    mutable ftxui::Box _chordInputBox = kEmptyMouseBox;
    mutable ftxui::Box _chordTextBox = kEmptyMouseBox;
    mutable ftxui::Box _keyboardViewport = kEmptyMouseBox;
    mutable ftxui::Box _bodyBox = kEmptyMouseBox;
    mutable SettingsPage _renderedPage = SettingsPage::General;
    mutable bool _renderedLanguage = false;
    mutable bool _renderedChord = false;
  };
} // namespace ao::tui
