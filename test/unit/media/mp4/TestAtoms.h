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
  inline void appendBe32(std::vector<std::uint8_t>& buffer, std::uint32_t value)
  {
    auto valueBuf = boost::endian::big_uint32_buf_t{};
    valueBuf = value;
    auto const* valueAddr = reinterpret_cast<std::uint8_t const*>(&valueBuf);
    buffer.insert(buffer.end(), valueAddr, valueAddr + 4);
  }

  inline void appendBe64(std::vector<std::uint8_t>& buffer, std::uint64_t value)
  {
    auto valueBuf = boost::endian::big_uint64_buf_t{};
    valueBuf = value;
    auto const* valueAddr = reinterpret_cast<std::uint8_t const*>(&valueBuf);
    buffer.insert(buffer.end(), valueAddr, valueAddr + 8);
  }

  inline void addAtom(std::vector<std::uint8_t>& buffer, std::string_view type, std::vector<std::uint8_t> const& body)
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

  inline std::vector<std::uint8_t> makeExtendedAtom(std::string_view type, std::vector<std::uint8_t> const& body)
  {
    auto atom = std::vector<std::uint8_t>{};
    appendBe32(atom, 1);
    atom.insert(atom.end(), type.begin(), type.begin() + 4);
    appendBe64(atom, 16U + static_cast<std::uint64_t>(body.size()));
    atom.insert(atom.end(), body.begin(), body.end());
    return atom;
  }

  inline std::vector<std::uint8_t> makeEndOfFileAtom(std::string_view type, std::vector<std::uint8_t> const& body)
  {
    auto atom = std::vector<std::uint8_t>{};
    appendBe32(atom, 0);
    atom.insert(atom.end(), type.begin(), type.begin() + 4);
    atom.insert(atom.end(), body.begin(), body.end());
    return atom;
  }

  inline std::vector<std::uint8_t> makeExtendedFromCompactAtom(std::vector<std::uint8_t> const& atom)
  {
    if (atom.size() < 8)
    {
      return {};
    }

    auto const type = std::string_view{reinterpret_cast<char const*>(atom.data() + 4), 4};
    auto const body = std::vector<std::uint8_t>{atom.begin() + 8, atom.end()};
    return makeExtendedAtom(type, body);
  }

  inline std::vector<std::uint8_t> makeAudioSampleEntryAtom(std::string_view sampleEntryType,
                                                            std::vector<std::uint8_t> const& extensions = {},
                                                            std::uint32_t sampleRate = 44100)
  {
    auto sampleEntry = media::mp4::AudioSampleEntryLayout{};
    sampleEntry.common.length = sizeof(media::mp4::AudioSampleEntryLayout);
    std::memcpy(sampleEntry.common.type.data(), sampleEntryType.data(), 4);
    sampleEntry.channelCount = 2;
    sampleEntry.sampleSize = 16;
    sampleEntry.sampleRate = (sampleRate << 16);

    auto const* sampleEntryAddr = reinterpret_cast<std::uint8_t const*>(&sampleEntry);
    auto sampleEntryBody = std::vector<std::uint8_t>{};
    sampleEntryBody.insert(
      sampleEntryBody.end(), sampleEntryAddr + 8, sampleEntryAddr + sizeof(media::mp4::AudioSampleEntryLayout));
    sampleEntryBody.insert(sampleEntryBody.end(), extensions.begin(), extensions.end());

    return makeAtom(sampleEntryType, sampleEntryBody);
  }

  inline std::vector<std::uint8_t> makeSampleEntryAtom(std::string_view sampleEntryType,
                                                       std::vector<std::uint8_t> const& body)
  {
    return makeAtom(sampleEntryType, body);
  }

  inline std::vector<std::uint8_t> makeStsdAtom(std::string_view sampleEntryType,
                                                std::vector<std::uint8_t> const& sampleEntryExtensions = {},
                                                std::uint32_t sampleRate = 44100)
  {
    auto stsdBody = std::vector<std::uint8_t>{0, 0, 0, 0, 0, 0, 0, 1};
    auto const sampleEntryAtom = makeAudioSampleEntryAtom(sampleEntryType, sampleEntryExtensions, sampleRate);
    stsdBody.insert(stsdBody.end(), sampleEntryAtom.begin(), sampleEntryAtom.end());
    return makeAtom("stsd", stsdBody);
  }

  inline std::vector<std::uint8_t> makeStsdAtomFromSampleEntries(
    std::vector<std::vector<std::uint8_t>> const& sampleEntryAtoms)
  {
    auto stsdBody = std::vector<std::uint8_t>{};
    appendBe32(stsdBody, 0);
    appendBe32(stsdBody, static_cast<std::uint32_t>(sampleEntryAtoms.size()));

    for (auto const& sampleEntryAtom : sampleEntryAtoms)
    {
      stsdBody.insert(stsdBody.end(), sampleEntryAtom.begin(), sampleEntryAtom.end());
    }

    return makeAtom("stsd", stsdBody);
  }

  inline std::vector<std::uint8_t> makeStsdAtomFromSampleEntry(std::vector<std::uint8_t> const& sampleEntryAtom)
  {
    return makeStsdAtomFromSampleEntries({sampleEntryAtom});
  }

  inline std::vector<std::uint8_t> makeHdlrAtom(std::string_view handlerType)
  {
    auto body = std::vector<std::uint8_t>{};
    appendBe32(body, 0);
    appendBe32(body, 0);
    body.insert(body.end(), handlerType.begin(), handlerType.begin() + 4);
    appendBe32(body, 0);
    appendBe32(body, 0);
    appendBe32(body, 0);
    return makeAtom("hdlr", body);
  }

  inline std::vector<std::uint8_t> makeMdhdAtom(std::uint32_t timescale = 44100, std::uint32_t duration = 44100)
  {
    auto mdhd = media::mp4::MdhdAtomLayout{};
    mdhd.common.length = sizeof(media::mp4::MdhdAtomLayout);
    std::memcpy(mdhd.common.type.data(), "mdhd", 4);
    mdhd.timescale = 44100;
    mdhd.timescale = timescale;
    mdhd.duration = duration;

    auto const* mdhdAddr = reinterpret_cast<std::uint8_t const*>(&mdhd);
    auto mdhdBody = std::vector<std::uint8_t>{};
    mdhdBody.insert(mdhdBody.end(), mdhdAddr + 8, mdhdAddr + sizeof(media::mp4::MdhdAtomLayout));
    return makeAtom("mdhd", mdhdBody);
  }

  inline std::vector<std::uint8_t> makeMdhdVersion1Atom(std::uint32_t timescale, std::uint64_t duration)
  {
    auto body = std::vector<std::uint8_t>{1, 0, 0, 0};
    appendBe64(body, 0);
    appendBe64(body, 0);
    appendBe32(body, timescale);
    appendBe64(body, duration);
    appendBe32(body, 0);
    return makeAtom("mdhd", body);
  }

  inline std::vector<std::uint8_t> makeStszAtom(std::uint32_t sampleSize = 4, std::uint32_t sampleCount = 1)
  {
    auto body = std::vector<std::uint8_t>{};
    appendBe32(body, 0);
    appendBe32(body, sampleSize);
    appendBe32(body, sampleCount);
    return makeAtom("stsz", body);
  }

  inline std::vector<std::uint8_t> makeSttsAtom(std::uint32_t sampleCount = 1, std::uint32_t sampleDelta = 1024)
  {
    auto body = std::vector<std::uint8_t>{};
    appendBe32(body, 0);
    appendBe32(body, 1);
    appendBe32(body, sampleCount);
    appendBe32(body, sampleDelta);
    return makeAtom("stts", body);
  }

  inline std::vector<std::uint8_t> makeStscAtom(std::uint32_t samplesPerChunk = 1,
                                                std::uint32_t sampleDescriptionIndex = 1,
                                                std::uint32_t firstChunk = 1)
  {
    auto body = std::vector<std::uint8_t>{};
    appendBe32(body, 0);
    appendBe32(body, 1);
    appendBe32(body, firstChunk);
    appendBe32(body, samplesPerChunk);
    appendBe32(body, sampleDescriptionIndex);
    return makeAtom("stsc", body);
  }

  inline std::vector<std::uint8_t> makeStcoAtom(std::uint32_t chunkOffset = 0)
  {
    auto body = std::vector<std::uint8_t>{};
    appendBe32(body, 0);
    appendBe32(body, 1);
    appendBe32(body, chunkOffset);
    return makeAtom("stco", body);
  }

  inline std::vector<std::uint8_t> makeSampleTableAtom(std::vector<std::uint8_t> const& stsdAtom,
                                                       std::uint32_t sampleSize = 4,
                                                       std::uint32_t sampleDelta = 1024,
                                                       std::uint32_t chunkOffset = 0)
  {
    auto body = std::vector<std::uint8_t>{};
    auto const stsz = makeStszAtom(sampleSize);
    auto const stts = makeSttsAtom(1, sampleDelta);
    auto const stsc = makeStscAtom();
    auto const stco = makeStcoAtom(chunkOffset);
    body.insert(body.end(), stsdAtom.begin(), stsdAtom.end());
    body.insert(body.end(), stsz.begin(), stsz.end());
    body.insert(body.end(), stts.begin(), stts.end());
    body.insert(body.end(), stsc.begin(), stsc.end());
    body.insert(body.end(), stco.begin(), stco.end());
    return makeAtom("stbl", body);
  }

  inline std::vector<std::uint8_t> makeTrackAtom(std::string_view handlerType,
                                                 std::vector<std::uint8_t> const& stblAtom,
                                                 std::uint32_t timescale = 44100,
                                                 std::uint32_t duration = 44100)
  {
    auto const mdhdAtom = makeMdhdAtom(timescale, duration);
    auto const hdlrAtom = makeHdlrAtom(handlerType);
    auto const minfAtom = makeAtom("minf", stblAtom);

    auto mdiaBody = std::vector<std::uint8_t>{};
    mdiaBody.insert(mdiaBody.end(), mdhdAtom.begin(), mdhdAtom.end());
    mdiaBody.insert(mdiaBody.end(), hdlrAtom.begin(), hdlrAtom.end());
    mdiaBody.insert(mdiaBody.end(), minfAtom.begin(), minfAtom.end());
    auto const mdiaAtom = makeAtom("mdia", mdiaBody);

    return makeAtom("trak", mdiaAtom);
  }

  inline std::vector<std::uint8_t> makeTrackAtomWithMdhd(std::string_view handlerType,
                                                         std::vector<std::uint8_t> const& stblAtom,
                                                         std::vector<std::uint8_t> const& mdhdAtom)
  {
    auto const hdlrAtom = makeHdlrAtom(handlerType);
    auto const minfAtom = makeAtom("minf", stblAtom);

    auto mdiaBody = std::vector<std::uint8_t>{};
    mdiaBody.insert(mdiaBody.end(), mdhdAtom.begin(), mdhdAtom.end());
    mdiaBody.insert(mdiaBody.end(), hdlrAtom.begin(), hdlrAtom.end());
    mdiaBody.insert(mdiaBody.end(), minfAtom.begin(), minfAtom.end());
    auto const mdiaAtom = makeAtom("mdia", mdiaBody);

    return makeAtom("trak", mdiaAtom);
  }

  inline std::vector<std::uint8_t> makeAudioTrackAtom(std::string_view sampleEntryType,
                                                      std::vector<std::uint8_t> const& sampleEntryExtensions = {})
  {
    auto const stsdAtom = makeStsdAtom(sampleEntryType, sampleEntryExtensions);
    auto const stblAtom = makeAtom("stbl", stsdAtom);
    return makeTrackAtom("soun", stblAtom);
  }

  inline std::vector<std::uint8_t> makeCompleteAudioTrackAtom(
    std::string_view sampleEntryType,
    std::vector<std::uint8_t> const& sampleEntryExtensions = {},
    std::uint32_t timescale = 44100,
    std::uint32_t duration = 44100,
    std::uint32_t sampleSize = 4,
    std::uint32_t sampleDelta = 1024,
    std::uint32_t chunkOffset = 0)
  {
    auto const stsdAtom = makeStsdAtom(sampleEntryType, sampleEntryExtensions, timescale);
    auto const stblAtom = makeSampleTableAtom(stsdAtom, sampleSize, sampleDelta, chunkOffset);
    return makeTrackAtom("soun", stblAtom, timescale, duration);
  }

  inline std::vector<std::uint8_t> makeVideoTrackAtom(std::string_view sampleEntryType = "avc1")
  {
    auto const sampleEntry = makeSampleEntryAtom(sampleEntryType, std::vector<std::uint8_t>(78, 0));
    auto const stsdAtom = makeStsdAtomFromSampleEntry(sampleEntry);
    auto const stblAtom = makeAtom("stbl", stsdAtom);
    return makeTrackAtom("vide", stblAtom, 90000, 90000);
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
