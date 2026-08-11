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
#include <functional>
#include <memory>
#include <optional>
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

    explicit ScriptedDecoderSession(DecodedStreamInfo info);

    // Increments the shared counter (if any) on destruction, so a test can
    // observe that retired gapless sources are actually reclaimed rather than
    // accumulated across a continuous splice run.
    ~ScriptedDecoderSession() override;

    ScriptedDecoderSession(ScriptedDecoderSession const&) = delete;
    ScriptedDecoderSession& operator=(ScriptedDecoderSession const&) = delete;
    ScriptedDecoderSession(ScriptedDecoderSession&&) = delete;
    ScriptedDecoderSession& operator=(ScriptedDecoderSession&&) = delete;

    void setReadScript(std::vector<ReadScriptEntry> script);

    // Replaces the read script on the next seek(), letting a test script the
    // post-seek stream independently of initial playback (e.g. an empty script
    // makes every seek land at end-of-stream).
    void setSeekReadScript(std::vector<ReadScriptEntry> script);
    void setSeekObserver(std::function<void(std::chrono::milliseconds)> observer);
    void setReadObserver(std::function<void(std::size_t)> observer);
    void setDestroyCounter(std::shared_ptr<std::atomic<std::size_t>> counterPtr);

    void flush() noexcept override;
    Result<> seek(std::chrono::milliseconds offset) noexcept override;
    Result<PcmBlock> readNextBlock() noexcept override;
    DecodedStreamInfo streamInfo() const noexcept override;

    // Inspection
    bool isFlushed() const;
    std::chrono::milliseconds lastSeekOffset() const;
    std::size_t seekCount() const;
    std::size_t readCount() const;

    // Configuration
    void setSeekResult(Result<> resRes);

  private:
    DecodedStreamInfo _info;
    std::vector<ReadScriptEntry> _script;
    std::optional<std::vector<ReadScriptEntry>> _optSeekScript;
    std::function<void(std::chrono::milliseconds)> _seekObserver;
    std::function<void(std::size_t)> _readObserver;
    std::size_t _scriptIndex = 0;
    std::size_t _seekCount = 0;

    Result<> _seekRes = {};

    bool _flushed = false;
    std::chrono::milliseconds _lastSeekOffset{0};
    std::size_t _readCount = 0;
    std::shared_ptr<std::atomic<std::size_t>> _destroyCounterPtr;
  };
} // namespace ao::audio::test
