// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "RenderTimeline.h"

#include <ao/Contract.h>
#include <ao/audio/PcmSource.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <utility>

namespace ao::audio::detail
{
  PcmSource* RenderTimeline::activeSource() const noexcept
  {
    auto* const node = _active.load(std::memory_order_acquire);
    return node != nullptr ? node->sourcePtr.get() : nullptr;
  }

  std::uint64_t RenderTimeline::activeSourceGeneration() const noexcept
  {
    auto* const node = activeNode();
    return node != nullptr ? node->sourceGeneration : 0;
  }

  void RenderTimeline::publishCurrent(std::unique_ptr<Node> nodePtr)
  {
    auto retiredCurrentPtr = std::unique_ptr<Node>{};
    auto retiredLookaheadPtr = std::unique_ptr<Node>{};
    {
      auto const lock = std::scoped_lock{_mutex};
      _lookahead.store(nullptr, std::memory_order_release);
      _active.store(nodePtr.get(), std::memory_order_release);
      retiredCurrentPtr = std::move(_currentNodePtr);
      retiredLookaheadPtr = std::move(_lookaheadNodePtr);
      _currentNodePtr = std::move(nodePtr);
    }
  }

  void RenderTimeline::armLookahead(std::unique_ptr<Node> nodePtr)
  {
    AO_EXPECTS(nodePtr != nullptr, "RenderTimeline lookahead node must be present");

    auto const lock = std::scoped_lock{_mutex};
    AO_INVARIANT(_lookahead.load(std::memory_order_acquire) == nullptr,
                 "RenderTimeline lookahead cursor must be empty before arm");
    AO_INVARIANT(_lookaheadNodePtr == nullptr, "RenderTimeline lookahead owner must be empty before arm");

    _lookaheadNodePtr = std::move(nodePtr);
    _lookahead.store(_lookaheadNodePtr.get(), std::memory_order_release);
  }

  void RenderTimeline::retireCursor() noexcept
  {
    _active.store(nullptr, std::memory_order_release);
    _lookahead.store(nullptr, std::memory_order_release);
  }

  void RenderTimeline::clear()
  {
    auto retiredCurrentPtr = std::unique_ptr<Node>{};
    auto retiredLookaheadPtr = std::unique_ptr<Node>{};
    {
      auto const lock = std::scoped_lock{_mutex};
      _active.store(nullptr, std::memory_order_release);
      _lookahead.store(nullptr, std::memory_order_release);
      retiredCurrentPtr = std::move(_currentNodePtr);
      retiredLookaheadPtr = std::move(_lookaheadNodePtr);
    }
  }

  void RenderTimeline::dropDisarmedLookahead(Node* node)
  {
    if (node == nullptr)
    {
      return;
    }

    auto retiredLookaheadPtr = std::unique_ptr<Node>{};
    {
      auto const lock = std::scoped_lock{_mutex};

      if (_lookaheadNodePtr.get() == node && _active.load(std::memory_order_acquire) != node)
      {
        retiredLookaheadPtr = std::move(_lookaheadNodePtr);
      }
    }
  }

  std::unique_ptr<RenderTimeline::Node> RenderTimeline::promoteSplicedLookahead(Node* node)
  {
    if (node == nullptr)
    {
      return {};
    }

    auto retiredCurrentPtr = std::unique_ptr<Node>{};
    {
      auto const lock = std::scoped_lock{_mutex};

      if (_lookaheadNodePtr.get() != node || _active.load(std::memory_order_acquire) != node)
      {
        return {};
      }

      retiredCurrentPtr = std::move(_currentNodePtr);
      _currentNodePtr = std::move(_lookaheadNodePtr);
      _lookahead.store(nullptr, std::memory_order_release);
    }

    return retiredCurrentPtr;
  }

  std::shared_ptr<PcmSource> RenderTimeline::current() const
  {
    auto const lock = std::scoped_lock{_mutex};
    auto* const node = _active.load(std::memory_order_acquire);

    if (node == _currentNodePtr.get())
    {
      return _currentNodePtr ? _currentNodePtr->sourcePtr : nullptr;
    }

    if (node == _lookaheadNodePtr.get())
    {
      return _lookaheadNodePtr ? _lookaheadNodePtr->sourcePtr : nullptr;
    }

    return {};
  }
} // namespace ao::audio::detail
