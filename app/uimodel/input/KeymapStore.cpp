// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/uimodel/input/KeymapStore.h>

#include <ao/Error.h>
#include <ao/rt/ConfigStore.h>
#include <ao/rt/Log.h>
#include <ao/uimodel/input/KeymapModel.h>
#include <ao/yaml/Serialization.h>

#include <array>
#include <cstdint>
#include <expected>
#include <format>
#include <string>
#include <string_view>
#include <utility>

namespace ao::uimodel
{
  namespace
  {
    // Protected configuration (decision 0020): an older build never overwrites a newer group.
    struct KeymapOverridesYamlSchema final
    {
      static constexpr std::uint32_t kVersion = 1;

      Result<> serialize(ryml::NodeRef node, KeymapOverrides const& overrides) const
      {
        auto const writeBindings = [](ryml::NodeRef child, KeymapOverrides const& value)
        {
          return yaml::writeStringMap(child,
                                      value,
                                      "keymap overrides",
                                      [](ryml::NodeRef binding, auto const& chords)
                                      { return yaml::writeScalarSequence(binding, chords); });
        };

        auto writer = yaml::MapWriter{node};
        writer.scalar("version", kVersion).value("bindings", overrides, writeBindings);
        return std::move(writer).finish();
      }

      Result<KeymapOverrides> deserialize(ryml::ConstNodeRef node, KeymapOverrides const& /*seed*/) const
      {
        constexpr auto kContext = std::string_view{"keymap overrides"};

        if (auto const res = yaml::requireMap(node, kContext); !res)
        {
          return std::unexpected{res.error()};
        }

        auto const versionRes = yaml::requireScalar<std::uint32_t>(node, "version", kContext);

        if (!versionRes)
        {
          return std::unexpected{versionRes.error()};
        }

        // Version 1 is the first version any release writes, and the store refuses a
        // newer one first. Every older version stays readable (decision 0020), so the
        // first increment replaces this check with a reader for each older version.
        static_assert(kVersion == 1, "an older protected config version needs its own reader under decision 0020");

        if (*versionRes != kVersion)
        {
          return makeError(
            Error::Code::NotSupported, std::format("Unsupported keymap overrides version {}", *versionRes));
        }

        auto const readBindings = [](ryml::ConstNodeRef child, std::string_view context)
        {
          return yaml::readStringMap<KeymapOverrides>(
            child,
            context,
            [](ryml::ConstNodeRef binding, std::string_view bindingContext)
            { return yaml::readScalarSequence<std::string>(binding, bindingContext); });
        };

        constexpr auto kKeys = std::to_array<std::string_view>({"version", "bindings"});
        auto overrides = KeymapOverrides{};
        auto reader = yaml::MapReader{node, kKeys, kContext};
        reader.requiredValue("bindings", overrides, readBindings);
        return std::move(reader).finish(std::move(overrides));
      }
    };
  } // namespace

  KeymapModel loadKeymap(rt::ConfigStore& store, KeymapBindings defaults)
  {
    auto keymap = KeymapModel{std::move(defaults)};
    auto overrides = KeymapOverrides{};

    if (auto const res = store.load(kKeymapConfigGroup, overrides, KeymapOverridesYamlSchema{}); !res)
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

  Result<> saveKeymap(rt::ConfigStore& store, KeymapModel const& keymap)
  {
    auto const overrides = keymap.toOverrides();
    return store.save(kKeymapConfigGroup, overrides, KeymapOverridesYamlSchema{});
  }
} // namespace ao::uimodel
