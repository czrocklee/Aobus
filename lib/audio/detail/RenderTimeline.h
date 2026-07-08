// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <ao/audio/DecodedStreamInfo.h>
#include <ao/audio/Engine.h>
#include <ao/audio/Format.h>
#include <ao/audio/PcmSource.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <utility>

namespace ao::audio::detail
{
  struct RenderTimelineNode final
  {
    Engine::PlaybackItem item;
    std::shared_ptr<PcmSource> sourcePtr;
    Format backendFormat;
    DecodedStreamInfo info;
    std::uint64_t sourceGeneration = 0;
  };

  class RenderTimeline final
  {
  public:
    using Node = RenderTimelineNode;

    PcmSource* activeSource() const noexcept
    {
      auto* const node = _active.load(std::memory_order_acquire);
      return node != nullptr ? node->sourcePtr.get() : nullptr;
    }

    Node* activeNode() const noexcept { return _active.load(std::memory_order_acquire); }

    Node* lookaheadNode() const noexcept { return _lookahead.load(std::memory_order_acquire); }

    Node* disarmLookahead() noexcept { return _lookahead.exchange(nullptr, std::memory_order_acq_rel); }

    Node* consumeLookaheadForRender() noexcept { return _lookahead.exchange(nullptr, std::memory_order_acquire); }

    std::uint64_t activeSourceGeneration() const noexcept
    {
      auto* const node = activeNode();
      return node != nullptr ? node->sourceGeneration : 0;
    }

    // Publish `node` as the active current node. MUST be called only with the
    // backend quiesced (after backendPtr->stop()/close(), or before the first
    // start()), so destroying retired sources cannot race the RT render thread.
    void publishCurrent(std::unique_ptr<Node> nodePtr)
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

    void armLookahead(std::unique_ptr<Node> nodePtr)
    {
      auto const lock = std::scoped_lock{_mutex};
      _lookahead.store(nodePtr.get(), std::memory_order_release);
      _lookaheadNodePtr = std::move(nodePtr);
    }

    void publishActive(Node* node) noexcept { _active.store(node, std::memory_order_release); }

    void retireCursor() noexcept
    {
      _active.store(nullptr, std::memory_order_release);
      _lookahead.store(nullptr, std::memory_order_release);
    }

    void clear()
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

    void dropDisarmedLookahead(Node* node)
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

    std::unique_ptr<Node> promoteSplicedLookahead(Node* node)
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

    // Copy the active source pointer for non-RT callers (status / seek /
    // resume), keeping it alive for the duration of their use.
    std::shared_ptr<PcmSource> current() const
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

  private:
    std::atomic<Node*> _active{nullptr};
    std::atomic<Node*> _lookahead{nullptr};
    mutable std::mutex _mutex;
    std::unique_ptr<Node> _currentNodePtr;
    std::unique_ptr<Node> _lookaheadNodePtr;
  };
} // namespace ao::audio::detail
