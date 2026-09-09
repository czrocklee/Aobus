// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "Preferences.h"

#include "CoverArt.h"
#include <ao/Error.h>
#include <ao/i18n/MessageCatalog.h>
#include <ao/rt/ConfigStore.h>
#include <ao/yaml/Serialization.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <expected>
#include <string_view>
#include <utility>

namespace ao::tui
{
  namespace
  {
    Result<> validatePreferences(Preferences const& value)
    {
      if ((!value.language.empty() &&
           !std::ranges::contains(i18n::availableCatalogLocales(), value.language, &i18n::CatalogLocale::tag)) ||
          !std::ranges::contains(kCoverArtModes, value.coverArtMode, &CoverArtModeDescriptor::name) ||
          value.wheelStep < 1 || value.wheelStep > kMaximumWheelStep || value.seekSeconds < 1 ||
          value.seekSeconds > kMaximumSeekSeconds || value.volumePercent < 1 ||
          value.volumePercent > kMaximumVolumePercent)
      {
        return makeError(Error::Code::InvalidInput, "Invalid TUI preference value");
      }

      return {};
    }

    struct PreferencesSchema final
    {
      Result<> serialize(ryml::NodeRef node, Preferences const& value) const
      {
        if (auto const res = validatePreferences(value); !res)
        {
          return res;
        }

        auto writer = yaml::MapWriter{node};
        writer.scalar("version", 1)
          .scalar("language", value.language)
          .scalar("coverArtMode", value.coverArtMode)
          .scalar("dimBackdrop", value.dimBackdrop)
          .scalar("reducedMotion", value.reducedMotion)
          .scalar("mouseEnabled", value.mouseEnabled)
          .scalar("qualityHover", value.qualityHover)
          .scalar("wheelStep", value.wheelStep)
          .scalar("seekSeconds", value.seekSeconds)
          .scalar("volumePercent", value.volumePercent);
        return std::move(writer).finish();
      }

      Result<Preferences> deserialize(ryml::ConstNodeRef node, Preferences const& /*seed*/) const
      {
        constexpr auto kKeys = std::to_array<std::string_view>({"version",
                                                                "language",
                                                                "coverArtMode",
                                                                "dimBackdrop",
                                                                "reducedMotion",
                                                                "mouseEnabled",
                                                                "qualityHover",
                                                                "wheelStep",
                                                                "seekSeconds",
                                                                "volumePercent"});
        auto value = Preferences{};
        std::int32_t version = 0;
        auto reader = yaml::MapReader{node, kKeys, "TUI preferences"};
        reader.requiredScalar("version", version)
          .optionalScalar("language", value.language)
          .optionalScalar("coverArtMode", value.coverArtMode)
          .optionalScalar("dimBackdrop", value.dimBackdrop)
          .optionalScalar("reducedMotion", value.reducedMotion)
          .optionalScalar("mouseEnabled", value.mouseEnabled)
          .optionalScalar("qualityHover", value.qualityHover)
          .optionalScalar("wheelStep", value.wheelStep)
          .optionalScalar("seekSeconds", value.seekSeconds)
          .optionalScalar("volumePercent", value.volumePercent);
        auto readRes = std::move(reader).finish(std::move(value));

        if (!readRes)
        {
          return readRes;
        }

        if (version != 1)
        {
          return makeError(Error::Code::NotSupported, "Unsupported TUI preferences version");
        }

        if (auto const res = validatePreferences(*readRes); !res)
        {
          return std::unexpected{res.error()};
        }

        return readRes;
      }
    };
  } // namespace

  Result<Preferences> loadPreferences(rt::ConfigStore& store)
  {
    auto preferences = Preferences{};

    if (auto const res = store.load("preferences", preferences, PreferencesSchema{}); !res)
    {
      return std::unexpected{res.error()};
    }

    return preferences;
  }

  Result<> savePreferences(rt::ConfigStore& store, Preferences const& preferences)
  {
    if (!store.hasLocation())
    {
      return makeError(Error::Code::NotFound, "No persistent TUI configuration location");
    }

    return store.save("preferences", preferences, PreferencesSchema{});
  }
} // namespace ao::tui
