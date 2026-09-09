// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "SettingsEditor.h"

#include "Command.h"
#include "CoverArt.h"
#include "Keymap.h"
#include "Preferences.h"
#include "Style.h"
#include "TextCell.h"
#include <ao/compat/Enumerate.h>
#include <ao/i18n/MessageCatalog.h>
#include <ao/uimodel/input/KeyChord.h>
#include <ao/uimodel/input/KeymapModel.h>

#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <format>
#include <iterator>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ao::tui
{
  namespace
  {
    using i18n::MessageId;
    constexpr auto kPages = std::to_array<MessageId>({MessageId::TuiSettingsGeneral,
                                                      MessageId::TuiSettingsAppearance,
                                                      MessageId::TuiSettingsInteraction,
                                                      MessageId::TuiSettingsKeyboard});
    constexpr auto kAppearanceLabels =
      std::to_array<MessageId>({MessageId::TuiSettingsDim, MessageId::TuiSettingsMotion, MessageId::TuiSettingsCover});
    constexpr auto kInteractionLabels = std::to_array<MessageId>({MessageId::TuiSettingsMouse,
                                                                  MessageId::TuiSettingsWheel,
                                                                  MessageId::TuiSettingsSeek,
                                                                  MessageId::TuiSettingsVolume,
                                                                  MessageId::TuiSettingsHover});

    std::span<i18n::CatalogLocale const> languageChoices()
    {
      static auto const kChoices = []
      {
        auto result = std::vector<i18n::CatalogLocale>{{.tag = "", .selfName = ""}};
        result.append_range(i18n::availableCatalogLocales());
        return result;
      }();
      return kChoices;
    }

    std::string settingsText(i18n::MessageCatalog const& catalog, MessageId const id)
    {
      switch (id)
      {
        case MessageId::TuiSettingsPageKeys:
          return i18n::requiredFormat(catalog, id, {{"pages", "Tab/Shift+Tab"}, {"close", "Esc"}});
        case MessageId::TuiSettingsPreferenceKeys: return i18n::requiredFormat(catalog, id, {{"change", "←/→/Enter"}});
        case MessageId::TuiSettingsLanguageKeys:
          return i18n::requiredFormat(catalog, id, {{"move", "↑/↓"}, {"apply", "Enter"}, {"close", "Esc"}});
        case MessageId::TuiSettingsChordKeys:
          return i18n::requiredFormat(catalog, id, {{"apply", "Enter"}, {"close", "Esc"}});
        case MessageId::TuiSettingsConfirmKeys:
          return i18n::requiredFormat(catalog, id, {{"apply", "Enter"}, {"retry", "Ctrl+R"}, {"close", "Esc"}});
        case MessageId::TuiSettingsRetryKeys:
          return i18n::requiredFormat(catalog, id, {{"retry", "Ctrl+R"}, {"discard", "Ctrl+G"}, {"close", "Esc"}});
        case MessageId::TuiSettingsKeyboardKeys:
          return i18n::requiredFormat(
            catalog,
            id,
            {{"choose", "←/→"}, {"edit", "Enter"}, {"add", "Insert"}, {"remove", "Delete"}, {"reset", "r"}});
        default: return std::string{i18n::requiredText(catalog, id)};
      }
    }

    ftxui::Element selectedRow(std::string value, bool const selected)
    {
      auto elementPtr = ftxui::text(std::move(value));
      return selected ? std::move(elementPtr) | style::selected() | ftxui::focus : elementPtr;
    }

    std::size_t movedIndex(std::size_t const index, std::int32_t const delta, std::size_t const count)
    {
      if (count == 0)
      {
        return 0;
      }

      return static_cast<std::size_t>(
        std::clamp(static_cast<std::int64_t>(index) + delta, std::int64_t{0}, static_cast<std::int64_t>(count - 1)));
    }
  } // namespace

  SettingsEditor::SettingsEditor(i18n::MessageCatalog const& textCatalog,
                                 Preferences const& preferences,
                                 uimodel::KeymapModel const& keymap,
                                 Outputs outputs)
    : _textCatalog{textCatalog}, _preferences{preferences}, _keymap{keymap}, _outputs{std::move(outputs)}
  {
  }

  void SettingsEditor::open()
  {
    retire();
    _active = true;
    _page = SettingsPage::General;
    _row = 0;
    _chord = 0;
    _diagnostic.clear();
  }

  void SettingsEditor::retire()
  {
    _active = false;
    _editingChord = false;
    _choosingLanguage = false;
    _confirmClose = false;
    _optPreferenceCandidate.reset();
    _optKeymapCandidate.reset();
    _diagnostic.clear();
  }

  bool SettingsEditor::tryHandleEvent(ftxui::Event const& event)
  {
    if (!_active)
    {
      return false;
    }

    if (tryHandlePrompt(event))
    {
      return true;
    }

    if (_choosingLanguage)
    {
      if (event == ftxui::Event::Escape)
      {
        _choosingLanguage = false;
      }
      else if (event == ftxui::Event::ArrowUp)
      {
        _language = movedIndex(_language, -1, languageChoices().size());
      }
      else if (event == ftxui::Event::ArrowDown)
      {
        _language = movedIndex(_language, 1, languageChoices().size());
      }
      else if (event == ftxui::Event::Return)
      {
        auto candidate = _preferences;
        candidate.language = languageChoices()[_language].tag;
        _choosingLanguage = false;
        applyPreferences(std::move(candidate));
      }

      return true;
    }

    if (_editingChord)
    {
      handleKeyboard(event);
      return true;
    }

    if (event == ftxui::Event::Escape)
    {
      retire();

      return true;
    }

    if (event == ftxui::Event::Tab || event == ftxui::Event::TabReverse)
    {
      auto const delta = event == ftxui::Event::Tab ? 1 : -1;
      _page = static_cast<SettingsPage>(
        (static_cast<std::int32_t>(_page) + delta + static_cast<std::int32_t>(kPages.size())) %
        static_cast<std::int32_t>(kPages.size()));
      _row = 0;
      _chord = 0;
      _diagnostic.clear();
    }
    else if (event == ftxui::Event::ArrowUp)
    {
      moveRow(-1);
    }
    else if (event == ftxui::Event::ArrowDown)
    {
      moveRow(1);
    }
    else if (event == ftxui::Event::PageUp)
    {
      moveRow(-8);
    }
    else if (event == ftxui::Event::PageDown)
    {
      moveRow(8);
    }
    else if (event == ftxui::Event::Home)
    {
      _row = 0;
      _chord = 0;
    }
    else if (event == ftxui::Event::End)
    {
      _row = rowCount() == 0 ? 0 : rowCount() - 1;
      _chord = 0;
    }
    else if (_page == SettingsPage::Keyboard)
    {
      handleKeyboard(event);
    }
    else if (event == ftxui::Event::Return || event == ftxui::Event::ArrowRight ||
             event == ftxui::Event::Character(" "))
    {
      changePreference(1);
    }
    else if (event == ftxui::Event::ArrowLeft)
    {
      changePreference(-1);
    }

    return true;
  }

  ftxui::Element SettingsEditor::renderModal(std::int32_t const columns, std::int32_t const rows) const
  {
    using namespace ftxui;
    auto const width = std::min(columns, std::clamp(columns - 4, 76, 100));
    auto const height = std::min(rows, 28);
    auto tabs = Elements{};

    for (std::size_t index = 0; index < kPages.size(); ++index)
    {
      auto itemPtr = text(" " + std::string{i18n::requiredText(_textCatalog, kPages[index])} + " ");
      tabs.push_back(index == static_cast<std::size_t>(_page) ? std::move(itemPtr) | style::selected() : itemPtr);
    }

    auto title = std::string{i18n::requiredText(_textCatalog, MessageId::TuiSettingsTitle)};
    auto context = std::string{i18n::requiredText(_textCatalog, MessageId::TuiSettingsGlobal)};

    title += " · " + context;
    auto modalPtr = vbox({text(ellipsizeToCellWidth(title, width - 2)) | bold,
                          hflow(std::move(tabs)),
                          separator(),
                          renderBody(width - 2),
                          separator(),
                          renderFooter()}) |
                    border | size(WIDTH, EQUAL, width) | size(HEIGHT, EQUAL, height) | clear_under;
    return vbox({filler(), hbox({filler(), std::move(modalPtr), filler()}), filler()});
  }

  std::size_t SettingsEditor::rowCount() const
  {
    switch (_page)
    {
      case SettingsPage::General: return 1;
      case SettingsPage::Appearance: return kAppearanceLabels.size();
      case SettingsPage::Interaction: return kInteractionLabels.size();
      case SettingsPage::Keyboard: return actionDescriptors().size();
    }

    return 0;
  }

  void SettingsEditor::moveRow(std::int32_t const delta)
  {
    _row = movedIndex(_row, delta, rowCount());
    _chord = 0;
    _diagnostic.clear();
  }

  void SettingsEditor::changePreference(std::int32_t const delta)
  {
    auto candidate = _preferences;

    if (_page == SettingsPage::General)
    {
      _choosingLanguage = true;
      auto const choices = languageChoices();
      auto const it = std::ranges::find(choices, candidate.language, &i18n::CatalogLocale::tag);
      _language = it == choices.end() ? 0 : static_cast<std::size_t>(it - choices.begin());
      return;
    }

    if (_page == SettingsPage::Appearance)
    {
      if (_row == 0)
      {
        candidate.dimBackdrop = !candidate.dimBackdrop;
      }
      else if (_row == 1)
      {
        candidate.reducedMotion = !candidate.reducedMotion;
      }
      else
      {
        auto const index = static_cast<std::int32_t>(std::ranges::distance(
          kCoverArtModes.begin(),
          std::ranges::find(kCoverArtModes, candidate.coverArtMode, &CoverArtModeDescriptor::name)));
        auto const count = static_cast<std::int32_t>(kCoverArtModes.size());
        candidate.coverArtMode = kCoverArtModes[static_cast<std::size_t>((index + delta + count) % count)].name;
      }
    }
    else if (_page == SettingsPage::Interaction)
    {
      switch (_row)
      {
        case 0: candidate.mouseEnabled = !candidate.mouseEnabled; break;
        case 1: candidate.wheelStep = std::clamp(candidate.wheelStep + delta, 1, kMaximumWheelStep); break;
        case 2: candidate.seekSeconds = std::clamp(candidate.seekSeconds + delta, 1, kMaximumSeekSeconds); break;
        case 3: candidate.volumePercent = std::clamp(candidate.volumePercent + delta, 1, kMaximumVolumePercent); break;
        case 4: candidate.qualityHover = !candidate.qualityHover; break;
        default: return;
      }
    }

    if (candidate != _preferences)
    {
      applyPreferences(std::move(candidate));
    }
  }

  void SettingsEditor::applyPreferences(Preferences candidate)
  {
    if (auto const res = _outputs.applyPreferences(candidate); !res)
    {
      _diagnostic =
        i18n::requiredFormat(_textCatalog, MessageId::TuiSettingsSaveFailed, {{"detail", res.error().message}});
      _optPreferenceCandidate = std::move(candidate);
      return;
    }

    _optPreferenceCandidate.reset();
    _confirmClose = false;
    _diagnostic.clear();
  }

  void SettingsEditor::applyKeymap(uimodel::KeymapModel candidate)
  {
    auto const chords = candidate.chordsFor(actionDescriptors()[_row].actionId);
    _chord = std::min(_chord, chords.empty() ? std::size_t{0} : chords.size() - 1);

    if (auto const res = _outputs.applyKeymap(candidate); !res)
    {
      _diagnostic =
        i18n::requiredFormat(_textCatalog, MessageId::TuiSettingsSaveFailed, {{"detail", res.error().message}});
      _optKeymapCandidate = std::move(candidate);
      return;
    }

    _optKeymapCandidate.reset();
    _confirmClose = false;
    _diagnostic.clear();
  }

  void SettingsEditor::retry()
  {
    if (_optPreferenceCandidate)
    {
      applyPreferences(*_optPreferenceCandidate);
    }
    else if (_optKeymapCandidate)
    {
      applyKeymap(*_optKeymapCandidate);
    }
  }

  bool SettingsEditor::tryHandlePrompt(ftxui::Event const& event)
  {
    if (_confirmClose)
    {
      if (event == ftxui::Event::Escape)
      {
        _confirmClose = false;
      }
      else if (event == ftxui::Event::CtrlR)
      {
        retry();
      }
      else if (event == ftxui::Event::Return)
      {
        retire();
      }

      return true;
    }

    if (_optPreferenceCandidate || _optKeymapCandidate)
    {
      if (event == ftxui::Event::CtrlR)
      {
        retry();
      }
      else if (event == ftxui::Event::CtrlG)
      {
        _optPreferenceCandidate.reset();
        _optKeymapCandidate.reset();
        _diagnostic.clear();
      }
      else if (event == ftxui::Event::Escape)
      {
        _confirmClose = true;
      }

      return true;
    }

    return false;
  }

  void SettingsEditor::editChordText(ftxui::Event const& event)
  {
    if (event == ftxui::Event::Backspace)
    {
      std::ignore = _chordInput.tryBackspace();
    }
    else if (event == ftxui::Event::Delete)
    {
      std::ignore = _chordInput.tryDeleteForward();
    }
    else if (event == ftxui::Event::ArrowLeft)
    {
      std::ignore = _chordInput.tryMoveLeft();
    }
    else if (event == ftxui::Event::ArrowRight)
    {
      std::ignore = _chordInput.tryMoveRight();
    }
    else if (event == ftxui::Event::Home)
    {
      std::ignore = _chordInput.tryMoveToBegin();
    }
    else if (event == ftxui::Event::End)
    {
      std::ignore = _chordInput.tryMoveToEnd();
    }
    else if (event.is_character())
    {
      std::ignore = _chordInput.tryInsert(event.character());
    }
  }

  void SettingsEditor::handleChordEditing(ftxui::Event const& event)
  {
    if (event == ftxui::Event::Escape)
    {
      _editingChord = false;
      _diagnostic.clear();
      return;
    }

    if (event != ftxui::Event::Return)
    {
      editChordText(event);
      return;
    }

    auto const optChord = uimodel::KeyChord::parse(_chordInput.value());

    if (!optChord)
    {
      _diagnostic = i18n::requiredFormat(_textCatalog, MessageId::TuiSettingsInvalidChord, {{"example", "Ctrl+P"}});
      return;
    }

    auto const& action = actionDescriptors()[_row];
    auto const chords = _keymap.chordsFor(action.actionId);
    auto candidate = _keymap;

    if (!_addingChord && !chords.empty())
    {
      std::ignore = candidate.tryUnbind(action.actionId, chords[_chord]);
    }

    // Terminal aliases for one action represent a single physical binding.
    auto const optEvent = eventForChord(*optChord);

    for (auto const& existing : candidate.chordsFor(action.actionId))
    {
      if (optEvent && eventForChord(existing) == optEvent)
      {
        std::ignore = candidate.tryUnbind(action.actionId, existing);
      }
    }

    std::ignore = candidate.tryBind(action.actionId, *optChord);
    submitKeymap(std::move(candidate));
  }

  void SettingsEditor::submitKeymap(uimodel::KeymapModel candidate)
  {
    auto const& action = actionDescriptors()[_row];

    if (auto const res = validateActionBindings(candidate, action.actionId); !res)
    {
      auto const message =
        res.error().code == Error::Code::Conflict ? MessageId::TuiSettingsConflict : MessageId::TuiSettingsUnsupported;
      auto detail = res.error().message;

      if (res.error().code == Error::Code::Conflict)
      {
        auto const descriptors = actionDescriptors();
        auto const it = std::ranges::find(descriptors, detail, &ActionDescriptor::actionId);

        if (it != descriptors.end())
        {
          detail = actionLabel(static_cast<std::size_t>(it - descriptors.begin()));
        }
      }

      _diagnostic = i18n::requiredFormat(_textCatalog, message, {{"detail", detail}});
      return;
    }

    _editingChord = false;
    applyKeymap(std::move(candidate));
  }

  void SettingsEditor::handleKeyboard(ftxui::Event const& event)
  {
    if (_editingChord)
    {
      handleChordEditing(event);
      return;
    }

    auto const& action = actionDescriptors()[_row];
    auto const chords = _keymap.chordsFor(action.actionId);
    _chord = std::min(_chord, chords.empty() ? std::size_t{0} : chords.size() - 1);

    if (event == ftxui::Event::ArrowLeft || event == ftxui::Event::ArrowRight)
    {
      _chord = movedIndex(_chord, event == ftxui::Event::ArrowLeft ? -1 : 1, chords.size());
    }
    else if (event == ftxui::Event::Return || event == ftxui::Event::Insert)
    {
      _addingChord = event == ftxui::Event::Insert || chords.empty();
      _chordInput.reset(_addingChord ? std::string{} : chords[_chord].toString());
      _editingChord = true;
      _diagnostic.clear();
    }
    else if (event == ftxui::Event::Delete && !chords.empty())
    {
      auto candidate = _keymap;
      std::ignore = candidate.tryUnbind(action.actionId, chords[_chord]);
      // Removal is a recovery operation even if another saved chord is unsupported.
      applyKeymap(std::move(candidate));
    }
    else if (event == ftxui::Event::Character("r"))
    {
      auto candidate = _keymap;
      candidate.resetToDefault(action.actionId);
      submitKeymap(std::move(candidate));
    }
  }

  std::string SettingsEditor::actionLabel(std::size_t const index) const
  {
    auto const& descriptor = actionDescriptors()[index];

    if (auto const optCommand = commandActionForKeyAction(descriptor.action); optCommand)
    {
      for (auto const& alias : commandAliasSpecs())
      {
        if (alias.action == *optCommand)
        {
          return std::string{i18n::requiredText(_textCatalog, alias.detail)};
        }
      }
    }

    switch (descriptor.action)
    {
      case KeyAction::OpenQuickFilter:
        return std::string{i18n::requiredText(_textCatalog, MessageId::TuiShellDetailQuickFilter)};
      case KeyAction::OpenCommandPalette:
        return std::string{i18n::requiredText(_textCatalog, MessageId::TuiSettingsPalette)};
      case KeyAction::PreviousTrack:
        return std::string{i18n::requiredText(_textCatalog, MessageId::TuiSettingsPreviousRow)};
      case KeyAction::NextTrack: return std::string{i18n::requiredText(_textCatalog, MessageId::TuiSettingsNextRow)};
      case KeyAction::PreviousSection:
        return std::string{i18n::requiredText(_textCatalog, MessageId::TuiSettingsPreviousGroup)};
      case KeyAction::NextSection:
        return std::string{i18n::requiredText(_textCatalog, MessageId::TuiSettingsNextGroup)};
      case KeyAction::SeekBackward:
        return std::string{i18n::requiredText(_textCatalog, MessageId::TuiSettingsSeekBack)};
      case KeyAction::SeekForward:
        return std::string{i18n::requiredText(_textCatalog, MessageId::TuiSettingsSeekForward)};
      case KeyAction::VolumeDown:
        return std::string{i18n::requiredText(_textCatalog, MessageId::TuiSettingsVolumeDown)};
      case KeyAction::VolumeUp: return std::string{i18n::requiredText(_textCatalog, MessageId::TuiSettingsVolumeUp)};
      default: return descriptor.actionId;
    }
  }

  std::string SettingsEditor::preferenceLabel(std::size_t const index) const
  {
    auto id = MessageId::TuiSettingsLanguage;

    if (_page == SettingsPage::Appearance)
    {
      id = kAppearanceLabels[index];
    }
    else if (_page == SettingsPage::Interaction)
    {
      id = kInteractionLabels[index];
    }

    return std::string{i18n::requiredText(_textCatalog, id)};
  }

  std::string SettingsEditor::preferenceValue(std::size_t const index) const
  {
    auto const& preferences = _optPreferenceCandidate ? *_optPreferenceCandidate : _preferences;
    auto const boolean = [&](bool value)
    {
      return std::string{
        i18n::requiredText(_textCatalog, value ? MessageId::TuiSettingsOn : MessageId::TuiSettingsOff)};
    };

    if (_page == SettingsPage::General)
    {
      auto const choices = languageChoices();
      auto const it = std::ranges::find(choices, preferences.language, &i18n::CatalogLocale::tag);
      return it == choices.end() || it->tag.empty()
               ? std::string{i18n::requiredText(_textCatalog, MessageId::TuiSettingsSystem)}
               : std::string{it->selfName};
    }

    if (_page == SettingsPage::Appearance)
    {
      if (index == 0)
      {
        return boolean(preferences.dimBackdrop);
      }

      if (index == 1)
      {
        return boolean(preferences.reducedMotion);
      }

      return preferences.coverArtMode;
    }

    switch (index)
    {
      case 0: return boolean(preferences.mouseEnabled);
      case 1:
        return i18n::requiredFormat(_textCatalog, MessageId::TuiSettingsTracks, {{"count", preferences.wheelStep}});
      case 2:
        return i18n::requiredFormat(_textCatalog, MessageId::TuiSettingsSeconds, {{"count", preferences.seekSeconds}});
      case 3: return std::format("{}%", preferences.volumePercent);
      default: return boolean(preferences.qualityHover);
    }
  }

  ftxui::Element SettingsEditor::renderKeyboard(std::int32_t const columns) const
  {
    using namespace ftxui;
    auto rows = Elements{};

    for (std::size_t index = 0; index < rowCount(); ++index)
    {
      auto value = actionLabel(index) + "  ";
      auto const& keymap = _optKeymapCandidate ? *_optKeymapCandidate : _keymap;
      auto const chords = keymap.chordsFor(actionDescriptors()[index].actionId);

      if (chords.empty())
      {
        value += i18n::requiredText(_textCatalog, MessageId::TuiSettingsUnbound);
      }

      for (std::size_t chordIndex = 0; chordIndex < chords.size(); ++chordIndex)
      {
        auto const selected = index == _row && chordIndex == _chord;
        value += (selected ? "[" : " ") + chords[chordIndex].toString() + (selected ? "] " : "  ");
      }

      rows.push_back(selectedRow(ellipsizeToCellWidth(value, columns), index == _row));
    }

    return vbox(std::move(rows)) | vscroll_indicator | yframe | flex;
  }

  ftxui::Element SettingsEditor::renderBody(std::int32_t const columns) const
  {
    using namespace ftxui;
    auto rows = Elements{};

    if (_choosingLanguage)
    {
      for (auto const& [index, choice] : compat::views::enumerate(languageChoices()))
      {
        rows.push_back(
          selectedRow(std::string{choice.tag.empty() ? i18n::requiredText(_textCatalog, MessageId::TuiSettingsSystem)
                                                     : choice.selfName},
                      std::cmp_equal(index, _language)));
      }
    }
    else if (_page == SettingsPage::Keyboard)
    {
      return renderKeyboard(columns);
    }
    else
    {
      for (std::size_t index = 0; index < rowCount(); ++index)
      {
        rows.push_back(
          selectedRow(ellipsizeToCellWidth(preferenceLabel(index) + "  < " + preferenceValue(index) + " >", columns),
                      index == _row));
      }
    }

    return vbox(std::move(rows)) | vscroll_indicator | yframe | flex;
  }

  ftxui::Element SettingsEditor::renderFooter() const
  {
    using namespace ftxui;
    auto rows = Elements{};
    auto line = [&](MessageId id) { rows.push_back(paragraph(settingsText(_textCatalog, id))); };

    if (_confirmClose)
    {
      line(MessageId::TuiSettingsClosePrompt);
      line(MessageId::TuiSettingsConfirmKeys);
      return vbox(std::move(rows));
    }

    if (!_diagnostic.empty())
    {
      rows.push_back(paragraph(_diagnostic) | style::danger());
    }

    if (_optPreferenceCandidate || _optKeymapCandidate)
    {
      line(MessageId::TuiSettingsRetryKeys);
      return vbox(std::move(rows));
    }

    if (_editingChord)
    {
      rows.push_back(text(_chordInput.value().substr(0, _chordInput.cursor()) + "▏" +
                          _chordInput.value().substr(_chordInput.cursor())) |
                     style::selected());
      line(MessageId::TuiSettingsChordKeys);
      return vbox(std::move(rows));
    }

    if (_choosingLanguage)
    {
      line(MessageId::TuiSettingsLanguageKeys);
      return vbox(std::move(rows));
    }

    if (_page == SettingsPage::Keyboard)
    {
      auto const id = actionDescriptors()[_row].actionId;
      rows.push_back(text(id) | dim);

      if (auto const res = validateActionBindings(_keymap, id); !res)
      {
        rows.push_back(paragraph(i18n::requiredFormat(
                         _textCatalog, MessageId::TuiSettingsStoredIssue, {{"detail", res.error().message}})) |
                       style::warning());
      }

      line(MessageId::TuiSettingsKeyboardKeys);
      line(MessageId::TuiSettingsImmediate);
    }
    else
    {
      if (_page == SettingsPage::General)
      {
        line(MessageId::TuiSettingsLanguageHint);
      }

      if (_page == SettingsPage::Appearance && _row == 2)
      {
        rows.push_back(paragraph(
          i18n::requiredFormat(_textCatalog, MessageId::TuiSettingsEffectiveCover, {{"mode", _outputs.coverMode()}})));
      }

      line(MessageId::TuiSettingsPreferenceKeys);
      line(MessageId::TuiSettingsImmediate);
    }

    line(MessageId::TuiSettingsPageKeys);
    return vbox(std::move(rows));
  }
} // namespace ao::tui
