// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#include "core/playback/AlacDecoderSession.h"
#include "core/Log.h"

#include <alac/ALACDecoder.h>
#include <alac/ALACBitUtilities.h>

#include <boost/endian/buffers.hpp>
#include <fstream>
#include <optional>
#include <vector>

namespace app::core::playback
{

  struct AlacDecoderSession::Impl
  {
    std::ifstream file;
    std::string error;
    DecodedStreamInfo info;
    
    std::unique_ptr<ALACDecoder> decoder;
    
    struct SampleEntry
    {
      std::uint64_t offset;
      std::uint32_t size;
    };
    std::vector<SampleEntry> samples;
    std::uint32_t currentSampleIndex = 0;
    std::uint32_t timescale = 0;
    
    std::vector<std::byte> magicCookie;

    Impl(StreamFormat /*output*/)
    {
      decoder = std::make_unique<ALACDecoder>();
    }

    bool parseMp4();
    void setError(std::string_view msg) { error = std::string(msg); }
  };

  AlacDecoderSession::AlacDecoderSession(StreamFormat outputFormat)
    : _impl(std::make_unique<Impl>(outputFormat))
  {
  }

  AlacDecoderSession::~AlacDecoderSession() = default;

  bool AlacDecoderSession::open(std::filesystem::path const& filePath)
  {
    _impl->file.open(filePath, std::ios::binary);
    if (!_impl->file)
    {
      _impl->setError("Failed to open file");
      return false;
    }

    if (!_impl->parseMp4())
    {
      return false;
    }

    if (!_impl->magicCookie.empty())
    {
      _impl->decoder->Init(_impl->magicCookie.data(), static_cast<uint32_t>(_impl->magicCookie.size()));
    }
    else
    {
      _impl->setError("Missing ALAC magic cookie");
      return false;
    }

    return true;
  }

  void AlacDecoderSession::close()
  {
    _impl->file.close();
    _impl->samples.clear();
    _impl->currentSampleIndex = 0;
  }

  bool AlacDecoderSession::seek(std::uint32_t /*positionMs*/)
  {
    if (_impl->timescale == 0) return false;
    _impl->currentSampleIndex = 0; 
    return true;
  }

  void AlacDecoderSession::flush()
  {
  }

  std::optional<PcmBlock> AlacDecoderSession::readNextBlock()
  {
    if (_impl->currentSampleIndex >= _impl->samples.size())
    {
      return PcmBlock{.bytes = {}, .endOfStream = true};
    }

    auto const& entry = _impl->samples[_impl->currentSampleIndex];
    std::vector<std::byte> packet(entry.size);
    _impl->file.seekg(static_cast<std::streamoff>(entry.offset));
    _impl->file.read(reinterpret_cast<char*>(packet.data()), static_cast<std::streamsize>(entry.size));

    if (!_impl->file)
    {
      return std::nullopt;
    }

    std::uint32_t numFrames = 0;
    std::vector<std::byte> decodedPcm(1024 * 1024); 
    
    BitBuffer bitBuffer;
    BitBufferInit(&bitBuffer, reinterpret_cast<uint8_t*>(packet.data()), static_cast<uint32_t>(packet.size()));

    auto const status = _impl->decoder->Decode(&bitBuffer, reinterpret_cast<uint8_t*>(decodedPcm.data()), 4096, _impl->info.outputFormat.channels, &numFrames);
    
    if (status != 0)
    {
      return std::nullopt;
    }

    auto block = PcmBlock{};
    auto const bytesPerFrame = _impl->info.outputFormat.channels * (_impl->info.outputFormat.bitDepth / 8);
    decodedPcm.resize(numFrames * bytesPerFrame);
    block.bytes = std::move(decodedPcm);
    block.frames = numFrames;
    block.bitDepth = _impl->info.outputFormat.bitDepth;
    
    _impl->currentSampleIndex++;
    block.endOfStream = (_impl->currentSampleIndex >= _impl->samples.size());

    return block;
  }

