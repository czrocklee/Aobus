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

namespace ao::audio::detail
{
  struct RenderTimelineNode final
  {
    Engine::PlaybackItem item;
    std::shared_ptr<PcmSource> sourcePtr;
    Format backendFormat;
    DecodedStreamInfo info;
    std::uint64_t sourceGeneration = 0;
    std::uint64_t playbackGeneration = 0;
  };

  class RenderTimeline final
  {
  public:
    using Node = RenderTimelineNode;

    PcmSource* activeSource() const noexcept;

    Node* activeNode() const noexcept { return _active.load(std::memory_order_acquire); }

    Node* lookaheadNode() const noexcept { return _lookahead.load(std::memory_order_acquire); }

    Node* disarmLookahead() noexcept { return _lookahead.exchange(nullptr, std::memory_order_acq_rel); }

    Node* consumeLookaheadForRender() noexcept { return _lookahead.exchange(nullptr, std::memory_order_acquire); }

    std::uint64_t activeSourceGeneration() const noexcept;

    // Publish `node` as the active current node. MUST be called only with the
    // backend quiesced (after backendPtr->stop()/close(), or before the first
    // start()), so destroying retired sources cannot race the RT render thread.
    void publishCurrent(std::unique_ptr<Node> nodePtr);

    void armLookahead(std::unique_ptr<Node> nodePtr);

    void publishActive(Node* node) noexcept { _active.store(node, std::memory_order_release); }

    void retireCursor() noexcept;

    void clear();

    void dropDisarmedLookahead(Node* node);

    std::unique_ptr<Node> promoteSplicedLookahead(Node* node);

    // Copy the active source pointer for non-RT callers (status / seek /
    // resume), keeping it alive for the duration of their use.
    std::shared_ptr<PcmSource> current() const;

  private:
    std::atomic<Node*> _active{nullptr};
    std::atomic<Node*> _lookahead{nullptr};
    mutable std::mutex _mutex;
    std::unique_ptr<Node> _currentNodePtr;
    std::unique_ptr<Node> _lookaheadNodePtr;
  };
} // namespace ao::audio::detail
