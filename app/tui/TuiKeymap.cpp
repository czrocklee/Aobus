// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "TuiKeymap.h"

#include <ao/Contract.h>
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
    constexpr auto kQuitDefaults = std::to_array<std::string_view>({"Q"});
    constexpr auto kToggleListChooserDefaults = std::to_array<std::string_view>({"L"});
    constexpr auto kToggleDetailsDefaults = std::to_array<std::string_view>({"D"});
    constexpr auto kToggleAudioPipelineDefaults = std::to_array<std::string_view>({"A"});
    constexpr auto kToggleOutputDevicesDefaults = std::to_array<std::string_view>({"O"});
    constexpr auto kTogglePresentationsDefaults = std::to_array<std::string_view>({"V"});
    constexpr auto kToggleNotificationsDefaults = std::to_array<std::string_view>({"N"});
    constexpr auto kShowHelpDefaults = std::to_array<std::string_view>({"?"});
    constexpr auto kOpenCommandPaletteDefaults = std::to_array<std::string_view>({":"});
    constexpr auto kOpenQuickFilterDefaults = std::to_array<std::string_view>({"/"});
    constexpr auto kClearFilterDefaults = std::to_array<std::string_view>({"C"});
    constexpr auto kReloadDefaults = std::to_array<std::string_view>({"R"});
    constexpr auto kPlaySelectionDefaults = std::to_array<std::string_view>({"Enter", "P"});
    constexpr auto kPreviousSectionDefaults = std::to_array<std::string_view>({"{"});
    constexpr auto kNextSectionDefaults = std::to_array<std::string_view>({"}"});
    constexpr auto kSeekBackwardDefaults = std::to_array<std::string_view>({"["});
    constexpr auto kSeekForwardDefaults = std::to_array<std::string_view>({"]"});
    constexpr auto kVolumeDownDefaults = std::to_array<std::string_view>({"-"});
    constexpr auto kVolumeUpDefaults = std::to_array<std::string_view>({"+", "="});
    constexpr auto kPlayPauseDefaults = std::to_array<std::string_view>({"Space"});
    constexpr auto kStopDefaults = std::to_array<std::string_view>({"S"});

    std::vector<TuiActionDescriptor> makeDescriptors()
    {
      using enum uimodel::PlaybackCommand;

      return {
        {.actionId = "tui.shell.quit", .action = TuiKeyAction::Quit, .defaultChords = kQuitDefaults},
        {.actionId = "tui.shell.toggleListChooser",
         .action = TuiKeyAction::ToggleListChooser,
         .defaultChords = kToggleListChooserDefaults},
        {.actionId = "tui.shell.toggleTrackDetail",
         .action = TuiKeyAction::ToggleDetails,
         .defaultChords = kToggleDetailsDefaults},
        {.actionId = "tui.shell.toggleAudioQuality",
         .action = TuiKeyAction::ToggleAudioPipeline,
         .defaultChords = kToggleAudioPipelineDefaults},
        {.actionId = "tui.shell.toggleOutputDevices",
         .action = TuiKeyAction::ToggleOutputDevices,
         .defaultChords = kToggleOutputDevicesDefaults},
        {.actionId = "tui.shell.togglePresentationChooser",
         .action = TuiKeyAction::TogglePresentations,
         .defaultChords = kTogglePresentationsDefaults},
        {.actionId = "tui.shell.toggleNotifications",
         .action = TuiKeyAction::ToggleNotifications,
         .defaultChords = kToggleNotificationsDefaults},
        {.actionId = "tui.shell.showHelp", .action = TuiKeyAction::ShowHelp, .defaultChords = kShowHelpDefaults},
        {.actionId = "tui.shell.openCommandPalette",
         .action = TuiKeyAction::OpenCommandPalette,
         .defaultChords = kOpenCommandPaletteDefaults},
        {.actionId = "tui.library.openQuickFilter",
         .action = TuiKeyAction::OpenQuickFilter,
         .defaultChords = kOpenQuickFilterDefaults},
        {.actionId = std::string{uimodel::kRevealCurrentTrackActionId},
         .action = TuiKeyAction::RevealCurrentTrack,
         .defaultChords = kNoDefaults},
        {.actionId = "tui.library.clearFilter",
         .action = TuiKeyAction::ClearFilter,
         .defaultChords = kClearFilterDefaults},
        {.actionId = "tui.library.reloadActiveList", .action = TuiKeyAction::Reload, .defaultChords = kReloadDefaults},
        {.actionId = "tui.library.scan", .action = TuiKeyAction::Scan, .defaultChords = kNoDefaults},
        {.actionId = "tui.library.scanCancel", .action = TuiKeyAction::ScanCancel, .defaultChords = kNoDefaults},
        {.actionId = "tui.library.playSelection",
         .action = TuiKeyAction::PlaySelection,
         .defaultChords = kPlaySelectionDefaults},
        {.actionId = "tui.library.previousSection",
         .action = TuiKeyAction::PreviousSection,
         .defaultChords = kPreviousSectionDefaults},
        {.actionId = "tui.library.nextSection",
         .action = TuiKeyAction::NextSection,
         .defaultChords = kNextSectionDefaults},
        {.actionId = "tui.playback.seekBackward",
         .action = TuiKeyAction::SeekBackward,
         .defaultChords = kSeekBackwardDefaults},
        {.actionId = "tui.playback.seekForward",
         .action = TuiKeyAction::SeekForward,
         .defaultChords = kSeekForwardDefaults},
        {.actionId = "tui.playback.volumeDown",
         .action = TuiKeyAction::VolumeDown,
         .defaultChords = kVolumeDownDefaults},
        {.actionId = "tui.playback.volumeUp", .action = TuiKeyAction::VolumeUp, .defaultChords = kVolumeUpDefaults},
        {.actionId = uimodel::playbackCommandActionId(PlayPause),
         .action = TuiKeyAction::PlaybackPlayPause,
         .defaultChords = kPlayPauseDefaults},
        {.actionId = uimodel::playbackCommandActionId(Stop),
         .action = TuiKeyAction::PlaybackStop,
         .defaultChords = kStopDefaults},
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

    std::string displayChord(uimodel::KeyChord const& chord)
    {
      if (chord.modifiers.isEmpty() && chord.key.size() == 1 && utility::isAsciiAlpha(chord.key.front()))
      {
        return std::string{utility::toAsciiLower(chord.key.front())};
      }

      return chord.toString();
    }

    bool isReservedRootEvent(ftxui::Event const& event)
    {
      return event == ftxui::Event::CtrlC || event == ftxui::Event::Escape || event == ftxui::Event::ArrowUp ||
             event == ftxui::Event::ArrowDown || event == ftxui::Event::Home || event == ftxui::Event::End ||
             event == ftxui::Event::PageUp || event == ftxui::Event::PageDown;
    }
  } // namespace

  std::span<TuiActionDescriptor const> tuiActionDescriptors()
  {
    static auto const descriptors = makeDescriptors();
    return descriptors;
  }

  uimodel::KeymapBindings tuiDefaultKeymap()
  {
    auto bindings = uimodel::defaultKeymap();

    for (auto const& descriptor : tuiActionDescriptors())
    {
      auto& chords = bindings[descriptor.actionId];
      auto local = std::vector<uimodel::KeyChord>{};
      local.reserve(descriptor.defaultChords.size());

      for (auto const text : descriptor.defaultChords)
      {
        auto optChord = uimodel::KeyChord::parse(text);
        AO_INVARIANT(optChord, "Invalid built-in TUI key chord: {}", text);

        if (!std::ranges::contains(local, *optChord) && !std::ranges::contains(chords, *optChord))
        {
          local.push_back(std::move(*optChord));
        }
      }

      chords.insert(chords.begin(), local.begin(), local.end());
    }

    return bindings;
  }

  std::optional<ftxui::Event> tuiEventForChord(uimodel::KeyChord const& chord)
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

  TuiKeymapPlan::TuiKeymapPlan(uimodel::KeymapModel const& keymap)
  {
    for (auto const& descriptor : tuiActionDescriptors())
    {
      auto const bindingIt = keymap.bindings().find(descriptor.actionId);

      if (bindingIt == keymap.bindings().end())
      {
        continue;
      }

      for (auto const& chord : bindingIt->second)
      {
        auto optEvent = tuiEventForChord(chord);

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
                                 .shortcut = displayChord(chord)});
      }
    }
  }

  std::optional<TuiKeyAction> TuiKeymapPlan::actionFor(ftxui::Event const& event) const
  {
    auto const found = std::ranges::find(_entries, event, &Entry::event);
    return found == _entries.end() ? std::nullopt : std::optional{found->action};
  }

  std::string_view TuiKeymapPlan::shortcutFor(TuiKeyAction const action,
                                              std::span<ftxui::Event const> const unavailableEvents) const noexcept
  {
    auto const found = std::ranges::find_if(
      _entries,
      [&](Entry const& entry)
      { return entry.action == action && !std::ranges::contains(unavailableEvents, entry.event); });
    return found == _entries.end() ? std::string_view{} : std::string_view{found->shortcut};
  }
} // namespace ao::tui
