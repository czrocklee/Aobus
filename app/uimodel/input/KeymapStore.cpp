// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/uimodel/input/KeymapStore.h>

#include <ao/Error.h>
#include <ao/rt/ConfigStore.h>
#include <ao/rt/Log.h>
#include <ao/uimodel/input/KeymapModel.h>
#include <ao/yaml/Serialization.h>

#include <string>
#include <string_view>
#include <utility>

namespace ao::uimodel
{
  namespace
  {
    struct KeymapOverridesYamlSchema final
    {
      Result<> serialize(ryml::NodeRef node, KeymapOverrides const& overrides) const
      {
        return yaml::writeStringMap(node,
                                    overrides,
                                    "keymap overrides",
                                    [](ryml::NodeRef child, auto const& bindings)
                                    { return yaml::writeScalarSequence(child, bindings); });
      }

      Result<KeymapOverrides> deserialize(ryml::ConstNodeRef node, KeymapOverrides const& /*seed*/) const
      {
        constexpr auto kContext = std::string_view{"keymap overrides"};
        return yaml::readStringMap<KeymapOverrides>(node,
                                                    kContext,
                                                    [](ryml::ConstNodeRef child, std::string_view context)
                                                    { return yaml::readScalarSequence<std::string>(child, context); });
      }
    };
  } // namespace

  KeymapModel loadKeymap(rt::ConfigStore& store, KeymapBindings defaults)
  {
    auto keymap = KeymapModel{std::move(defaults)};
    auto overrides = KeymapOverrides{};

    if (auto const resRes = store.load(kKeymapConfigGroup, overrides, KeymapOverridesYamlSchema{}); !resRes)
    {
      if (resRes.error().code != Error::Code::NotFound)
      {
        APP_LOG_WARN("KeymapStore: failed to load keymap overrides: {}", resRes.error().message);
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

  Result<> saveKeymap(rt::ConfigStore& store, KeymapModel const& keymap)
  {
    auto const overrides = keymap.toOverrides();
    return store.save(kKeymapConfigGroup, overrides, KeymapOverridesYamlSchema{});
  }
} // namespace ao::uimodel
