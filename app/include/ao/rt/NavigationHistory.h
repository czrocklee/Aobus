// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "TrackPresentation.h"
#include <ao/CoreIds.h>

#include <cstddef>
#include <deque>
#include <optional>
#include <string>

namespace ao::rt
{
  struct NavigationPoint final
  {
    ListId listId{};
    std::string filterExpression{};
    TrackPresentationSpec presentation{};

    bool operator==(NavigationPoint const&) const = default;
  };

  class NavigationHistory final
  {
  public:
    static constexpr std::size_t kDefaultMaxSize = 256;

    explicit NavigationHistory(std::size_t maxSize = kDefaultMaxSize);

    bool commit(NavigationPoint point);
    std::optional<NavigationPoint> back();
    std::optional<NavigationPoint> forward();
    std::optional<NavigationPoint> current() const;

    bool canGoBack() const noexcept;
    bool canGoForward() const noexcept;
    std::size_t size() const noexcept;
    std::optional<std::size_t> currentIndex() const noexcept;

  private:
    std::deque<NavigationPoint> _points;
    std::optional<std::size_t> _optCurrentIndex;
    std::size_t _maxSize;
  };
} // namespace ao::rt
