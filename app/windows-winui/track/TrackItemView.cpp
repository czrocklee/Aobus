// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "track/TrackItemView.h"

#include "track/TrackRowItem.h"

#include <winerror.h>
#include <winrt/Aobus.h>
#include <winrt/Microsoft.UI.Xaml.Data.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <limits>
#include <list>
#include <map>
#include <memory>
#include <optional>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

namespace ao::winui
{
  struct TrackItemSourceIdentity final
  {};

  namespace
  {
    using winrt::Microsoft::UI::Xaml::Data::ISelectionInfo;
    using winrt::Microsoft::UI::Xaml::Data::ItemIndexRange;
    using Item = winrt::Windows::Foundation::IInspectable;
    using Iterable = winrt::Windows::Foundation::Collections::IIterable<Item>;
    using View = winrt::Windows::Foundation::Collections::IVectorView<Item>;
    using Vector = winrt::Windows::Foundation::Collections::IVector<Item>;
    using RangeView = winrt::Windows::Foundation::Collections::IVectorView<ItemIndexRange>;

    class TrackItemIterator;

    // ListView consumes IVector by index. With only IVectorView/IIterable it
    // copies every row through the iterator before realizing the first screen.
    // ISelectionInfo also lets it update selection without retrieving rows.
    class TrackItemView : public winrt::implements<TrackItemView, View, Vector, Iterable, ISelectionInfo>
    {
    public:
      TrackItemView(std::uint32_t const size, TrackItemProvider provider, std::size_t const maximumEntries)
        : _size{size}, _provider{std::move(provider)}, _maximumEntries{std::max<std::size_t>(1, maximumEntries)}
      {
        _entries.reserve(_maximumEntries);
      }

      Item GetAt(std::uint32_t index);
      std::uint32_t Size() const noexcept { return _size; }
      bool IndexOf(Item const& value, std::uint32_t& index) const noexcept;
      std::uint32_t GetMany(std::uint32_t startIndex, winrt::array_view<Item> values);
      winrt::Windows::Foundation::Collections::IIterator<Item> First();

      // C++/WinRT's IVector and ISelectionInfo projections require these names.
      // NOLINTBEGIN(readability-identifier-naming)
      View GetView() { return get_strong().as<View>(); }

      void SelectRange(ItemIndexRange const& range);
      void DeselectRange(ItemIndexRange const& range);
      bool IsSelected(std::int32_t index) const noexcept;
      RangeView GetSelectedRanges() const;

      // The projection owns membership and order. XAML gets indexed reads, not edits.
      void SetAt(std::uint32_t /*index*/, Item const& /*value*/) { winrt::throw_hresult(E_ACCESSDENIED); }
      void InsertAt(std::uint32_t /*index*/, Item const& /*value*/) { winrt::throw_hresult(E_ACCESSDENIED); }
      void RemoveAt(std::uint32_t /*index*/) { winrt::throw_hresult(E_ACCESSDENIED); }
      void Append(Item const& /*value*/) { winrt::throw_hresult(E_ACCESSDENIED); }
      void RemoveAtEnd() { winrt::throw_hresult(E_ACCESSDENIED); }
      void Clear() { winrt::throw_hresult(E_ACCESSDENIED); }
      void ReplaceAll(winrt::array_view<Item const> /*values*/) { winrt::throw_hresult(E_ACCESSDENIED); }
      // NOLINTEND(readability-identifier-naming)

    private:
      using Recency = std::list<std::uint32_t>;

      struct Entry final
      {
        Item item{nullptr};
        Recency::iterator recency;
      };

      struct SelectionBounds final
      {
        std::uint32_t first;
        std::uint32_t end;
      };

      std::optional<SelectionBounds> selectionBounds(ItemIndexRange const& range) const;
      void evictLeastRecentlyUsed();

