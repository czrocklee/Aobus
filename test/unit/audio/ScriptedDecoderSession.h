// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include <ao/Error.h>
#include <ao/audio/DecodedStreamInfo.h>
#include <ao/audio/DecoderSession.h>
#include <ao/audio/PcmBlock.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace ao::audio::test
{
  class [[nodiscard]] ScriptedDecoderSession final : public DecoderSession
  {
  public:
    struct ReadScriptEntry final
    {
      std::vector<std::byte> data = {};
      bool endOfStream = false;
      Result<> result = {};
    };

    explicit ScriptedDecoderSession(DecodedStreamInfo info)
      : _info{info}
    {
    }

    // Increments the shared counter (if any) on destruction, so a test can
    // observe that retired gapless sources are actually reclaimed rather than
    // accumulated across a continuous splice run.
    ~ScriptedDecoderSession() override
    {
      if (_destroyCounterPtr)
      {
        _destroyCounterPtr->fetch_add(1, std::memory_order_relaxed);
      }
    }

    ScriptedDecoderSession(ScriptedDecoderSession const&) = delete;
    ScriptedDecoderSession& operator=(ScriptedDecoderSession const&) = delete;
    ScriptedDecoderSession(ScriptedDecoderSession&&) = delete;
    ScriptedDecoderSession& operator=(ScriptedDecoderSession&&) = delete;

    void setReadScript(std::vector<ReadScriptEntry> script) { _script = std::move(script); }

    // Replaces the read script on the next seek(), letting a test script the
    // post-seek stream independently of initial playback (e.g. an empty script
    // makes every seek land at end-of-stream).
    void setSeekReadScript(std::vector<ReadScriptEntry> script) { _optSeekScript = std::move(script); }
    void setSeekObserver(std::function<void(std::chrono::milliseconds)> observer)
    {
      _seekObserver = std::move(observer);
    }
    void setReadObserver(std::function<void(std::size_t)> observer) { _readObserver = std::move(observer); }

    void setDestroyCounter(std::shared_ptr<std::atomic<std::size_t>> counterPtr)
    {
      _destroyCounterPtr = std::move(counterPtr);
    }

    Result<> open(std::filesystem::path const& path) noexcept override
    {
      _opened = true;
      _lastOpenedPath = path;
      return _openResult;
    }

    void close() noexcept override { _closed = true; }
    void flush() noexcept override { _flushed = true; }

    Result<> seek(std::chrono::milliseconds offset) noexcept override
    {
      _lastSeekOffset = offset;
      ++_seekCount;

      if (_seekObserver)
      {
        _seekObserver(offset);
      }

      if (_optSeekScript)
      {
        _script = *_optSeekScript;
      }

      _scriptIndex = 0;
      return _seekResult;
    }

    Result<PcmBlock> readNextBlock() noexcept override
    {
      _readCount++;

      if (_readObserver)
      {
        _readObserver(_readCount);
      }

      if (_scriptIndex >= _script.size())
      {
        return PcmBlock{.bitDepth = 16, .frames = 0, .firstFrameIndex = 0, .endOfStream = true};
      }

      auto const& entry = _script[_scriptIndex++];

      if (!entry.result)
      {
        return std::unexpected{entry.result.error()};
      }

      return PcmBlock{.bytes = entry.data,
                      .bitDepth = 16,
                      .frames = static_cast<std::uint32_t>(entry.data.size() / std::size_t{4}),
                      .firstFrameIndex = 0,
                      .endOfStream = entry.endOfStream};
    }

    DecodedStreamInfo streamInfo() const noexcept override { return _info; }

    // Inspection
    bool isOpened() const { return _opened; }
    bool isClosed() const { return _closed; }
    bool isFlushed() const { return _flushed; }
    std::chrono::milliseconds lastSeekOffset() const { return _lastSeekOffset; }
    std::size_t seekCount() const { return _seekCount; }
    std::filesystem::path const& lastOpenedPath() const { return _lastOpenedPath; }
    std::size_t readCount() const { return _readCount; }

    // Configuration
    void setOpenResult(Result<> res) { _openResult = res; }
    void setSeekResult(Result<> res) { _seekResult = res; }

  private:
    DecodedStreamInfo _info;
    std::vector<ReadScriptEntry> _script;
    std::optional<std::vector<ReadScriptEntry>> _optSeekScript;
    std::function<void(std::chrono::milliseconds)> _seekObserver;
    std::function<void(std::size_t)> _readObserver;
    std::size_t _scriptIndex = 0;
    std::size_t _seekCount = 0;

    Result<> _openResult = {};
    Result<> _seekResult = {};

    bool _opened = false;
    bool _closed = false;
    bool _flushed = false;
    std::chrono::milliseconds _lastSeekOffset{0};
    std::filesystem::path _lastOpenedPath;
    std::size_t _readCount = 0;
    std::shared_ptr<std::atomic<std::size_t>> _destroyCounterPtr;
  };
} // namespace ao::audio::test
