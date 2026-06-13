// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include <ao/Error.h>
#include <ao/audio/DecoderTypes.h>
#include <ao/audio/IDecoderSession.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <utility>
#include <vector>

namespace ao::audio::test
{
  class ScriptedDecoderSession final : public IDecoderSession
  {
  public:
    struct ReadScriptEntry final
    {
      std::vector<std::byte> data;
      bool endOfStream = false;
      Result<> result = {};
    };

    explicit ScriptedDecoderSession(DecodedStreamInfo info)
      : _info{info}
    {
    }

    void setReadScript(std::vector<ReadScriptEntry> script) { _script = std::move(script); }

    Result<> open(std::filesystem::path const& path) override
    {
      _opened = true;
      _lastOpenedPath = path;
      return _openResult;
    }

    void close() override { _closed = true; }
    void flush() override { _flushed = true; }

    Result<> seek(std::chrono::milliseconds offset) override
    {
      _lastSeekOffset = offset;
      _scriptIdx = 0;
      return _seekResult;
    }

    Result<PcmBlock> readNextBlock() override
    {
      _readCount++;

      if (_scriptIdx >= _script.size())
      {
        return PcmBlock{.bytes = {}, .bitDepth = 16, .frames = 0, .firstFrameIndex = 0, .endOfStream = true};
      }

      auto const& entry = _script[_scriptIdx++];

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

    DecodedStreamInfo streamInfo() const override { return _info; }

    // Inspection
    bool isOpened() const { return _opened; }
    bool isClosed() const { return _closed; }
    bool isFlushed() const { return _flushed; }
    std::chrono::milliseconds lastSeekOffset() const { return _lastSeekOffset; }
    std::filesystem::path const& lastOpenedPath() const { return _lastOpenedPath; }
    std::size_t readCount() const { return _readCount; }

    // Configuration
    void setOpenResult(Result<> res) { _openResult = res; }
    void setSeekResult(Result<> res) { _seekResult = res; }

  private:
    DecodedStreamInfo _info;
    std::vector<ReadScriptEntry> _script;
    std::size_t _scriptIdx = 0;

    Result<> _openResult = {};
    Result<> _seekResult = {};

    bool _opened = false;
    bool _closed = false;
    bool _flushed = false;
    std::chrono::milliseconds _lastSeekOffset{0};
    std::filesystem::path _lastOpenedPath;
    std::size_t _readCount = 0;
  };
} // namespace ao::audio::test
