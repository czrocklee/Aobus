// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "track/TrackItemView.h"

#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <list>
#include <unordered_map>
#include <utility>

namespace ao::winui
{
  namespace
  {
    using Item = winrt::Windows::Foundation::IInspectable;
    using View = winrt::Windows::Foundation::Collections::IVectorView<Item>;

    class TrackItemIterator;

    class TrackItemView
      : public winrt::implements<TrackItemView, View, winrt::Windows::Foundation::Collections::IIterable<Item>>
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

    private:
      using Recency = std::list<std::uint32_t>;

      struct Entry final
      {
        Item item{nullptr};
        Recency::iterator recency;
      };

      void evictLeastRecentlyUsed();

      std::uint32_t _size = 0;
      TrackItemProvider _provider;
      std::size_t _maximumEntries = 1;
      Recency _recency;
      std::unordered_map<std::uint32_t, Entry> _entries;
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
        if (auto const candidate = row.DisplayIndex(); candidate < _size)
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
