// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/Error.h>
#include <ao/rt/ConfigStore.h>
#include <ao/rt/Log.h>
#include <ao/uimodel/input/KeymapModel.h>
#include <ao/uimodel/input/KeymapStore.h>

#include <utility>

namespace ao::uimodel::input
{
  KeymapModel loadKeymap(rt::ConfigStore& store, KeymapBindings defaults)
  {
    auto keymap = KeymapModel{std::move(defaults)};
    auto overrides = KeymapOverrides{};

    if (auto const res = store.load(kKeymapConfigGroup, overrides); !res)
    {
      if (res.error().code != Error::Code::NotFound)
      {
        APP_LOG_WARN("KeymapStore: failed to load keymap overrides: {}", res.error().message);
      }

      return keymap; // defaults only
    }

    if (auto const diagnostics = keymap.applyOverrides(overrides); !diagnostics.empty())
    {
      for (auto const& entry : diagnostics)
      {
        APP_LOG_WARN("KeymapStore: ignoring invalid shortcut binding '{}'", entry);
      }
    }

    return keymap;
  }

  void saveKeymap(rt::ConfigStore& store, KeymapModel const& keymap)
  {
    store.save(kKeymapConfigGroup, keymap.toOverrides());

    if (auto const res = store.flush(); !res)
    {
      APP_LOG_ERROR("KeymapStore: failed to flush keymap overrides: {}", res.error().message);
    }
  }
} // namespace ao::uimodel::input