      std::uint32_t _size = 0;
      TrackItemProvider _provider;
      std::size_t _maximumEntries = 1;
      std::shared_ptr<TrackItemSourceIdentity const> _identityPtr = std::make_shared<TrackItemSourceIdentity>();
      Recency _recency;
      std::unordered_map<std::uint32_t, Entry> _entries;
      // Half-open intervals keyed by first index, with no overlap or adjacency.
      std::map<std::uint32_t, std::uint32_t> _selectedRanges;
    };

    class TrackItemIterator
      : public winrt::implements<TrackItemIterator, winrt::Windows::Foundation::Collections::IIterator<Item>>
    {
    public:
      explicit TrackItemIterator(winrt::com_ptr<TrackItemView> view)
        : _view{std::move(view)}
      {
      }

      Item Current() const
      {
        if (!HasCurrent())
        {
          winrt::throw_hresult(E_BOUNDS);
        }

        return _view->GetAt(_index);
      }

      bool HasCurrent() const noexcept { return _view && _index < _view->Size(); }

      bool MoveNext() noexcept
      {
        if (HasCurrent())
        {
          ++_index;
        }

        return HasCurrent();
      }

      std::uint32_t GetMany(winrt::array_view<Item> values)
      {
        auto const copied = _view->GetMany(_index, values);
        _index += copied;
        return copied;
      }

    private:
      winrt::com_ptr<TrackItemView> _view;
      std::uint32_t _index = 0;
    };

    Item TrackItemView::GetAt(std::uint32_t const index)
    {
      if (index >= _size || !_provider)
      {
        winrt::throw_hresult(E_BOUNDS);
      }

      if (auto const found = _entries.find(index); found != _entries.end())
      {
        _recency.splice(_recency.begin(), _recency, found->second.recency);
        return found->second.item;
      }

      auto item = _provider(index);

      if (!item)
      {
        winrt::throw_hresult(E_BOUNDS);
      }

      if (auto const row = item.try_as<winrt::Aobus::TrackRowItem>(); row)
      {
        winrt::get_self<winrt::Aobus::implementation::TrackRowItem>(row)->bindSource(_identityPtr);
      }

      if (_entries.size() == _maximumEntries)
      {
        evictLeastRecentlyUsed();
      }

      _recency.push_front(index);

      try
      {
        auto const [inserted, wasInserted] = _entries.emplace(index, Entry{.item = item, .recency = _recency.begin()});

        if (!wasInserted)
        {
          _recency.pop_front();
        }

        return inserted->second.item;
      }
      catch (...)
      {
        _recency.pop_front();
        throw;
      }
    }

    bool TrackItemView::IndexOf(Item const& value, std::uint32_t& index) const noexcept
    {
      if (auto const row = value.try_as<winrt::Aobus::TrackRowItem>(); row)
      {
        if (auto const candidate = row.DisplayIndex();
            candidate < _size &&
            winrt::get_self<winrt::Aobus::implementation::TrackRowItem>(row)->hasSource(*_identityPtr))
        {
          index = candidate;
          return true;
        }
      }

      index = 0;
      return false;
    }

    std::uint32_t TrackItemView::GetMany(std::uint32_t const startIndex, winrt::array_view<Item> values)
    {
      if (startIndex >= _size)
      {
        return 0;
      }

      std::uint32_t copied = 0;

      while (copied < values.size() && startIndex + copied < _size)
      {
        values[copied] = GetAt(startIndex + copied);
        ++copied;
      }

      return copied;
    }

    winrt::Windows::Foundation::Collections::IIterator<Item> TrackItemView::First()
    {
      return winrt::make<TrackItemIterator>(get_strong());
    }

