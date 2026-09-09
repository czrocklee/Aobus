// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "Keymap.h"

#include <ao/Contract.h>
#include <ao/Error.h>
#include <ao/rt/Log.h>
#include <ao/uimodel/input/KeyChord.h>
#include <ao/uimodel/input/KeymapModel.h>
#include <ao/uimodel/playback/command/PlaybackCommand.h>
#include <ao/utility/String.h>

#include <ftxui/component/event.hpp>

#include <algorithm>
#include <array>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ao::tui
{
  namespace
  {
    using uimodel::KeyModifier;

    constexpr auto kNoDefaults = std::array<std::string_view, 0>{};
    constexpr auto kOpenSettingsDefaults = std::to_array<std::string_view>({","});
    constexpr auto kQuitDefaults = std::to_array<std::string_view>({"Shift+Q"});
    constexpr auto kSwitchWorkspaceFocusDefaults = std::to_array<std::string_view>({"Tab", "Shift+Tab"});
    constexpr auto kToggleListsDefaults = std::to_array<std::string_view>({"L"});
    constexpr auto kToggleDetailsDefaults = std::to_array<std::string_view>({"D"});
    constexpr auto kToggleAudioPipelineDefaults = std::to_array<std::string_view>({"A"});
    constexpr auto kToggleOutputDevicesDefaults = std::to_array<std::string_view>({"O"});
    constexpr auto kTogglePresentationsDefaults = std::to_array<std::string_view>({"P"});
    constexpr auto kToggleNotificationsDefaults = std::to_array<std::string_view>({"N"});
    constexpr auto kShowHelpDefaults = std::to_array<std::string_view>({"?", "F1"});
    constexpr auto kOpenCommandPaletteDefaults = std::to_array<std::string_view>({":"});
    constexpr auto kOpenQuickFilterDefaults = std::to_array<std::string_view>({"/"});
    constexpr auto kRevealDefaults = std::to_array<std::string_view>({"C"});
    constexpr auto kClearFilterDefaults = std::to_array<std::string_view>({"Shift+C"});
    constexpr auto kReloadDefaults = std::to_array<std::string_view>({"Shift+R"});
    constexpr auto kSelectToggleDefaults = std::to_array<std::string_view>({"M"});
    constexpr auto kSelectVisualDefaults = std::to_array<std::string_view>({"V"});
    constexpr auto kSelectAllDefaults = std::to_array<std::string_view>({"Shift+A"});
    constexpr auto kSelectClearDefaults = std::to_array<std::string_view>({"U"});
    constexpr auto kEditPropertiesDefaults = std::to_array<std::string_view>({"E"});
    constexpr auto kPlaySelectionDefaults = std::to_array<std::string_view>({"Enter"});
    constexpr auto kPreviousTrackDefaults = std::to_array<std::string_view>({"K"});
    constexpr auto kNextTrackDefaults = std::to_array<std::string_view>({"J"});
    constexpr auto kPreviousSectionDefaults = std::to_array<std::string_view>({"{"});
    constexpr auto kNextSectionDefaults = std::to_array<std::string_view>({"}"});
    constexpr auto kSeekBackwardDefaults = std::to_array<std::string_view>({"Left", "["});
    constexpr auto kSeekForwardDefaults = std::to_array<std::string_view>({"Right", "]"});
    constexpr auto kVolumeDownDefaults = std::to_array<std::string_view>({"-"});
    constexpr auto kVolumeUpDefaults = std::to_array<std::string_view>({"+", "="});
    constexpr auto kPlayPauseDefaults = std::to_array<std::string_view>({"Space"});
    constexpr auto kStopDefaults = std::to_array<std::string_view>({"S"});

    constexpr auto kPlaybackPreviousDefaults = std::to_array<std::string_view>({"<", "Ctrl+Left"});
    constexpr auto kPlaybackNextDefaults = std::to_array<std::string_view>({">", "Ctrl+Right"});
    constexpr auto kShuffleDefaults = std::to_array<std::string_view>({"Shift+S"});
    constexpr auto kRepeatDefaults = std::to_array<std::string_view>({"R"});

    std::vector<ActionDescriptor> makeDescriptors()
    {
      using enum uimodel::PlaybackCommand;

      return {
        {.actionId = "tui.shell.openSettings",
         .action = KeyAction::OpenSettings,
         .defaultChords = kOpenSettingsDefaults},
        {.actionId = "tui.shell.quit", .action = KeyAction::Quit, .defaultChords = kQuitDefaults},
        {.actionId = "tui.shell.toggleListChooser",
         .action = KeyAction::ToggleLists,
         .defaultChords = kToggleListsDefaults},
        {.actionId = "tui.shell.toggleTrackDetail",
         .action = KeyAction::ToggleDetails,
         .defaultChords = kToggleDetailsDefaults},
        {.actionId = "tui.shell.toggleAudioQuality",
         .action = KeyAction::ToggleAudioPipeline,
         .defaultChords = kToggleAudioPipelineDefaults},
        {.actionId = "tui.shell.toggleOutputDevices",
         .action = KeyAction::ToggleOutputDevices,
         .defaultChords = kToggleOutputDevicesDefaults},
        {.actionId = "tui.shell.togglePresentationChooser",
         .action = KeyAction::TogglePresentations,
         .defaultChords = kTogglePresentationsDefaults},
        {.actionId = "tui.shell.toggleNotifications",
         .action = KeyAction::ToggleNotifications,
         .defaultChords = kToggleNotificationsDefaults},
        {.actionId = "tui.shell.showHelp", .action = KeyAction::ShowHelp, .defaultChords = kShowHelpDefaults},
        {.actionId = "tui.shell.openCommandPalette",
         .action = KeyAction::OpenCommandPalette,
         .defaultChords = kOpenCommandPaletteDefaults},
        {.actionId = "tui.library.openQuickFilter",
         .action = KeyAction::OpenQuickFilter,
         .defaultChords = kOpenQuickFilterDefaults},
        {.actionId = std::string{uimodel::kRevealCurrentTrackActionId},
         .action = KeyAction::RevealCurrentTrack,
         .defaultChords = kRevealDefaults},
        {.actionId = "tui.library.clearFilter",
         .action = KeyAction::ClearFilter,
         .defaultChords = kClearFilterDefaults},
        {.actionId = "tui.library.reloadActiveList", .action = KeyAction::Reload, .defaultChords = kReloadDefaults},
        {.actionId = "tui.library.scan", .action = KeyAction::Scan, .defaultChords = kNoDefaults},
        {.actionId = "tui.library.scanCancel", .action = KeyAction::ScanCancel, .defaultChords = kNoDefaults},
        {.actionId = "tui.library.selectToggle",
         .action = KeyAction::SelectToggle,
         .defaultChords = kSelectToggleDefaults},
        {.actionId = "tui.library.selectVisual",
         .action = KeyAction::SelectVisual,
         .defaultChords = kSelectVisualDefaults},
        {.actionId = "tui.library.selectAll", .action = KeyAction::SelectAll, .defaultChords = kSelectAllDefaults},
        {.actionId = "tui.library.selectClear",
         .action = KeyAction::SelectClear,
         .defaultChords = kSelectClearDefaults},
        {.actionId = "tui.library.editProperties",
         .action = KeyAction::EditProperties,
         .defaultChords = kEditPropertiesDefaults},
        {.actionId = "tui.library.playSelection",
         .action = KeyAction::PlaySelection,
         .defaultChords = kPlaySelectionDefaults},
        {.actionId = "tui.library.previousRow",
         .action = KeyAction::PreviousRow,
         .defaultChords = kPreviousTrackDefaults},
        {.actionId = "tui.library.nextRow", .action = KeyAction::NextRow, .defaultChords = kNextTrackDefaults},
        {.actionId = "tui.library.previousSection",
         .action = KeyAction::PreviousSection,
         .defaultChords = kPreviousSectionDefaults},
        {.actionId = "tui.library.nextSection",
         .action = KeyAction::NextSection,
         .defaultChords = kNextSectionDefaults},
        {.actionId = "tui.playback.seekBackward",
         .action = KeyAction::SeekBackward,
         .defaultChords = kSeekBackwardDefaults},
        {.actionId = "tui.playback.seekForward",
         .action = KeyAction::SeekForward,
         .defaultChords = kSeekForwardDefaults},
        {.actionId = "tui.playback.volumeDown", .action = KeyAction::VolumeDown, .defaultChords = kVolumeDownDefaults},
        {.actionId = "tui.playback.volumeUp", .action = KeyAction::VolumeUp, .defaultChords = kVolumeUpDefaults},
        {.actionId = uimodel::playbackCommandActionId(PlayPause),
         .action = KeyAction::PlaybackPlayPause,
         .defaultChords = kPlayPauseDefaults},
        {.actionId = uimodel::playbackCommandActionId(Stop),
         .action = KeyAction::PlaybackStop,
         .defaultChords = kStopDefaults},
        {.actionId = uimodel::playbackCommandActionId(Previous),
         .action = KeyAction::PlaybackPrevious,
         .defaultChords = kPlaybackPreviousDefaults},
        {.actionId = uimodel::playbackCommandActionId(Next),
         .action = KeyAction::PlaybackNext,
         .defaultChords = kPlaybackNextDefaults},
        {.actionId = uimodel::playbackCommandActionId(ToggleShuffle),
         .action = KeyAction::PlaybackShuffle,
         .defaultChords = kShuffleDefaults},
        {.actionId = uimodel::playbackCommandActionId(CycleRepeat),
         .action = KeyAction::PlaybackRepeat,
         .defaultChords = kRepeatDefaults},
        {.actionId = "tui.workspace.switchFocus",
         .action = KeyAction::SwitchWorkspaceFocus,
         .defaultChords = kSwitchWorkspaceFocusDefaults},
      };
    }

    bool hasOnly(uimodel::KeyChord const& chord, KeyModifier const modifier)
    {
      return chord.modifiers.mask == uimodel::KeyModifiers{modifier}.mask;
    }

    std::optional<ftxui::Event> namedEvent(std::string_view const key)
    {
      using NamedEvent = std::pair<std::string_view, ftxui::Event>;

      static auto const kEvents = std::to_array<NamedEvent>({
        {"Enter", ftxui::Event::Return},
        {"Escape", ftxui::Event::Escape},
        {"Space", ftxui::Event::Character(" ")},
        {"Tab", ftxui::Event::Tab},
        {"Backspace", ftxui::Event::Backspace},
        {"Insert", ftxui::Event::Insert},
        {"Delete", ftxui::Event::Delete},
        {"Left", ftxui::Event::ArrowLeft},
        {"Right", ftxui::Event::ArrowRight},
        {"Up", ftxui::Event::ArrowUp},
        {"Down", ftxui::Event::ArrowDown},
        {"Home", ftxui::Event::Home},
        {"End", ftxui::Event::End},
        {"PageUp", ftxui::Event::PageUp},
        {"PageDown", ftxui::Event::PageDown},
        {"F1", ftxui::Event::F1},
        {"F2", ftxui::Event::F2},
        {"F3", ftxui::Event::F3},
        {"F4", ftxui::Event::F4},
        {"F5", ftxui::Event::F5},
        {"F6", ftxui::Event::F6},
        {"F7", ftxui::Event::F7},
        {"F8", ftxui::Event::F8},
        {"F9", ftxui::Event::F9},
        {"F10", ftxui::Event::F10},
        {"F11", ftxui::Event::F11},
        {"F12", ftxui::Event::F12},
      });

      for (auto const& [name, event] : kEvents)
      {
        if (name == key)
        {
          return event;
        }
      }

      return std::nullopt;
    }

    std::optional<ftxui::Event> controlLetterEvent(char const letter)
    {
      // Terminal protocol aliases are deliberately projected onto the fixed
      // protocol event the shell already handles in scoped input surfaces.
      switch (letter)
      {
        case 'H': return ftxui::Event::Backspace;
        case 'I': return ftxui::Event::Tab;
        case 'J':
        case 'M': return ftxui::Event::Return;
        default: break;
      }

      auto const control = static_cast<char>(letter - 'A' + 1);
      return ftxui::Event::Special(std::string{control});
    }

    bool isReservedRootEvent(ftxui::Event const& event)
    {
      return event == ftxui::Event::CtrlC || event == ftxui::Event::Escape || event == ftxui::Event::ArrowUp ||
             event == ftxui::Event::ArrowDown || event == ftxui::Event::Home || event == ftxui::Event::End ||
             event == ftxui::Event::PageUp || event == ftxui::Event::PageDown;
    }
  } // namespace

  std::span<ActionDescriptor const> actionDescriptors()
  {
    static auto const kDescriptors = makeDescriptors();
    return kDescriptors;
  }

  uimodel::KeymapBindings defaultKeymap()
  {
    auto bindings = uimodel::KeymapBindings{};

    for (auto const& descriptor : actionDescriptors())
    {
      auto& chords = bindings[descriptor.actionId];
      chords.reserve(descriptor.defaultChords.size());

      for (auto const text : descriptor.defaultChords)
      {
        auto optChord = uimodel::KeyChord::parse(text);
        AO_INVARIANT(optChord, "Invalid built-in TUI key chord: {}", text);

        if (!std::ranges::contains(chords, *optChord))
        {
          chords.push_back(std::move(*optChord));
        }
      }
    }

    return bindings;
  }

  std::optional<ftxui::Event> eventForChord(uimodel::KeyChord const& chord)
  {
    if (!chord.isValid() || chord.isMediaKey() || chord.modifiers.has(KeyModifier::Super))
    {
      return std::nullopt;
    }

    if (chord.modifiers.isEmpty())
    {
      if (auto optEvent = namedEvent(chord.key); optEvent)
      {
        return optEvent;
      }

      if (chord.key.size() == 1)
      {
        auto value = chord.key.front();

        if (utility::isAsciiAlpha(value))
        {
          value = utility::toAsciiLower(value);
        }

        return ftxui::Event::Character(std::string{value});
      }

      return std::nullopt;
    }

    if (hasOnly(chord, KeyModifier::Shift))
    {
      if (chord.key == "Tab")
      {
        return ftxui::Event::TabReverse;
      }

      if (chord.key.size() == 1 && utility::isAsciiAlpha(chord.key.front()))
      {
        return ftxui::Event::Character(std::string{utility::toAsciiUpper(chord.key.front())});
      }

      return std::nullopt;
    }

    if (hasOnly(chord, KeyModifier::Ctrl))
    {
      if (chord.key == "[")
      {
        return ftxui::Event::Escape;
      }

      if (chord.key == "Left")
      {
        return ftxui::Event::ArrowLeftCtrl;
      }

      if (chord.key == "Right")
      {
        return ftxui::Event::ArrowRightCtrl;
      }

      if (chord.key == "Up")
      {
        return ftxui::Event::ArrowUpCtrl;
      }

      if (chord.key == "Down")
      {
        return ftxui::Event::ArrowDownCtrl;
      }

      if (chord.key.size() == 1 && utility::isAsciiAlpha(chord.key.front()))
      {
        auto const letter = utility::toAsciiUpper(chord.key.front());

        return controlLetterEvent(letter);
      }

      return std::nullopt;
    }

    return std::nullopt;
  }

  std::string keyChordLabel(uimodel::KeyChord const& chord)
  {
    if (chord.modifiers.isEmpty() && chord.key.size() == 1 && utility::isAsciiAlpha(chord.key.front()))
    {
      return std::string{utility::toAsciiLower(chord.key.front())};
    }

    if (hasOnly(chord, KeyModifier::Shift) && chord.key.size() == 1 && utility::isAsciiAlpha(chord.key.front()))
    {
      return std::string{utility::toAsciiUpper(chord.key.front())};
    }

    return chord.toString();
  }

  KeymapPlan::KeymapPlan(uimodel::KeymapModel const& keymap)
  {
    for (auto const& descriptor : actionDescriptors())
    {
      auto const bindingIt = keymap.bindings().find(descriptor.actionId);

      if (bindingIt == keymap.bindings().end())
      {
        continue;
      }

      for (auto const& chord : bindingIt->second)
      {
        auto optEvent = eventForChord(chord);

        if (!optEvent)
        {
          APP_LOG_DEBUG("TUI: shortcut '{}' for '{}' is not representable by the terminal adapter",
                        chord.toString(),
                        descriptor.actionId);
          continue;
        }

        if (isReservedRootEvent(*optEvent))
        {
          APP_LOG_DEBUG("TUI: shortcut '{}' for '{}' belongs to the fixed terminal protocol",
                        chord.toString(),
                        descriptor.actionId);
          continue;
        }

        auto const claimed = std::ranges::find(_entries, *optEvent, &Entry::event);

        if (claimed != _entries.end())
        {
          if (claimed->action != descriptor.action)
          {
            APP_LOG_WARN("TUI: shortcut '{}' for '{}' projects onto a terminal event already claimed by '{}'; "
                         "keeping the earlier action",
                         chord.toString(),
                         descriptor.actionId,
                         claimed->actionId);
          }

          continue;
        }

        _entries.push_back(Entry{.event = std::move(*optEvent),
                                 .action = descriptor.action,
                                 .actionId = descriptor.actionId,
                                 .shortcut = keyChordLabel(chord)});
      }
    }
  }

  std::optional<KeyAction> KeymapPlan::actionFor(ftxui::Event const& event) const
  {
    auto const found = std::ranges::find(_entries, event, &Entry::event);
    return found == _entries.end() ? std::nullopt : std::optional{found->action};
  }

  std::string_view KeymapPlan::shortcutFor(KeyAction const action,
                                           std::span<ftxui::Event const> const unavailableEvents) const noexcept
  {
    auto const found = std::ranges::find_if(
      _entries,
      [&](Entry const& entry)
      { return entry.action == action && !std::ranges::contains(unavailableEvents, entry.event); });
    return found == _entries.end() ? std::string_view{} : std::string_view{found->shortcut};
  }

  Result<> validateActionBindings(uimodel::KeymapModel const& candidate, std::string_view const actionId)
  {
    for (auto const& chord : candidate.chordsFor(actionId))
    {
      auto const optEvent = eventForChord(chord);

      if (!optEvent)
      {
        return makeError(Error::Code::NotSupported, chord.toString());
      }

      if (isReservedRootEvent(*optEvent))
      {
        return makeError(Error::Code::InvalidInput, chord.toString());
      }

      for (auto const& descriptor : actionDescriptors())
      {
        if (descriptor.actionId == actionId)
        {
          continue;
        }

        for (auto const& other : candidate.chordsFor(descriptor.actionId))
        {
          if (auto const optOther = eventForChord(other); optOther && *optOther == *optEvent)
          {
            return makeError(Error::Code::Conflict, descriptor.actionId);
          }
        }
      }
    }

    return {};
  }

  bool isPlaybackControl(KeyAction const action)
  {
    switch (action)
    {
      case KeyAction::PlaybackPlayPause:
      case KeyAction::PlaybackStop:
      case KeyAction::PlaybackPrevious:
      case KeyAction::PlaybackNext:
      case KeyAction::PlaybackShuffle:
      case KeyAction::PlaybackRepeat:
      case KeyAction::SeekBackward:
      case KeyAction::SeekForward:
      case KeyAction::VolumeDown:
      case KeyAction::VolumeUp: return true;
      default: return false;
    }
  }
} // namespace ao::tui
