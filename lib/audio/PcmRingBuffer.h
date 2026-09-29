// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <boost/lockfree/policies.hpp>
#include <boost/lockfree/spsc_queue.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

namespace ao::audio
{
  // Capacity in bytes. 2 MiB holds about 1.82 s of 192 kHz stereo packed
  // 24-bit PCM or 1.37 s at 32-bit.
  constexpr std::size_t kRingBufferCapacity = 2097152;

  // Store raw bytes (supports any bitdepth: 16/24/32-bit). Normal access follows
  // the queue's single-producer/single-consumer contract. clear() is a control-
  // domain operation and requires exclusive access against queue operations.
  //
  // Frame-alignment invariant: buffered content is always a whole number of
  // @p frameSize frames. write() commits only whole frames and leaves any
  // sub-frame remainder with the caller; read() returns only whole frames and
  // keeps any sub-frame fragment buffered. The invariant closes the RenderTarget
  // frame contract at the only boundary that can floor without discarding PCM:
  // a fragment parked here is still buffered, while a fragment floored after a
  // pop would already be lost to the reader.
  class PcmRingBuffer final
  {
  public:
    // Requires 0 < frameSize <= kRingBufferCapacity. The frame size is immutable
    // after construction and only read by both access threads, so it needs no
    // further synchronization.
    explicit PcmRingBuffer(std::size_t frameSize);

    // Write whole frames of input. Returns the number of bytes actually
    // written: always a multiple of frameSize, and never more than
    // availableToWrite() observed at the call. A trailing partial frame, or a
    // trailing whole frame that cannot fully fit, stays with the caller.
    std::size_t write(std::span<std::byte const> input) noexcept;

    // Read whole frames into output. Returns the number of bytes actually
    // read: always a multiple of frameSize, and never more than size() observed
    // at the call. A partial-frame request floor leaves the fragment buffered.
    std::size_t read(std::span<std::byte> output) noexcept;

    // Reset to empty in constant time. No write(), read(), size(), or
    // availableToWrite() call may overlap this call. Debug builds diagnose
    // violations of this exclusive-access precondition.
    void clear() noexcept;

    // Bytes currently buffered (available to read). Committed content is always
    // a whole number of frames. Within the SPSC contract the sole consumer
    // sees readable content only grow between query and pop, so its
    // observations are exact whole-frame counts. Any other observer, such as
    // the producer or a third thread taking a status snapshot, receives only
    // an advisory value: it may pair stale in-flight index updates, so it is
    // neither guaranteed frame-aligned nor a view of any single queue state,
    // and it provides no synchronization. The queue tracks this internally,
    // so no separate accounting is kept.
    std::size_t size() const noexcept;

    // Whole-frame bytes the producer can write without a partial write. This is
    // an advisory SPSC snapshot: the consumer can only increase it.
    std::size_t availableToWrite() const noexcept;

    // Whole-frame byte capacity: kRingBufferCapacity floored to whole frames.
    std::size_t capacity() const noexcept { return _frameCapacity * _frameSize; }

    std::size_t frameSize() const noexcept { return _frameSize; }

  private:
    using Queue = boost::lockfree::spsc_queue<std::byte, boost::lockfree::capacity<kRingBufferCapacity>>;

    // The queue embeds its 2MB buffer, which is far too large for a by-value
    // member (stack frames and enclosing objects would inherit it), so it
    // lives on the heap.
    std::unique_ptr<Queue> _queuePtr;

    std::size_t const _frameSize;
    // Whole-frame capacity in frames; immutable after construction.
    std::size_t const _frameCapacity;

    // Kept in the object in every configuration for layout stability, but only
    // touched by debug builds so release render/producer paths gain no access
    // accounting. The high bit denotes clear(); the remaining bits count
    // ordinary queue operations.
    mutable std::atomic<std::uint32_t> _debugAccessState{0};

    void beginDebugAccess() const noexcept;
    void endDebugAccess() const noexcept;
    void beginDebugClear() noexcept;
    void endDebugClear() noexcept;
  };
} // namespace ao::audio
