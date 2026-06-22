// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/Exception.h>
#include <ao/uimodel/input/KeyChord.h>
#include <ao/uimodel/input/KeymapModel.h>
#include <ao/uimodel/layout/ActionCatalog.h>

#include <algorithm>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ao::uimodel::input
{
  namespace
  {
    bool containsChord(std::vector<KeyChord> const& chords, KeyChord const& chord)
    {
      return std::ranges::contains(chords, chord);
    }
  }

  KeymapModel::KeymapModel(KeymapBindings defaults)
    : _defaults{std::move(defaults)}, _effective{_defaults}
  {
  }

  std::vector<std::string> KeymapModel::applyOverrides(KeymapOverrides const& overrides)
  {
    _effective = _defaults;
    auto diagnostics = std::vector<std::string>{};

    for (auto const& [actionId, chordStrings] : overrides)
    {
      auto chords = std::vector<KeyChord>{};

      for (auto const& text : chordStrings)
      {
        if (auto optChord = KeyChord::parse(text); optChord && optChord->valid())
        {
          if (!containsChord(chords, *optChord))
          {
            chords.push_back(std::move(*optChord));
          }
        }
        else
        {
          auto diag = actionId;
          diag += ": ";
          diag += text;
          diagnostics.push_back(std::move(diag));
        }
      }

      _effective[actionId] = std::move(chords);
    }

    return diagnostics;
  }

  std::vector<KeyChord> KeymapModel::chordsFor(std::string_view actionId) const
  {
    if (auto const it = _effective.find(actionId); it != _effective.end())
    {
      return it->second;
    }

    return {};
  }

  std::optional<std::string> KeymapModel::actionFor(KeyChord const& chord) const
  {
    for (auto const& [actionId, chords] : _effective)
    {
      if (containsChord(chords, chord))
      {
        return actionId;
      }
    }

    return std::nullopt;
  }

  std::vector<KeymapConflict> KeymapModel::conflicts() const
  {
    // Group action ids by chord, keyed by canonical chord text for deterministic order.
    auto byChord = std::map<std::string, std::pair<KeyChord, std::vector<std::string>>>{};

    for (auto const& [actionId, chords] : _effective)
    {
      for (auto const& chord : chords)
      {
        auto& entry = byChord[chord.toString()];
        entry.first = chord;
        entry.second.push_back(actionId);
      }
    }

    auto result = std::vector<KeymapConflict>{};

    for (auto& [text, entry] : byChord)
    {
      if (entry.second.size() > 1)
      {
        result.push_back(KeymapConflict{.chord = entry.first, .actionIds = std::move(entry.second)});
      }
    }

    return result;
  }

  std::vector<std::string> KeymapModel::unknownActionIds(layout::ActionCatalog const& catalog) const
  {
    auto result = std::vector<std::string>{};

    for (auto const& [actionId, chords] : _effective)
    {
      if (chords.empty())
      {
        continue;
      }

      if (!catalog.descriptor(actionId))
      {
        result.push_back(actionId);
      }
    }

    return result;
  }

  bool KeymapModel::bind(std::string actionId, KeyChord chord)
  {
    if (!chord.valid())
    {
      return false;
    }

    auto& chords = _effective[actionId];

    if (containsChord(chords, chord))
    {
      return false;
    }

    chords.push_back(std::move(chord));
    return true;
  }

  bool KeymapModel::unbind(std::string_view actionId, KeyChord const& chord)
  {
    auto const it = _effective.find(actionId);

    if (it == _effective.end())
    {
      return false;
    }

    auto& chords = it->second;
    auto const erased = std::erase(chords, chord);
    return erased > 0;
  }

  void KeymapModel::resetToDefault(std::string_view actionId)
  {
    if (auto const it = _defaults.find(actionId); it != _defaults.end())
    {
      _effective[std::string{actionId}] = it->second;
    }
    else
    {
      _effective.erase(std::string{actionId});
    }
  }

  void KeymapModel::resetAllToDefault()
  {
    _effective = _defaults;
  }

  KeymapOverrides KeymapModel::toOverrides() const
  {
    auto overrides = KeymapOverrides{};

    auto const serialize = [](std::vector<KeyChord> const& chords)
    {
      auto strings = std::vector<std::string>{};
      strings.reserve(chords.size());

      for (auto const& chord : chords)
      {
        strings.push_back(chord.toString());
      }

      return strings;
    };

    for (auto const& [actionId, chords] : _effective)
    {
      if (auto const defaultIt = _defaults.find(actionId); defaultIt != _defaults.end() && defaultIt->second == chords)
      {
        continue; // identical to default; no override needed
      }

      overrides[actionId] = serialize(chords);
    }

    return overrides;
  }

  KeymapBindings defaultKeymap()
  {
    auto const chord = [](std::string text)
    {
      if (auto optChord = KeyChord::parse(text); optChord)
      {
        return *optChord;
      }

      throwException<Exception>("Invalid default key chord: {}", text);
    };

    return KeymapBindings{
      {"playback.playPause", {chord("Ctrl+P"), chord("Media:Play"), chord("Media:Pause")}},
      {"playback.stop", {chord("Media:Stop")}},
      {"playback.next", {chord("Ctrl+Right"), chord("Media:Next")}},
      {"playback.previous", {chord("Ctrl+Left"), chord("Media:Prev")}},
      {"playback.toggleShuffle", {chord("Ctrl+U")}},
      {"playback.cycleRepeat", {chord("Ctrl+R")}},
      {"workspace.revealCurrentTrack", {chord("Ctrl+L")}},
    };
  }
}
