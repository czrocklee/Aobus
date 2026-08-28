// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include <ao/winui/input/KeymapAcceleratorPlan.h>

#include <ao/rt/Log.h>
#include <ao/uimodel/input/KeyChord.h>
#include <ao/uimodel/input/KeymapModel.h>
#include <ao/uimodel/layout/component/LayoutSchema.h>
#include <ao/winui/input/KeyChordAccelerator.h>

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

namespace ao::winui
{
  namespace
  {
    /// Whether running @p actionId from a keystroke has anywhere to present.
    bool presentsWithoutAnchor(uimodel::LayoutSchema const& schema, std::string const& actionId)
    {
      auto const optActionSchema = schema.action(actionId);

      if (!optActionSchema)
      {
        // An action the shell offers but never described is still runnable; the
        // schema only decides what a document may bind.
        return true;
      }

      return !optActionSchema->supports(uimodel::ActionCapability::RequiresAnchor);
    }
  } // namespace

  std::vector<KeymapAcceleratorPlan> planKeymapAccelerators(uimodel::KeymapModel const& keymap,
                                                            uimodel::LayoutSchema const& schema,
                                                            KeymapActionAvailability const& isOffered,
                                                            CharacterKeyResolver const& resolveCharacter)
  {
    auto plans = std::vector<KeymapAcceleratorPlan>{};

    for (auto const& [actionId, chords] : keymap.bindings())
    {
      if (!isOffered || !isOffered(actionId))
      {
        continue;
      }

      if (!presentsWithoutAnchor(schema, actionId))
      {
        APP_LOG_DEBUG("KeymapAccelerators: '{}' presents from an anchor, which a keystroke has none of", actionId);
        continue;
      }

      for (auto const& chord : chords)
      {
        if (chord.isMediaKey())
        {
          // Windows delivers the transport keys to whichever application
          // registered for system media control, which this shell does. An
          // accelerator for the same key would run the command a second time
          // whenever the window happens to be focused.
          continue;
        }

        auto const optKey = toWindowsAccelerator(chord, resolveCharacter);

        if (!optKey)
        {
          APP_LOG_DEBUG("KeymapAccelerators: '{}' has no Windows key for '{}'", actionId, chord.toString());
          continue;
        }

        auto candidate = KeymapAcceleratorPlan{.actionId = actionId, .key = *optKey};
        auto const claimed = std::ranges::find(plans, candidate.key, &KeymapAcceleratorPlan::key);

        if (claimed != plans.end())
        {
          if (claimed->actionId != candidate.actionId)
          {
            // Two chords can reach one Windows key even when they read
            // differently: `+` and `Shift+=` are the same keystroke. Installing
            // both would leave which action runs up to XAML's ordering, so the
            // first one the map declares keeps the key and the second is
            // reported rather than silently shadowed.
            APP_LOG_WARN("KeymapAccelerators: '{}' wanted the key '{}' already taken by '{}'",
                         actionId,
                         chord.toString(),
                         claimed->actionId);
          }

          continue;
        }

        plans.push_back(std::move(candidate));
      }
    }

    return plans;
  }
} // namespace ao::winui
