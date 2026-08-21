// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "platform/StringResources.h"
#include <ao/i18n/MessageCatalog.h>

#include <winrt/base.h>

#include <array>
#include <cstdint>
#include <exception>
#include <iomanip>
#include <iostream>
#include <string_view>

namespace
{
  struct ProbeCase final
  {
    std::string_view requestedLocale;
    ao::i18n::MessageId messageId;
    std::string_view resourceId;
    std::string_view argumentName{};
    std::string_view argumentValue{};
  };

  struct LegacyProbeCase final
  {
    std::string_view requestedLocale;
    std::string_view resourceId;
    std::string_view expectedText;
  };

  std::int32_t run()
  {
    auto systemCatalogRes = ao::i18n::MessageCatalog::createForSystemLocale();

    if (!systemCatalogRes)
    {
      std::cerr << "Could not construct the ICU catalog for the preferred Windows UI language: "
                << systemCatalogRes.error().message << '\n';
      return 1;
    }

    constexpr auto kCases = std::to_array<ProbeCase>({
      {.requestedLocale = "en-GB",
       .messageId = ao::i18n::MessageId::PilotLibraryTitle,
       .resourceId = "pilot_library_title"},
      {.requestedLocale = "de-DE",
       .messageId = ao::i18n::MessageId::PilotRegionalFallback,
       .resourceId = "pilot_regional_fallback"},
      {.requestedLocale = "de-AT",
       .messageId = ao::i18n::MessageId::PilotEnglishFallback,
       .resourceId = "pilot_english_fallback"},
      {.requestedLocale = "sv-SE",
       .messageId = ao::i18n::MessageId::PilotLibraryTitle,
       .resourceId = "pilot_library_title"},
      {.requestedLocale = "qps-ploc",
       .messageId = ao::i18n::MessageId::PilotLibraryTitle,
       .resourceId = "pilot_library_title"},
      {.requestedLocale = "de-DE",
       .messageId = ao::i18n::MessageId::TrackFieldTitle,
       .resourceId = "track_field_title"},
      {.requestedLocale = "de-AT",
       .messageId = ao::i18n::MessageId::TrackPresentationLibrary,
       .resourceId = "track_presentation_library"},
      {.requestedLocale = "de-DE",
       .messageId = ao::i18n::MessageId::TrackPresentationPickerLabel,
       .resourceId = "track_presentation_picker_label"},
      {.requestedLocale = "en-GB",
       .messageId = ao::i18n::MessageId::TrackFieldChannels,
       .resourceId = "track_field_channels"},
      {.requestedLocale = "qps-ploc",
       .messageId = ao::i18n::MessageId::TrackPresentationTechnical,
       .resourceId = "track_presentation_technical"},
      {.requestedLocale = "de-DE",
       .messageId = ao::i18n::MessageId::WinUiShellOpenLibrary,
       .resourceId = "winui_shell_open_library"},
      {.requestedLocale = "de-AT",
       .messageId = ao::i18n::MessageId::WinUiPlaybackVolume,
       .resourceId = "winui_playback_volume"},
      {.requestedLocale = "de-DE",
       .messageId = ao::i18n::MessageId::WinUiPlaybackOutputDevice,
       .resourceId = "winui_playback_output_device"},
      {.requestedLocale = "qps-ploc",
       .messageId = ao::i18n::MessageId::WinUiPlaybackNoOutputDevices,
       .resourceId = "winui_playback_no_output_devices"},
      {.requestedLocale = "de-DE",
       .messageId = ao::i18n::MessageId::WinUiLibraryNavigationPaneTitle,
       .resourceId = "winui_library_navigation_pane_title"},
      {.requestedLocale = "de-AT",
       .messageId = ao::i18n::MessageId::WinUiLibraryQuickFilterPlaceholder,
       .resourceId = "winui_library_quick_filter_placeholder"},
      {.requestedLocale = "qps-ploc",
       .messageId = ao::i18n::MessageId::WinUiLibraryNoSelection,
       .resourceId = "winui_library_no_selection"},
      {.requestedLocale = "de-DE",
       .messageId = ao::i18n::MessageId::WinUiTrackMoveColumnLeft,
       .resourceId = "winui_track_move_column_left_button/Text"},
      {.requestedLocale = "qps-ploc",
       .messageId = ao::i18n::MessageId::WinUiTrackMoveColumnRight,
       .resourceId = "winui_track_move_column_right_button/Text"},
      {.requestedLocale = "de-DE", .messageId = ao::i18n::MessageId::WinUiMoreMenu, .resourceId = "winui_more_menu"},
      {.requestedLocale = "qps-ploc",
       .messageId = ao::i18n::MessageId::WinUiUnavailableTrack,
       .resourceId = "winui_unavailable_track"},
      {.requestedLocale = "de-DE",
       .messageId = ao::i18n::MessageId::WinUiError,
       .resourceId = "winui_error",
       .argumentName = "detail",
       .argumentValue = "Datei fehlt"},
      {.requestedLocale = "de-AT",
       .messageId = ao::i18n::MessageId::WinUiColumnNotSortable,
       .resourceId = "winui_column_not_sortable",
       .argumentName = "column",
       .argumentValue = "Album"},
    });

    for (auto const& probe : kCases)
    {
      auto catalogRes = ao::i18n::MessageCatalog::create(probe.requestedLocale);

      if (!catalogRes)
      {
        std::cerr << "Could not construct the ICU catalog for " << probe.requestedLocale << ": "
                  << catalogRes.error().message << '\n';
        return 2;
      }

      auto configureRes = ao::winui::configureResourceLanguage(catalogRes->requestedLocale());

      if (!configureRes)
      {
        std::cerr << "Could not configure MRT for " << probe.requestedLocale << ": " << configureRes.error().message
                  << '\n';
        return 3;
      }

      auto messageRes = probe.argumentName.empty()
                          ? catalogRes->format(probe.messageId)
                          : catalogRes->format(probe.messageId, {{probe.argumentName, probe.argumentValue}});
      auto const mrtText = probe.argumentName.empty()
                             ? ao::winui::resourceString(probe.resourceId)
                             : ao::winui::formatResource(probe.resourceId, probe.argumentValue);
      ao::winui::resetResourceLanguage();

      if (!messageRes)
      {
        std::cerr << "Could not format the ICU message for " << probe.requestedLocale << ": "
                  << messageRes.error().message << '\n';
        return 4;
      }

      if (mrtText != messageRes->text)
      {
        std::cerr << "MRT and ICU selected different text for " << probe.requestedLocale << " and " << probe.resourceId
                  << "\n  MRT: " << std::quoted(mrtText) << "\n  ICU: " << std::quoted(messageRes->text) << '\n';
        return 5;
      }
    }

    constexpr auto kLegacyCases = std::to_array<LegacyProbeCase>({
      {.requestedLocale = "en-GB", .resourceId = "AppTitleValue", .expectedText = "Aobus"},
      {.requestedLocale = "de-DE", .resourceId = "SoulWindowTitle", .expectedText = "Aobus Soul"},
      {.requestedLocale = "qps-ploc", .resourceId = "AppTitleValue", .expectedText = "Aobus"},
    });

    for (auto const& probe : kLegacyCases)
    {
      auto catalogRes = ao::i18n::MessageCatalog::create(probe.requestedLocale);

      if (!catalogRes)
      {
        std::cerr << "Could not construct the ICU catalog for legacy resource " << probe.resourceId << ": "
                  << catalogRes.error().message << '\n';
        return 6;
      }

      auto configureRes = ao::winui::configureResourceLanguage(catalogRes->requestedLocale());

      if (!configureRes)
      {
        std::cerr << "Could not configure MRT for legacy resource " << probe.resourceId << ": "
                  << configureRes.error().message << '\n';
        return 7;
      }

      auto const text = ao::winui::resourceString(probe.resourceId);
      ao::winui::resetResourceLanguage();

      if (text != probe.expectedText)
      {
        std::cerr << "MRT did not resolve neutral-English legacy resource " << probe.resourceId << " for "
                  << probe.requestedLocale << "\n  actual: " << std::quoted(text)
                  << "\n  expected: " << std::quoted(probe.expectedText) << '\n';
        return 8;
      }
    }

    return 0;
  }
} // namespace

int main()
{
  try
  {
    winrt::init_apartment();
    return run();
  }
  catch (std::exception const& error)
  {
    std::cerr << "WinUI localization probe failed: " << error.what() << '\n';
    return 9;
  }
}
