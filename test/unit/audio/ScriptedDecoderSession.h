// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#pragma once

#include <ao/audio/IDecoderSession.h>
#include <functional>
#include <vector>

namespace ao::audio
{
  class ScriptedDecoderSession : public IDecoderSession
  {
  public:
    struct ReadScriptEntry
    {
      std::vector<std::byte> data;
      bool endOfStream = false;
      ao::Result<> result = {};
    };

    explicit ScriptedDecoderSession(DecodedStreamInfo info)
      : _info(info)
    {
    }

    void setReadScript(std::vector<ReadScriptEntry> script) { _script = std::move(script); }

    ao::Result<> open(std::filesystem::path const& path) override
    {
      _opened = true;
      _lastOpenedPath = path;
      return _openResult;
    }

    void close() override { _closed = true; }
    void flush() override { _flushed = true; }

    ao::Result<> seek(std::uint32_t positionMs) override
    {
      _lastSeekPosition = positionMs;
      _scriptIdx = 0;
      return _seekResult;
    }

    ao::Result<PcmBlock> readNextBlock() override
    {
      _readCount++;
      if (_scriptIdx >= _script.size())
      {
        return PcmBlock{.bytes = {}, .bitDepth = 16, .frames = 0, .firstFrameIndex = 0, .endOfStream = true};
      }

      auto const& entry = _script[_scriptIdx++];

      if (!entry.result)
      {
        return std::unexpected(entry.result.error());
      }

      return PcmBlock{.bytes = entry.data,
                      .bitDepth = 16,
                      .frames = static_cast<std::uint32_t>(entry.data.size() / (16 / 8 * 2)),
                      .firstFrameIndex = 0,
                      .endOfStream = entry.endOfStream};
    }

    DecodedStreamInfo streamInfo() const override { return _info; }

    // Inspection
    bool isOpened() const { return _opened; }
    bool isClosed() const { return _closed; }
    bool isFlushed() const { return _flushed; }
    std::uint32_t lastSeekPosition() const { return _lastSeekPosition; }
    std::filesystem::path const& lastOpenedPath() const { return _lastOpenedPath; }
    std::size_t readCount() const { return _readCount; }

    // Configuration
    void setOpenResult(ao::Result<> res) { _openResult = res; }
    void setSeekResult(ao::Result<> res) { _seekResult = res; }

  private:
    DecodedStreamInfo _info;
    std::vector<ReadScriptEntry> _script;
    std::size_t _scriptIdx = 0;

    ao::Result<> _openResult = {};
    ao::Result<> _seekResult = {};

    bool _opened = false;
    bool _closed = false;
    bool _flushed = false;
    std::uint32_t _lastSeekPosition = 0;
    std::filesystem::path _lastOpenedPath;
    std::size_t _readCount = 0;
  };
} // namespace ao::audio
