// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "ScriptedDecoderSession.h"

#include <ao/Error.h>
#include <ao/audio/DecodedStreamInfo.h>
#include <ao/audio/PcmBlock.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <memory>
#include <utility>
#include <vector>

namespace ao::audio::test
{
  ScriptedDecoderSession::ScriptedDecoderSession(DecodedStreamInfo info)
    : _info{info}
  {
  }

  ScriptedDecoderSession::~ScriptedDecoderSession()
  {
    if (_destroyCounterPtr)
    {
      _destroyCounterPtr->fetch_add(1, std::memory_order_relaxed);
    }
  }

  void ScriptedDecoderSession::setReadScript(std::vector<ReadScriptEntry> script)
  {
    _script = std::move(script);
  }

  void ScriptedDecoderSession::setSeekReadScript(std::vector<ReadScriptEntry> script)
  {
    _optSeekScript = std::move(script);
  }

  void ScriptedDecoderSession::setSeekObserver(std::function<void(std::chrono::milliseconds)> observer)
  {
    _seekObserver = std::move(observer);
  }

  void ScriptedDecoderSession::setReadObserver(std::function<void(std::size_t)> observer)
  {
    _readObserver = std::move(observer);
  }

  void ScriptedDecoderSession::setDestroyCounter(std::shared_ptr<std::atomic<std::size_t>> counterPtr)
  {
    _destroyCounterPtr = std::move(counterPtr);
  }

  Result<> ScriptedDecoderSession::open(std::filesystem::path const& path) noexcept
  {
    _opened = true;
    _lastOpenedPath = path;
    return _openResult;
  }

  void ScriptedDecoderSession::close() noexcept
  {
    _closed = true;
  }

  void ScriptedDecoderSession::flush() noexcept
  {
    _flushed = true;
  }

  Result<> ScriptedDecoderSession::seek(std::chrono::milliseconds offset) noexcept
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

  Result<PcmBlock> ScriptedDecoderSession::readNextBlock() noexcept
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

  DecodedStreamInfo ScriptedDecoderSession::streamInfo() const noexcept
  {
    return _info;
  }

  bool ScriptedDecoderSession::isOpened() const
  {
    return _opened;
  }

  bool ScriptedDecoderSession::isClosed() const
  {
    return _closed;
  }

  bool ScriptedDecoderSession::isFlushed() const
  {
    return _flushed;
  }

  std::chrono::milliseconds ScriptedDecoderSession::lastSeekOffset() const
  {
    return _lastSeekOffset;
  }

  std::size_t ScriptedDecoderSession::seekCount() const
  {
    return _seekCount;
  }

  std::filesystem::path const& ScriptedDecoderSession::lastOpenedPath() const
  {
    return _lastOpenedPath;
  }

  std::size_t ScriptedDecoderSession::readCount() const
  {
    return _readCount;
  }

  void ScriptedDecoderSession::setOpenResult(Result<> res)
  {
    _openResult = std::move(res);
  }

  void ScriptedDecoderSession::setSeekResult(Result<> res)
  {
    _seekResult = std::move(res);
  }
} // namespace ao::audio::test
