// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "WinUiTrackSelectionProbe.h"

#include <ao/CoreIds.h>

#pragma push_macro("GetCurrentTime")
#include <unknwn.h>
#pragma pop_macro("GetCurrentTime")

#include <winrt/Aobus.h>
#include <winrt/Microsoft.UI.Xaml.Automation.Peers.h>
#include <winrt/Microsoft.UI.Xaml.Automation.Provider.h>
#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <winrt/Microsoft.UI.Xaml.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>

#include <algorithm>
#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <vector>

namespace ao::winui::test
{
  std::optional<std::string> trackSelectionProviderFailure(winrt::Microsoft::UI::Xaml::Controls::ListView const& rows,
                                                           std::vector<TrackId> const& expected)
  {
    using namespace winrt::Microsoft::UI::Xaml;
    using namespace Automation::Peers;
    using Automation::Provider::ISelectionProvider;
    using winrt::Windows::Foundation::IInspectable;
    using winrt::Windows::Foundation::Collections::IVectorView;

    if (!rows || !rows.IsLoaded() || rows.SelectionMode() != Controls::ListViewSelectionMode::Extended)
    {
      return "UIA readback requires the loaded, extended-selection track list";
    }

    auto const peer = FrameworkElementAutomationPeer::CreatePeerForElement(rows);
    auto const selection = peer ? peer.GetPattern(PatternInterface::Selection).try_as<ISelectionProvider>() : nullptr;

    if (!selection)
    {
      return "the track list must expose the UIA Selection pattern";
    }

    auto const multiple = selection.CanSelectMultiple();

    if (auto const required = selection.IsSelectionRequired(); !multiple || required)
    {
      return std::format("UIA Selection must allow multiple tracks and an empty selection (multiple={}, required={})",
                         multiple,
                         required);
    }

    // Decode providers from the real list peer in the probe only. Item() does not need a realized row.
    auto const decoder = peer.try_as<IAutomationPeerProtected>();
    auto const source = rows.ItemsSource().try_as<IVectorView<IInspectable>>();

    if (!decoder || !source)
    {
      return "UIA readback requires the list peer's provider decoder and its mounted indexed source";
    }

    auto const listIdentity = peer.as<::IUnknown>();
    auto actual = std::vector<TrackId>{};

    for (auto const& provider : selection.GetSelection())
    {
      auto const item = provider ? decoder.PeerFromProvider(provider).try_as<ItemAutomationPeer>() : nullptr;
      auto const row = item ? item.Item().try_as<winrt::Aobus::TrackRowItem>() : nullptr;
      auto const owner = item ? item.ItemsControlAutomationPeer() : nullptr;
      std::uint32_t index = 0;

      if (!row || row.IsGroupHeader())
      {
        return "UIA Selection returned a provider that does not identify a track row";
      }

      if (!owner || owner.as<::IUnknown>().get() != listIdentity.get() || !source.IndexOf(row, index))
      {
        return std::format("UIA Selection returned track {} from the wrong list peer or row source", row.TrackId());
      }

      actual.emplace_back(row.TrackId());
    }

    if (!std::ranges::is_permutation(actual, expected))
    {
      auto const describe = [](std::vector<TrackId> const& ids)
      {
        auto text = std::string{};

        for (auto const id : ids)
        {
          if (!text.empty())
          {
            text += ", ";
          }

          text += std::to_string(id.raw());
        }

        return text;
      };
      return std::format("UIA Selection expected track IDs [{}], got [{}]", describe(expected), describe(actual));
    }

    return std::nullopt;
  }
} // namespace ao::winui::test