  DecodedStreamInfo AlacDecoderSession::streamInfo() const
  {
    return _impl->info;
  }

  std::string_view AlacDecoderSession::lastError() const noexcept
  {
    return _impl->error;
  }

  bool AlacDecoderSession::Impl::parseMp4()
  {
    auto readU32 = [this]() -> std::uint32_t {
      boost::endian::big_uint32_buf_t val;
      file.read(reinterpret_cast<char*>(&val), 4);
      return val.value();
    };

    auto getFourCC = [this]() -> std::string {
      char buf[4];
      file.read(buf, 4);
      return std::string(buf, 4);
    };

    auto skip = [this](std::uint64_t bytes) {
      file.seekg(static_cast<std::streamoff>(bytes), std::ios::cur);
    };

    file.seekg(0, std::ios::end);
    auto fileSize = static_cast<std::uint64_t>(file.tellg());
    file.seekg(0, std::ios::beg);

    bool foundMoov = false;
    while (file.tellg() < static_cast<std::streamoff>(fileSize))
    {
      auto pos = static_cast<std::uint64_t>(file.tellg());
      auto len = readU32();
      auto type = getFourCC();

      if (type == "moov")
      {
        foundMoov = true;
        continue;
      }
      
      if (type == "trak" || type == "mdia" || type == "minf" || type == "stbl")
      {
        continue;
      }

      if (type == "stsd")
      {
        skip(8); 
        auto entryPos = static_cast<std::uint64_t>(file.tellg());
        auto entrySize = readU32();
        auto format = getFourCC();

        if (format != "alac" && format != "mp4a")
        {
          setError("Not an ALAC or MP4A file");
          return false;
        }
        
        file.seekg(static_cast<std::streamoff>(entryPos + 8 + 28));
        
        auto extensionsEnd = entryPos + entrySize;
        bool foundAlacTag = false;
        while (static_cast<std::uint64_t>(file.tellg()) + 8 <= extensionsEnd) {
          auto extSize = readU32();
          auto extType = getFourCC();
          if (extType == "alac") {
             magicCookie.resize(extSize - 8);
             file.read(reinterpret_cast<char*>(magicCookie.data()), static_cast<std::streamsize>(extSize - 8));
             foundAlacTag = true;
             break;
          }
          if (extSize < 8) break;
          file.seekg(static_cast<std::streamoff>(extSize - 8), std::ios::cur);
        }

        if (!foundAlacTag)
        {
           setError("Missing alac extension atom in stsd entry");
           return false;
        }
        
        file.seekg(static_cast<std::streamoff>(pos + len));
        continue;
      }

      if (type == "mdhd")
      {
        skip(12); 
        timescale = readU32();
        auto duration = readU32();
        info.durationMs = duration * 1000 / timescale;
        file.seekg(static_cast<std::streamoff>(pos + len));
        continue;
      }

      if (type == "stsz")
      {
        skip(4); 
        auto sampleSize = readU32();
        auto count = readU32();
        samples.resize(count);
        for (std::uint32_t i = 0; i < count; ++i)
        {
          samples[i].size = (sampleSize == 0) ? readU32() : sampleSize;
        }
        file.seekg(static_cast<std::streamoff>(pos + len));
        continue;
      }

      if (type == "stco")
      {
        skip(4); 
        auto count = readU32();
        if (count == samples.size())
        {
           for (std::uint32_t i = 0; i < count; ++i)
           {
             samples[i].offset = readU32();
           }
        }
        file.seekg(static_cast<std::streamoff>(pos + len));
        continue;
      }

      if (len < 8) break; 
      file.seekg(static_cast<std::streamoff>(pos + len));
    }

    if (!foundMoov)
    {
      setError("Not a valid MP4 file (missing moov)");
      return false;
    }

    info.sourceFormat.channels = 2; 
    info.sourceFormat.sampleRate = timescale;
    info.sourceFormat.bitDepth = 16;
    info.outputFormat = info.sourceFormat;

    return !magicCookie.empty() && !samples.empty();
  }

} // namespace app::core::playback
