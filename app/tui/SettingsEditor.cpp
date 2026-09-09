// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "SettingsEditor.h"

#include "Command.h"
#include "CoverArt.h"
#include "Keymap.h"
#include "MouseBindings.h"
#include "Preferences.h"
#include "SelectionNavigation.h"
#include "Style.h"
#include "TextCell.h"
#include "TextField.h"
#include <ao/compat/Enumerate.h>
#include <ao/i18n/MessageCatalog.h>
#include <ao/uimodel/input/KeyChord.h>
#include <ao/uimodel/input/KeymapModel.h>

#include <ftxui/component/event.hpp>
#include <ftxui/component/mouse.hpp>
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
    constexpr std::int32_t kTitleControlColumns = 6;
    constexpr std::int32_t kPreferenceControlColumns = 6;

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
            catalog, id, {{"choose", "←/→"}, {"edit", "Enter"}, {"add", "a"}, {"remove", "Delete"}, {"reset", "r"}});
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
    _search.clear();
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

    if (event.is_mouse())
    {
      if (!std::exchange(_mouseReady, false))
      {
        return true;
      }

      if (_page == SettingsPage::Keyboard && !_editingChord && _search.tryHandleEvent(event))
      {
        return true;
      }

      auto mouseEvent = event;
      handleMouse(mouseEvent.mouse());
      return true;
    }

    _mouseReady = false;

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
      else if (auto optDelta = listNavigationDelta(event, navigationPageRows(_bodyBox), true); optDelta)
      {
        _language = movedIndex(_language, *optDelta, languageChoices().size());
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

    if (_page == SettingsPage::Keyboard && _search.tryHandleEvent(event))
    {
      moveRow(0);
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
      _search.clear();
      _row = 0;
      _chord = 0;
      _diagnostic.clear();
    }
    else if (auto optDelta =
               listNavigationDelta(event,
                                   navigationPageRows(_page == SettingsPage::Keyboard ? _keyboardViewport : _bodyBox),
                                   !_search.isActive());
             optDelta)
    {
      moveRow(*optDelta);
    }
    else if (_page == SettingsPage::Keyboard)
    {
      if (_search.matches(actionLabel(_row) + " " + std::string{actionDescriptors()[_row].actionId}))
      {
        handleKeyboard(event);
      }
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
    _mouseBindings.clear();
    _tabBoxes.assign(kPages.size(), kEmptyMouseBox);
    _rowBoxes.clear();
    _decreaseBoxes.clear();
    _valueBoxes.clear();
    _chordBoxes.clear();
    _renderedPage = _page;
    _renderedLanguage = _choosingLanguage;
    _renderedChord = _editingChord;
    auto tabs = Elements{};
    _mouseReady = true;

    for (std::size_t index = 0; index < kPages.size(); ++index)
    {
      auto itemPtr = text(" " + std::string{i18n::requiredText(_textCatalog, kPages[index])} + " ");
      tabs.push_back((index == static_cast<std::size_t>(_page) ? std::move(itemPtr) | style::selected() : itemPtr) |
                     ftxui::reflect(_tabBoxes[index]));
    }

    auto title = std::string{i18n::requiredText(_textCatalog, MessageId::TuiSettingsTitle)};
    auto context = std::string{i18n::requiredText(_textCatalog, MessageId::TuiSettingsGlobal)};

    title += " · " + context;
    auto modalPtr = vbox({hbox({text(ellipsizeToCellWidth(title, width - kTitleControlColumns)) | bold | flex,
                                _mouseBindings.bind(text(" × "), Event::Escape)}),
                          hflow(std::move(tabs)),
                          separator(),
                          renderBody(width - 2) | ftxui::reflect(_bodyBox),
                          separator(),
                          renderFooter()}) |
                    border | size(WIDTH, EQUAL, width) | size(HEIGHT, EQUAL, height) | clear_under;
    return vbox({filler(), hbox({filler(), std::move(modalPtr), filler()}), filler()});
  }

  void SettingsEditor::handleMouse(ftxui::Mouse const& mouse)
  {
    if (_renderedPage != _page || _renderedLanguage != _choosingLanguage || _renderedChord != _editingChord)
    {
      return;
    }

    if (auto const optEvent = _mouseBindings.eventAt(mouse); optEvent)
    {
      _mouseBindings.clear();
      tryHandleEvent(*optEvent);
      return;
    }

    if (_editingChord && isLeftPress(mouse) && containsMouse(_chordInputBox, mouse))
    {
      _chordInput.tryMoveToCell(mouse.x - _chordTextBox.x_min);
      return;
    }

    if (_confirmClose || _optPreferenceCandidate || _optKeymapCandidate || _editingChord)
    {
      return;
    }

    if (auto const wheel = mouseWheelDirection(mouse); wheel != 0 && containsMouse(_bodyBox, mouse))
    {
      if (_choosingLanguage)
      {
        _language = movedIndex(_language, wheel, languageChoices().size());
      }
      else
      {
        moveRow(wheel);
      }

      return;
    }

    if (!isLeftPress(mouse))
    {
      return;
    }

    if (!_choosingLanguage)
    {
      if (auto const optTab = mouseRowAt(_tabBoxes, mouse); optTab)
      {
        _page = static_cast<SettingsPage>(*optTab);
        _search.clear();
        _row = 0;
        _chord = 0;
        _diagnostic.clear();
        _mouseBindings.clear();
        return;
      }
    }

    auto const optRow = mouseRowAt(_rowBoxes, mouse);

    if (!optRow)
    {
      return;
    }

    if (_choosingLanguage)
    {
      _language = *optRow;
      tryHandleEvent(ftxui::Event::Return);
      return;
    }

    if (*optRow >= rowCount())
    {
      return;
    }

    _row = *optRow;
    _chord = 0;
    _diagnostic.clear();

    if (_page == SettingsPage::Keyboard)
    {
      auto const hit = std::ranges::find_if(
        _chordBoxes, [&](ChordHit const& chord) { return chord.row == _row && containsMouse(chord.box, mouse); });

      if (hit != _chordBoxes.end())
      {
        _chord = hit->chord;
        handleKeyboard(ftxui::Event::Return);
      }
    }
    else if (mouseRowAt(_decreaseBoxes, mouse))
    {
      changePreference(-1);
    }
    else if (mouseRowAt(_valueBoxes, mouse) || _page == SettingsPage::General)
    {
      changePreference(1);
    }
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

  std::vector<std::string> SettingsEditor::keyboardLabels() const
  {
    auto labels = std::vector<std::string>{};

    for (std::size_t index = 0; index < actionDescriptors().size(); ++index)
    {
      labels.push_back(actionLabel(index) + " " + std::string{actionDescriptors()[index].actionId});
    }

    return labels;
  }

  void SettingsEditor::moveRow(std::int32_t const delta)
  {
    if (_page == SettingsPage::Keyboard)
    {
      if (auto optTarget = _search.selection(keyboardLabels(), static_cast<std::int32_t>(_row), delta); optTarget)
      {
        _row = static_cast<std::size_t>(*optTarget);
      }
    }
    else
    {
      _row = movedIndex(_row, delta, rowCount());
    }

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
      std::ignore = _chordInput.tryApplyEvent(event);
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
    else if (event == ftxui::Event::Return || event == ftxui::Event::Insert || event == ftxui::Event::Character("a"))
    {
      _addingChord = event != ftxui::Event::Return || chords.empty();
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
      case KeyAction::SwitchWorkspaceFocus:
        return std::string{i18n::requiredText(_textCatalog, MessageId::TuiNavigationFocus)};
      case KeyAction::OpenQuickFilter:
        return std::string{i18n::requiredText(_textCatalog, MessageId::TuiShellDetailQuickFilter)};
      case KeyAction::OpenCommandPalette:
        return std::string{i18n::requiredText(_textCatalog, MessageId::TuiSettingsPalette)};
      case KeyAction::PreviousRow:
        return std::string{i18n::requiredText(_textCatalog, MessageId::TuiSettingsPreviousRow)};
      case KeyAction::NextRow: return std::string{i18n::requiredText(_textCatalog, MessageId::TuiSettingsNextRow)};
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

    _rowBoxes.assign(rowCount(), kEmptyMouseBox);
    auto const labels = keyboardLabels();

    for (std::size_t index = 0; index < rowCount(); ++index)
    {
      if (!_search.matches(labels[index]))
      {
        continue;
      }

      auto cells = Elements{};
      cells.reserve(8);
      cells.push_back(text(ellipsizeToCellWidth(actionLabel(index), columns / 2)) | size(WIDTH, EQUAL, columns / 2));
      auto const& keymap = _optKeymapCandidate ? *_optKeymapCandidate : _keymap;
      auto const chords = keymap.chordsFor(actionDescriptors()[index].actionId);

      if (chords.empty())
      {
        _chordBoxes.push_back(ChordHit{.row = index});
        cells.push_back(text(std::string{i18n::requiredText(_textCatalog, MessageId::TuiSettingsUnbound)}) |
                        ftxui::reflect(_chordBoxes.back().box));
      }

      for (std::size_t chordIndex = 0; chordIndex < chords.size(); ++chordIndex)
      {
        auto const selected = index == _row && chordIndex == _chord;
        _chordBoxes.push_back(ChordHit{.row = index, .chord = chordIndex});
        cells.push_back(text((selected ? "[" : " ") + keyChordLabel(chords[chordIndex]) + (selected ? "] " : "  ")) |
                        ftxui::reflect(_chordBoxes.back().box));
      }

      auto rowPtr = hbox(std::move(cells));
      rows.push_back((index == _row ? std::move(rowPtr) | style::selected() | focus : rowPtr) |
                     ftxui::reflect(_rowBoxes[index]));
    }

    if (rows.empty())
    {
      rows.push_back(text(std::string{i18n::requiredText(_textCatalog, MessageId::TuiListSearchEmpty)}) | dim);
    }

    return vbox({paragraph(std::string{i18n::requiredText(_textCatalog, MessageId::TuiSettingsKeyboardScope)}) | dim,
                 _search.render(_textCatalog, !_editingChord),
                 vbox(std::move(rows)) | vscroll_indicator | yframe | flex | reflect(_keyboardViewport)}) |
           flex;
  }

  ftxui::Element SettingsEditor::renderBody(std::int32_t const columns) const
  {
    using namespace ftxui;
    auto rows = Elements{};

    if (_choosingLanguage)
    {
      _rowBoxes.assign(languageChoices().size(), kEmptyMouseBox);

      for (auto const& [index, choice] : compat::views::enumerate(languageChoices()))
      {
        rows.push_back(
          selectedRow(std::string{choice.tag.empty() ? i18n::requiredText(_textCatalog, MessageId::TuiSettingsSystem)
                                                     : choice.selfName},
                      std::cmp_equal(index, _language)) |
          ftxui::reflect(_rowBoxes[static_cast<std::size_t>(index)]));
      }
    }
    else if (_page == SettingsPage::Keyboard)
    {
      return renderKeyboard(columns);
    }
    else
    {
      _rowBoxes.assign(rowCount(), kEmptyMouseBox);
      _decreaseBoxes.assign(rowCount(), kEmptyMouseBox);
      _valueBoxes.assign(rowCount(), kEmptyMouseBox);

      for (std::size_t index = 0; index < rowCount(); ++index)
      {
        auto rowPtr =
          hbox({text(ellipsizeToCellWidth(preferenceLabel(index), columns / 2)) | flex,
                text(" < ") | ftxui::reflect(_decreaseBoxes[index]),
                text(ellipsizeToCellWidth(preferenceValue(index), (columns / 2) - kPreferenceControlColumns) + " > ") |
                  ftxui::reflect(_valueBoxes[index])});
        rows.push_back((index == _row ? std::move(rowPtr) | style::selected() | focus : rowPtr) |
                       ftxui::reflect(_rowBoxes[index]));
      }
    }

    return vbox(std::move(rows)) | vscroll_indicator | yframe | flex;
  }

  ftxui::Element SettingsEditor::renderFooter() const
  {
    using namespace ftxui;
    auto rows = Elements{};
    auto line = [&](MessageId id) { rows.push_back(paragraph(settingsText(_textCatalog, id))); };
    auto buttons = Elements{};
    auto button = [&](std::string const& label, Event event)
    { buttons.push_back(_mouseBindings.bind(text(" [" + label + "] ") | bold, std::move(event))); };

    if (_confirmClose || _choosingLanguage || _editingChord || _search.isActive())
    {
      button("Enter", Event::Return);
    }
    else if (_optPreferenceCandidate || _optKeymapCandidate)
    {
      button("Ctrl-R", Event::CtrlR);
      button("Ctrl-G", Event::CtrlG);
    }
    else if (_page == SettingsPage::Keyboard)
    {
      button("a", Event::Character("a"));
      button("Enter", Event::Return);
      button("Delete", Event::Delete);
      button("r", Event::Character("r"));
    }
    else
    {
      button("←", Event::ArrowLeft);
      button("→", Event::ArrowRight);
    }

    button("Esc", Event::Escape);
    rows.push_back(hflow(std::move(buttons)));

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
      rows.push_back(textFieldValue(_chordInput, &_chordTextBox) | ftxui::reflect(_chordInputBox));
      line(MessageId::TuiSettingsChordKeys);
      return vbox(std::move(rows));
    }

    if (_choosingLanguage)
    {
      line(MessageId::TuiSettingsLanguageKeys);
      return vbox(std::move(rows));
    }

    if (_search.isActive())
    {
      line(MessageId::TuiListSearchHint);
      line(MessageId::TuiSettingsPageKeys);
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
