// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/media/mp4/AtomLayout.h>

#include <boost/endian/buffers.hpp>

#include <cstdint>
#include <cstring>
#include <string_view>
#include <vector>

namespace ao::test::mp4
{
  inline void addAtom(std::vector<std::uint8_t>& buffer,
                      std::string_view type,
                      std::vector<std::uint8_t> const& body)
  {
    auto const length = 8U + static_cast<std::uint32_t>(body.size());
    auto lenBuf = boost::endian::big_uint32_buf_t{};
    lenBuf = length;
    auto const* lenAddr = reinterpret_cast<std::uint8_t const*>(&lenBuf);
    buffer.insert(buffer.end(), lenAddr, lenAddr + 4);
    buffer.insert(buffer.end(), type.begin(), type.begin() + 4);
    buffer.insert(buffer.end(), body.begin(), body.end());
  }

  inline std::vector<std::uint8_t> makeAtom(std::string_view type, std::vector<std::uint8_t> const& body)
  {
    auto atom = std::vector<std::uint8_t>{};
    addAtom(atom, type, body);
    return atom;
  }

  inline std::vector<std::uint8_t> makeAudioSampleEntryAtom(std::string_view sampleEntryType,
                                                            std::vector<std::uint8_t> const& extensions = {})
  {
    auto sampleEntry = media::mp4::AudioSampleEntryLayout{};
    sampleEntry.common.length = sizeof(media::mp4::AudioSampleEntryLayout);
    std::memcpy(sampleEntry.common.type.data(), sampleEntryType.data(), 4);
    sampleEntry.channelCount = 2;
    sampleEntry.sampleSize = 16;
    sampleEntry.sampleRate = (44100 << 16);

    auto const* sampleEntryAddr = reinterpret_cast<std::uint8_t const*>(&sampleEntry);
    auto sampleEntryBody = std::vector<std::uint8_t>{};
    sampleEntryBody.insert(
      sampleEntryBody.end(), sampleEntryAddr + 8, sampleEntryAddr + sizeof(media::mp4::AudioSampleEntryLayout));
    sampleEntryBody.insert(sampleEntryBody.end(), extensions.begin(), extensions.end());

    return makeAtom(sampleEntryType, sampleEntryBody);
  }

  inline std::vector<std::uint8_t> makeStsdAtom(std::string_view sampleEntryType,
                                                std::vector<std::uint8_t> const& sampleEntryExtensions = {})
  {
    auto stsdBody = std::vector<std::uint8_t>{0, 0, 0, 0, 0, 0, 0, 1};
    auto const sampleEntryAtom = makeAudioSampleEntryAtom(sampleEntryType, sampleEntryExtensions);
    stsdBody.insert(stsdBody.end(), sampleEntryAtom.begin(), sampleEntryAtom.end());
    return makeAtom("stsd", stsdBody);
  }

  inline std::vector<std::uint8_t> makeAudioTrackAtom(std::string_view sampleEntryType,
                                                      std::vector<std::uint8_t> const& sampleEntryExtensions = {})
  {
    auto mdhd = media::mp4::MdhdAtomLayout{};
    mdhd.common.length = sizeof(media::mp4::MdhdAtomLayout);
    std::memcpy(mdhd.common.type.data(), "mdhd", 4);
    mdhd.timescale = 44100;
    mdhd.duration = 44100;

    auto const* mdhdAddr = reinterpret_cast<std::uint8_t const*>(&mdhd);
    auto mdhdBody = std::vector<std::uint8_t>{};
    mdhdBody.insert(mdhdBody.end(), mdhdAddr + 8, mdhdAddr + sizeof(media::mp4::MdhdAtomLayout));
    auto const mdhdAtom = makeAtom("mdhd", mdhdBody);

    auto const stsdAtom = makeStsdAtom(sampleEntryType, sampleEntryExtensions);
    auto const stblAtom = makeAtom("stbl", stsdAtom);
    auto const minfAtom = makeAtom("minf", stblAtom);

    auto mdiaBody = std::vector<std::uint8_t>{};
    mdiaBody.insert(mdiaBody.end(), mdhdAtom.begin(), mdhdAtom.end());
    mdiaBody.insert(mdiaBody.end(), minfAtom.begin(), minfAtom.end());
    auto const mdiaAtom = makeAtom("mdia", mdiaBody);

    return makeAtom("trak", mdiaAtom);
  }

  inline std::vector<std::uint8_t> makeMinimalAudioMp4(std::string_view sampleEntryType,
                                                       std::vector<std::uint8_t> const& sampleEntryExtensions = {})
  {
    auto data = std::vector<std::uint8_t>{};
    auto const trakAtom = makeAudioTrackAtom(sampleEntryType, sampleEntryExtensions);
    addAtom(data, "moov", trakAtom);
    return data;
  }
} // namespace ao::test::mp4
