// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

// Console probe for the native lazy track item view. The first-party
// Aobus.TrackRowItem projection is generated only by the WinUI build, so this
// is the one place the native view contract can meet real rows: lazy size,
// bounded GetAt, null-result retry, the native least-recently-used cache,
// display-index lookup, GetMany, iterator exhaustion, and the iterator's
// strong view reference. Expected contract failures are reported and counted
// with a nonzero exit status; the probe never aborts on them.

#include "track/TrackItemView.h"
#include "track/TrackRowItem.h"

#include <windows.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/base.h>

#include <cstddef>
#include <cstdint>
#include <exception>
#include <format>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace
{
  using Item = winrt::Windows::Foundation::IInspectable;
  using ItemIterator = winrt::Windows::Foundation::Collections::IIterator<Item>;
  using ItemView = winrt::Windows::Foundation::Collections::IVectorView<Item>;
  using ProjectedTrackRow = winrt::Aobus::TrackRowItem;

  constexpr std::uint32_t kFirstTrackId = 9000;

  /// Scenario-scoped failure collection: expected contract violations are
  /// reported and counted instead of aborting the probe run.
  class ScenarioReport final
  {
  public:
    void beginScenario(std::string_view name)
    {
      _scenario = name;
      std::cerr << std::format("WinUI track item probe: scenario '{}'\n", name);
    }

    void check(bool const condition, std::string_view observation)
    {
      if (!condition)
      {
        record(observation);
      }
    }

    template<typename Action>
    void expectHResult(winrt::hresult const expected, std::string_view observation, Action&& action)
    {
      try
      {
        std::forward<Action>(action)();
        record(std::format("{}: expected HRESULT {:#x} but nothing was thrown", observation, expected.value));
      }
      catch (winrt::hresult_error const& error)
      {
        if (error.code().value != expected.value)
        {
          record(std::format("{}: expected HRESULT {:#x} but saw {:#x} ({})",
                             observation,
                             expected.value,
                             error.code().value,
                             winrt::to_string(error.message())));
        }
      }
      catch (...)
      {
        record(
          std::format("{}: expected HRESULT {:#x} but a non-WinRT exception was thrown", observation, expected.value));
      }
    }

    void record(std::string_view detail)
    {
      std::cerr << std::format("  FAIL [{}]: {}\n", _scenario, detail);
      ++_failureCount;
    }

    std::size_t failureCount() const noexcept { return _failureCount; }

  private:
    std::string_view _scenario;
    std::size_t _failureCount = 0;
  };

  Item makeRow(std::size_t displayIndex);

  /// The view owns a provider copy, while the scenario observes the same calls.
  class RecordingRowProvider final
  {
  public:
    /// The next @p failures requests for @p displayIndex return null.
    void failNext(std::size_t displayIndex, std::size_t failures) { _statePtr->pendingNulls[displayIndex] += failures; }

    std::size_t callsOf(std::size_t displayIndex) const
    {
      if (auto const found = _statePtr->calls.find(displayIndex); found != _statePtr->calls.end())
      {
        return found->second;
      }

      return std::size_t{0};
    }

    std::size_t totalCalls() const noexcept { return _statePtr->totalCalls; }

    Item operator()(std::size_t displayIndex)
    {
      ++_statePtr->totalCalls;
      ++_statePtr->calls[displayIndex];

      if (auto const pending = _statePtr->pendingNulls.find(displayIndex);
          pending != _statePtr->pendingNulls.end() && pending->second > 0)
      {
        --pending->second;
        return nullptr;
      }

      return makeRow(displayIndex);
    }

  private:
    struct State final
    {
      std::map<std::size_t, std::size_t> calls;
      std::map<std::size_t, std::size_t> pendingNulls;
      std::size_t totalCalls = 0;
    };

    std::shared_ptr<State> _statePtr{std::make_shared<State>()};
  };

  Item makeRow(std::size_t displayIndex)
  {
    return winrt::make<winrt::Aobus::implementation::TrackRowItem>(
      static_cast<std::uint32_t>(displayIndex),
      kFirstTrackId + static_cast<std::uint32_t>(displayIndex),
      0U,
      winrt::hstring{std::format(L"Probe title {}", displayIndex)},
      winrt::hstring{L"Probe artist"},
      winrt::hstring{L"Probe album"},
      winrt::hstring{L"3:21"});
  }

  bool isSameItem(Item const& lhs, Item const& rhs)
  {
    if (!lhs || !rhs)
    {
      return false;
    }

    return winrt::get_unknown(lhs) == winrt::get_unknown(rhs);
  }

  /// The materialized item must be a real first-party projected track row.
  void checkRowProjection(ScenarioReport& report, Item const& item, std::size_t expectedDisplayIndex)
  {
    auto const row = item.try_as<ProjectedTrackRow>();

    if (!row)
    {
      report.record("a materialized row must project to Aobus.TrackRowItem");
      return;
    }

    report.check(row.DisplayIndex() == expectedDisplayIndex,
                 std::format("expected DisplayIndex {} but saw {}", expectedDisplayIndex, row.DisplayIndex()));

    auto const expectedTrackId = kFirstTrackId + static_cast<std::uint32_t>(expectedDisplayIndex);
    report.check(
      row.TrackId() == expectedTrackId, std::format("expected TrackId {} but saw {}", expectedTrackId, row.TrackId()));
    report.check(!row.IsGroupHeader(), "a materialized track row is not a group header");
  }

  void scenarioLazySizeAndBoundedGetAt(ScenarioReport& report)
  {
    report.beginScenario("lazy size and bounded GetAt");

    auto provider = RecordingRowProvider{};
    auto const view = ao::winui::makeTrackItemView(8, provider, 4);

    report.check(view.Size() == 8, std::format("Size() was {} before any materialization", view.Size()));
    report.check(provider.totalCalls() == 0, "Size() must not materialize rows");

    report.expectHResult(
      winrt::hresult{E_BOUNDS}, "GetAt(size) must throw E_BOUNDS", [&view] { std::ignore = view.GetAt(8); });
    report.expectHResult(winrt::hresult{E_BOUNDS},
                         "GetAt beyond the size must throw E_BOUNDS",
                         [&view] { std::ignore = view.GetAt(100); });
    report.check(provider.totalCalls() == 0, "an out-of-range GetAt must not invoke the provider");

    auto const row = view.GetAt(0);
    report.check(provider.totalCalls() == 1, "the first in-range GetAt materializes exactly one row");
    checkRowProjection(report, row, 0);

    auto const rowAgain = view.GetAt(0);
    report.check(isSameItem(row, rowAgain), "a repeated GetAt returns the cached row instance");
    report.check(provider.callsOf(0) == 1, "a repeated GetAt does not re-materialize the row");

    auto const emptyProvider = RecordingRowProvider{};
    auto const emptyView = ao::winui::makeTrackItemView(0, emptyProvider, 4);

    report.check(emptyView.Size() == 0, "an empty view reports a zero size");
    report.expectHResult(winrt::hresult{E_BOUNDS},
                         "GetAt(0) on an empty view must throw E_BOUNDS",
                         [&emptyView] { std::ignore = emptyView.GetAt(0); });
    report.check(emptyProvider.totalCalls() == 0, "an empty view never invokes the provider");
    report.check(!emptyView.First().HasCurrent(), "an empty view iterates nothing");
  }

  void scenarioNullProviderRetry(ScenarioReport& report)
  {
    report.beginScenario("null provider results throw and are retried");

    auto provider = RecordingRowProvider{};
    provider.failNext(1, 1);
    auto const view = ao::winui::makeTrackItemView(4, provider, 4);

    report.expectHResult(winrt::hresult{E_BOUNDS},
                         "a null provider result must surface as E_BOUNDS",
                         [&view] { std::ignore = view.GetAt(1); });
    report.check(provider.callsOf(1) == 1, "the failed materialization consumed one provider call");

    auto const retried = view.GetAt(1);
    report.check(provider.callsOf(1) == 2, "the retry re-invokes the provider for the same index");
    checkRowProjection(report, retried, 1);

    auto const retriedAgain = view.GetAt(1);
    report.check(isSameItem(retried, retriedAgain), "a succeeded retry is cached like any other row");
    report.check(provider.callsOf(1) == 2, "the null result was not cached: success added no call");
    report.check(
      provider.callsOf(0) == 0 && provider.callsOf(2) == 0, "the failed materialization did not touch other indexes");
  }

  void scenarioNativeLruEvictionAndPromotion(ScenarioReport& report)
  {
    report.beginScenario("native LRU evicts and promotes at a bound of 2");

    auto provider = RecordingRowProvider{};
    auto const view = ao::winui::makeTrackItemView(4, provider, 2);

    auto const first = view.GetAt(0);
    auto const second = view.GetAt(1);
    report.check(provider.totalCalls() == 2, "two distinct rows materialize two rows");

    auto const third = view.GetAt(2);
    report.check(isSameItem(third, view.GetAt(2)), "the newest row survives the eviction it caused");
    report.check(provider.callsOf(2) == 1, "the surviving row was not re-materialized");

    auto const firstAgain = view.GetAt(0);
    report.check(provider.callsOf(0) == 2, "the least recently used row was evicted and re-materialized");
    report.check(!isSameItem(first, firstAgain), "an evicted row comes back as a fresh instance");

    // Touching a cached row promotes it, so the next miss evicts the other resident row.
    report.check(isSameItem(third, view.GetAt(2)), "a hit returns the cached instance");
    auto const secondAgain = view.GetAt(1);
    report.check(provider.callsOf(1) == 2, "the row demoted by the promotion was the eviction victim");
    report.check(!isSameItem(second, secondAgain), "the demoted row comes back as a fresh instance");
    report.check(isSameItem(third, view.GetAt(2)), "the promoted row survived the eviction");
    report.check(provider.callsOf(2) == 1, "the promoted row was never re-materialized");
    report.check(provider.totalCalls() == 5, "hits and evictions never call the provider");
  }

  void scenarioIndexOfRealDisplayIndexes(ScenarioReport& report)
  {
    report.beginScenario("IndexOf resolves real display indexes");

    auto provider = RecordingRowProvider{};
    auto const view = ao::winui::makeTrackItemView(10, provider, 4);

    auto const row = view.GetAt(5);
    checkRowProjection(report, row, 5);

    std::uint32_t foundAt = 99;
    report.check(
      view.IndexOf(row, foundAt) && foundAt == 5, "IndexOf finds a real materialized row at its display index");

    // IndexOf answers from the row's own projected display index, not from
    // membership: a first-party row from another view still resolves here.
    auto otherProvider = RecordingRowProvider{};
    auto const otherView = ao::winui::makeTrackItemView(10, otherProvider, 4);
    auto const borrowedRow = otherView.GetAt(3);
    foundAt = 99;
    report.check(view.IndexOf(borrowedRow, foundAt) && foundAt == 3,
                 "IndexOf follows the projected display index, not view membership");

    foundAt = 99;
    report.check(!view.IndexOf(makeRow(10), foundAt), "a display index at the size bound is not found");
    foundAt = 99;
    report.check(!view.IndexOf(makeRow(50), foundAt), "a display index beyond the size bound is not found");

    foundAt = 99;
    report.check(!view.IndexOf(winrt::box_value(L"not a track row"), foundAt), "a non-row object is not found");
    report.check(foundAt == 0, "a failed lookup reports the zero index");
  }

  void scenarioGetMany(ScenarioReport& report)
  {
    report.beginScenario("GetMany copies lazily and partially");

    auto provider = RecordingRowProvider{};
    auto const view = ao::winui::makeTrackItemView(6, provider, 8);

    auto firstFour = std::vector<Item>(4, nullptr);
    report.check(view.GetMany(0, winrt::array_view<Item>(firstFour)) == 4,
                 "a request within the size copies every requested slot");
    report.check(provider.totalCalls() == 4, "each copied slot materialized exactly one row");

    for (std::size_t index = 0; index < 4; ++index)
    {
      report.check(isSameItem(firstFour[index], view.GetAt(static_cast<std::uint32_t>(index))),
                   std::format("the copied slot {} shares the row cache with GetAt", index));
    }

    auto tail = std::vector<Item>(4, nullptr);
    report.check(view.GetMany(4, winrt::array_view<Item>(tail)) == 2,
                 "a request past the last row copies only the remaining rows");
    report.check(isSameItem(tail[0], view.GetAt(4)) && isSameItem(tail[1], view.GetAt(5)),
                 "the partial tail rows are the same cached instances");

    auto buffer = std::vector<Item>(2, nullptr);
    report.check(view.GetMany(6, winrt::array_view<Item>(buffer)) == 0, "a start at the size bound copies nothing");
    report.check(view.GetMany(9, winrt::array_view<Item>(buffer)) == 0, "a start beyond the size bound copies nothing");

    auto empty = std::vector<Item>{};
    report.check(view.GetMany(0, winrt::array_view<Item>(empty)) == 0, "an empty destination copies nothing");
    report.check(provider.totalCalls() == 6, "tail, out-of-range, and empty requests materialize only real rows");
  }

  void scenarioIterator(ScenarioReport& report)
  {
    report.beginScenario("the iterator walks rows and outlives the view handle");

    auto provider = RecordingRowProvider{};
    auto const view = ao::winui::makeTrackItemView(5, provider, 8);

    auto iterator = view.First();
    report.check(iterator.HasCurrent(), "a fresh iterator has a current row");
    report.check(isSameItem(iterator.Current(), view.GetAt(0)), "the iterator starts at display index 0");

    report.check(iterator.MoveNext(), "MoveNext advances");
    report.check(isSameItem(iterator.Current(), view.GetAt(1)), "the advanced current row is display index 1");

    auto step = std::vector<Item>(2, nullptr);
    report.check(iterator.GetMany(winrt::array_view<Item>(step)) == 2, "the iterator copies the next rows on demand");
    report.check(isSameItem(step[0], view.GetAt(1)) && isSameItem(step[1], view.GetAt(2)),
                 "the iterator copy starts at the iterator's own position");
    report.check(isSameItem(iterator.Current(), view.GetAt(3)), "current follows the iterator copy");

    report.check(iterator.MoveNext(), "MoveNext reaches the last row");
    report.check(isSameItem(iterator.Current(), view.GetAt(4)), "the last current row is the final display index");
    report.check(!iterator.MoveNext(), "MoveNext reports exhaustion at the end");
    report.check(!iterator.HasCurrent(), "an exhausted iterator has no current row");
    report.expectHResult(winrt::hresult{E_BOUNDS},
                         "Current() on an exhausted iterator must throw E_BOUNDS",
                         [&iterator] { std::ignore = iterator.Current(); });
    report.check(!iterator.MoveNext(), "a further MoveNext stays exhausted");

    // The iterator holds the native view strongly, so releasing the probe's
    // own view handle cannot invalidate a walk that is still in progress.
    auto survivingIterator = ItemIterator{nullptr};
    {
      auto scopedProvider = RecordingRowProvider{};
      auto const scopedView = ao::winui::makeTrackItemView(3, scopedProvider, 4);
      survivingIterator = scopedView.First();
      report.check(survivingIterator.HasCurrent(), "the scoped iterator starts before the view handle is released");
    }

    std::size_t visited = 0;

    while (survivingIterator.HasCurrent())
    {
      checkRowProjection(report, survivingIterator.Current(), visited);
      ++visited;
      std::ignore = survivingIterator.MoveNext();
    }

    report.check(visited == 3, "the iterator traverses every row after the view handle is gone");
  }

  void scenarioOversizedSizeRejected(ScenarioReport& report)
  {
    report.beginScenario("an oversized view size is rejected");

    auto provider = RecordingRowProvider{};
    auto const oversized = static_cast<std::size_t>(std::uint64_t{std::numeric_limits<std::uint32_t>::max()} + 1);
    report.expectHResult(winrt::hresult{E_INVALIDARG},
                         "a size above the 32-bit index domain must throw E_INVALIDARG",
                         [&provider] { std::ignore = ao::winui::makeTrackItemView(oversized, provider, 2); });
    report.check(provider.totalCalls() == 0, "the rejected view never invokes the provider");

    auto const maximal = ao::winui::makeTrackItemView(std::numeric_limits<std::uint32_t>::max(), provider, 2);
    report.check(
      maximal.Size() == std::numeric_limits<std::uint32_t>::max(), "the exact 32-bit size bound is accepted");
  }

  int run()
  {
    auto report = ScenarioReport{};

    scenarioLazySizeAndBoundedGetAt(report);
    scenarioNullProviderRetry(report);
    scenarioNativeLruEvictionAndPromotion(report);
    scenarioIndexOfRealDisplayIndexes(report);
    scenarioGetMany(report);
    scenarioIterator(report);
    scenarioOversizedSizeRejected(report);

    if (report.failureCount() == 0)
    {
      std::cerr << "WinUI track item probe: all scenarios passed\n";
      return 0;
    }

    std::cerr << std::format("WinUI track item probe: {} check(s) failed\n", report.failureCount());
    return 1;
  }
} // namespace

int main()
{
  try
  {
    winrt::init_apartment();
    return run();
  }
  catch (winrt::hresult_error const& error)
  {
    std::cerr << std::format("WinUI track item probe startup failed: {} (HRESULT {:#x})\n",
                             winrt::to_string(error.message()),
                             static_cast<std::uint32_t>(error.code().value));
    return 2;
  }
  catch (std::exception const& error)
  {
    std::cerr << std::format("WinUI track item probe failed: {}\n", error.what());
    return 3;
  }
}
