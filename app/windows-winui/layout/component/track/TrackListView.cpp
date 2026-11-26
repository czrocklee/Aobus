// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "layout/component/track/TrackListView.h"

#include <UIAutomationCore.h>
#include <UIAutomationCoreApi.h>
#include <winrt/Aobus.h>
#include <winrt/Microsoft.UI.Xaml.Automation.Peers.h>
#include <winrt/Microsoft.UI.Xaml.Automation.Provider.h>
#include <winrt/Microsoft.UI.Xaml.Data.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>

#include <cstdint>
#include <vector>

namespace ao::winui::layout
{
  namespace
  {
    namespace controls = winrt::Microsoft::UI::Xaml::Controls;
    namespace peers = winrt::Microsoft::UI::Xaml::Automation::Peers;
    namespace provider = winrt::Microsoft::UI::Xaml::Automation::Provider;
    using winrt::Windows::Foundation::IInspectable;
    using Items = winrt::Windows::Foundation::Collections::IVectorView<IInspectable>;

    // The stock ListView peer enumerates SelectedItems, which WinUI does not maintain
    // for ISelectionInfo sources. Keep all other patterns and the native data-item peers.
    struct TrackListPeer : peers::ListViewAutomationPeerT<TrackListPeer, provider::ISelectionProvider>
    {
      using Base = ListViewAutomationPeerT<TrackListPeer, provider::ISelectionProvider>;

      explicit TrackListPeer(controls::ListView const& owner)
        : Base{owner}
      {
      }

      IInspectable GetPatternCore(peers::PatternInterface const pattern)
      {
        auto native = Base::GetPatternCore(pattern);

        if (native && pattern == peers::PatternInterface::Selection)
        {
          return get_strong().as<provider::ISelectionProvider>();
        }

        return native;
      }

      bool CanSelectMultiple() const
      {
        auto const mode = ownerList().SelectionMode();
        return mode == controls::ListViewSelectionMode::Multiple || mode == controls::ListViewSelectionMode::Extended;
      }

      bool IsSelectionRequired() const noexcept { return false; }

      winrt::com_array<provider::IRawElementProviderSimple> GetSelection() const
      {
        auto const rows = ownerList();
        auto const items = rows.ItemsSource().try_as<Items>();

        if (!items)
        {
          return {};
        }

        auto selected = std::vector<provider::IRawElementProviderSimple>{};

        for (auto const& range : rows.SelectedRanges())
        {
          auto const first = static_cast<std::uint32_t>(range.FirstIndex());
          auto const length = range.Length();

          for (std::uint32_t offset = 0; offset < length; ++offset)
          {
            // Ask only for the selected data items. No container or scrolling is needed.
            auto const row = items.GetAt(first + offset).as<winrt::Aobus::TrackRowItem>();

            if (!row.IsGroupHeader())
            {
              selected.push_back(ProviderFromPeer(CreateItemAutomationPeer(row)));
            }
          }
        }

        return {selected.begin(), selected.end()};
      }

    private:
      controls::ListView ownerList() const
      {
        auto const rows = Owner().try_as<controls::ListView>();

        if (!rows)
        {
          winrt::throw_hresult(UIA_E_ELEMENTNOTAVAILABLE);
        }

        return rows;
      }
    };

    struct TrackListView : controls::ListViewT<TrackListView>
    {
      peers::AutomationPeer OnCreateAutomationPeer()
      {
        return winrt::make<TrackListPeer>(get_strong().as<controls::ListView>()).as<peers::AutomationPeer>();
      }
    };
  } // namespace

  controls::ListView makeTrackListView()
  {
    return winrt::make<TrackListView>().as<controls::ListView>();
  }
} // namespace ao::winui::layout
