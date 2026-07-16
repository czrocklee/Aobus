// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/rt/NavigationHistory.h>

#include <algorithm>
#include <cstddef>
#include <optional>
#include <utility>

namespace ao::rt
{
  NavigationHistory::NavigationHistory(std::size_t const maxSize)
    : _maxSize{std::max<std::size_t>(1, maxSize)}
  {
  }

  bool NavigationHistory::commit(NavigationPoint point)
  {
    if (_optCurrentIndex && _points[*_optCurrentIndex] == point)
    {
      return false;
    }

    if (_optCurrentIndex)
    {
      _points.erase(_points.begin() + static_cast<std::ptrdiff_t>(*_optCurrentIndex) + 1, _points.end());
    }

    _points.push_back(std::move(point));
    _optCurrentIndex = _points.size() - 1;

    while (_points.size() > _maxSize)
    {
      _points.pop_front();
      *_optCurrentIndex -= 1;
    }

    return true;
  }

  std::optional<NavigationPoint> NavigationHistory::back()
  {
    if (!_optCurrentIndex || *_optCurrentIndex == 0)
    {
      return std::nullopt;
    }

    *_optCurrentIndex -= 1;
    return _points[*_optCurrentIndex];
  }

  std::optional<NavigationPoint> NavigationHistory::forward()
  {
    if (!_optCurrentIndex || *_optCurrentIndex >= _points.size() - 1)
    {
      return std::nullopt;
    }

    *_optCurrentIndex += 1;
    return _points[*_optCurrentIndex];
  }

  std::optional<NavigationPoint> NavigationHistory::current() const
  {
    if (!_optCurrentIndex)
    {
      return std::nullopt;
    }

    return _points[*_optCurrentIndex];
  }

  bool NavigationHistory::canGoBack() const noexcept
  {
    return _optCurrentIndex && *_optCurrentIndex > 0;
  }

  bool NavigationHistory::canGoForward() const noexcept
  {
    return _optCurrentIndex && *_optCurrentIndex < _points.size() - 1;
  }

  std::size_t NavigationHistory::size() const noexcept
  {
    return _points.size();
  }

  std::optional<std::size_t> NavigationHistory::currentIndex() const noexcept
  {
    return _optCurrentIndex;
  }
} // namespace ao::rt
