// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "layout/component/track/TrackListSelectionEvents.h"

#include <ao/uimodel/layout/shell/ShellGenerationSequence.h>
#include <ao/winui/WinUiErrorBoundary.h>
#include <ao/winui/track/TrackSelectionNotifications.h>

#include <winrt/Microsoft.UI.Xaml.Automation.Peers.h>
#include <winrt/Windows.Foundation.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace ao::winui::layout
{
  namespace
  {
    namespace peers = winrt::Microsoft::UI::Xaml::Automation::Peers;
    using peers::AutomationEvents;
    using peers::AutomationPeer;
    using winrt::Windows::Foundation::IInspectable;
    using Items = winrt::Windows::Foundation::Collections::IVectorView<IInspectable>;
    using winrt::Microsoft::UI::Xaml::Controls::ListView;
    using Ranges = TrackListSelectionEvents::Ranges;

    // A new planner event needs a UIA mapping, even on builds that allow warnings.
#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic error "-Wswitch"
#pragma clang diagnostic error "-Wswitch-enum"
#elifdef _MSC_VER
#pragma warning(push)
#pragma warning(error : 4061 4062)
#endif
    std::optional<AutomationEvents> nativeEvent(TrackSelectionEventKind const kind)
    {
      switch (kind)
      {
        case TrackSelectionEventKind::Selected: return AutomationEvents::SelectionItemPatternOnElementSelected;
        case TrackSelectionEventKind::Added: return AutomationEvents::SelectionItemPatternOnElementAddedToSelection;
        case TrackSelectionEventKind::Removed:
          return AutomationEvents::SelectionItemPatternOnElementRemovedFromSelection;
        case TrackSelectionEventKind::Invalidated: return AutomationEvents::SelectionPatternOnInvalidated;
      }

      logWinUiCritical("Track list selection notification", "Unknown TrackSelectionEventKind; skipping this UIA event");
      return std::nullopt;
    }
#ifdef __clang__
#pragma clang diagnostic pop
#elifdef _MSC_VER
#pragma warning(pop)
#endif

    std::vector<TrackSelectionSpan> spans(Ranges const& ranges)
    {
      auto result = std::vector<TrackSelectionSpan>{};
      result.reserve(ranges.Size());

      for (auto const& range : ranges)
      {
        result.push_back({.first = static_cast<std::uint32_t>(range.FirstIndex()), .length = range.Length()});
      }

      return result;
    }

    std::vector<TrackSelectionEvent> committedEvents(bool const sameSource,
                                                     Ranges const& previous,
                                                     Ranges const& ranges)
    {
      if (sameSource)
      {
        return trackSelectionNotifications(spans(previous), spans(ranges));
      }

      if (previous.Size() != 0 || ranges.Size() != 0)
      {
        // Even retained track IDs may have different data peers after a reorder or filter.
        return {{.kind = TrackSelectionEventKind::Invalidated, .index = 0}};
      }

      return {};
    }

    bool hasSelectionListener()
    {
      return AutomationPeer::ListenerExists(AutomationEvents::SelectionItemPatternOnElementSelected) ||
             AutomationPeer::ListenerExists(AutomationEvents::SelectionItemPatternOnElementAddedToSelection) ||
             AutomationPeer::ListenerExists(AutomationEvents::SelectionItemPatternOnElementRemovedFromSelection) ||
             AutomationPeer::ListenerExists(AutomationEvents::SelectionPatternOnInvalidated);
    }
  } // namespace

  bool TrackListSelectionEvents::Revision::isCurrent() const noexcept
  {
    auto const livePtr = counterPtr.lock();
    return livePtr && *livePtr == serial;
  }

  void TrackListSelectionEvents::commit(ListView const& rows,
                                        Ranges const& ranges,
                                        std::weak_ptr<uimodel::ShellGenerationGate> const& gatePtr)
  {
    auto const source = rows.ItemsSource();
    auto const previousSource = std::exchange(_source, source);
    auto const previous = std::exchange(_ranges, ranges);
    interrupt();

    if (!previous || !ranges || !rows.IsLoaded() || !uimodel::isGenerationActive(gatePtr))
    {
      return;
    }

    // Capture by value: raising a native event must not keep a retired component alive.
    runOptionalWinRt(
      "track list selection event",
      [rows, source, previousSource, previous, ranges, gatePtr, observed = revision()]
      {
        if (!hasSelectionListener())
        {
          return;
        }

        auto const current = [&]
        { return observed.isCurrent() && uimodel::isGenerationActive(gatePtr) && rows.ItemsSource() == source; };
        peers::ItemsControlAutomationPeer listPeer{nullptr};

        for (auto const& event : committedEvents(source == previousSource, previous, ranges))
        {
          auto const optId = nativeEvent(event.kind);

          if (!optId || !AutomationPeer::ListenerExists(*optId))
          {
            continue;
          }

          if (!current())
          {
            return;
          }

          if (!listPeer)
          {
            listPeer =
              peers::FrameworkElementAutomationPeer::CreatePeerForElement(rows).as<peers::ItemsControlAutomationPeer>();

            if (!listPeer || !current())
            {
              return;
            }
          }

          AutomationPeer sender = listPeer;

          if (event.kind != TrackSelectionEventKind::Invalidated)
          {
            sender = listPeer.CreateItemAutomationPeer(source.as<Items>().GetAt(event.index));
          }

          if (sender && current())
          {
            sender.RaiseAutomationEvent(*optId);
          }
        }
      });
  }
} // namespace ao::winui::layout