    void TrackItemView::SelectRange(ItemIndexRange const& range)
    {
      auto const optBounds = selectionBounds(range);

      if (!optBounds)
      {
        return;
      }

      auto [first, end] = *optBounds;
      auto hint = _selectedRanges.upper_bound(first);

      if (hint != _selectedRanges.begin())
      {
        if (auto const previous = std::prev(hint); previous->second >= first)
        {
          first = previous->first;
          hint = previous;
        }
      }

      auto const merged = _selectedRanges.try_emplace(hint, first, end);
      merged->second = std::max(merged->second, end);
      auto next = std::next(merged);

      while (next != _selectedRanges.end() && next->first <= merged->second)
      {
        merged->second = std::max(merged->second, next->second);
        next = _selectedRanges.erase(next);
      }
    }

    void TrackItemView::DeselectRange(ItemIndexRange const& range)
    {
      auto const optBounds = selectionBounds(range);

      if (!optBounds)
      {
        return;
      }

      auto const [first, end] = *optBounds;
      auto next = _selectedRanges.lower_bound(first);

      if (next != _selectedRanges.begin())
      {
        if (auto const previous = std::prev(next); previous->second > first)
        {
          if (previous->second > end)
          {
            // Both ends of this interval survive. Allocate the right side before trimming the left.
            std::ignore = _selectedRanges.emplace_hint(next, end, previous->second);
            previous->second = first;
            return;
          }

          previous->second = first;
        }
      }

      while (next != _selectedRanges.end() && next->first < end)
      {
        if (next->second > end)
        {
          // Trim the left side without allocating a new node after earlier removals.
          auto const hint = std::next(next);
          auto tail = _selectedRanges.extract(next);
          tail.key() = end;
          std::ignore = _selectedRanges.insert(hint, std::move(tail));
          return;
        }

        next = _selectedRanges.erase(next);
      }
    }

    bool TrackItemView::IsSelected(std::int32_t const index) const noexcept
    {
      if (index < 0)
      {
        return false;
      }

      auto const position = static_cast<std::uint32_t>(index);
      auto const next = _selectedRanges.upper_bound(position);
      return next != _selectedRanges.begin() && position < std::prev(next)->second;
    }

    RangeView TrackItemView::GetSelectedRanges() const
    {
      // ListView can keep this snapshot while applying edits to the live selection.
      auto snapshot = std::vector<ItemIndexRange>{};
      snapshot.reserve(_selectedRanges.size());

      for (auto const& [first, end] : _selectedRanges)
      {
        snapshot.emplace_back(static_cast<std::int32_t>(first), end - first);
      }

      return winrt::single_threaded_vector<ItemIndexRange>(std::move(snapshot)).GetView();
    }

    std::optional<TrackItemView::SelectionBounds> TrackItemView::selectionBounds(ItemIndexRange const& range) const
    {
      if (!range)
      {
        return std::nullopt;
      }

      auto const index = range.FirstIndex();

      if (index < 0)
      {
        return std::nullopt;
      }

      // ItemIndexRange has a signed first and last index, unlike the indexed vector.
      constexpr std::uint32_t kSelectionEnd = std::uint32_t{std::numeric_limits<std::int32_t>::max()} + 1;
      auto const first = static_cast<std::uint32_t>(index);
      auto const limit = std::min(_size, kSelectionEnd);

      if (first >= limit)
      {
        return std::nullopt;
      }

      auto const length = std::min(range.Length(), limit - first);

      if (length == 0)
      {
        return std::nullopt;
      }

      return SelectionBounds{.first = first, .end = first + length};
    }

    void TrackItemView::evictLeastRecentlyUsed()
    {
      if (_recency.empty())
      {
        return;
      }

      auto const oldest = _recency.back();
      _recency.pop_back();
      _entries.erase(oldest);
    }
  } // namespace

  View makeTrackItemView(std::size_t const size, TrackItemProvider provider, std::size_t const maximumEntries)
  {
    if (size > std::numeric_limits<std::uint32_t>::max())
    {
      winrt::throw_hresult(E_INVALIDARG);
    }

    return winrt::make<TrackItemView>(static_cast<std::uint32_t>(size), std::move(provider), maximumEntries);
  }
} // namespace ao::winui
