// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <cstdint>
#include <string_view>
#include <vector>

namespace ao::test::mp4
{
  void appendBe32(std::vector<std::uint8_t>& buffer, std::uint32_t value);
  void appendBe64(std::vector<std::uint8_t>& buffer, std::uint64_t value);
  void addAtom(std::vector<std::uint8_t>& buffer, std::string_view type, std::vector<std::uint8_t> const& body);
  std::vector<std::uint8_t> makeAtom(std::string_view type, std::vector<std::uint8_t> const& body);
  std::vector<std::uint8_t> makeExtendedAtom(std::string_view type, std::vector<std::uint8_t> const& body);
  std::vector<std::uint8_t> makeEndOfFileAtom(std::string_view type, std::vector<std::uint8_t> const& body);
  std::vector<std::uint8_t> makeExtendedFromCompactAtom(std::vector<std::uint8_t> const& atom);
  std::vector<std::uint8_t> makeAudioSampleEntryAtom(std::string_view sampleEntryType,
                                                     std::vector<std::uint8_t> const& extensions = {},
                                                     std::uint32_t sampleRate = 44100);
  std::vector<std::uint8_t> makeSampleEntryAtom(std::string_view sampleEntryType,
                                                std::vector<std::uint8_t> const& body);
  std::vector<std::uint8_t> makeStsdAtom(std::string_view sampleEntryType,
                                         std::vector<std::uint8_t> const& sampleEntryExtensions = {},
                                         std::uint32_t sampleRate = 44100);
  std::vector<std::uint8_t> makeStsdAtomFromSampleEntries(
    std::vector<std::vector<std::uint8_t>> const& sampleEntryAtoms);
  std::vector<std::uint8_t> makeStsdAtomFromSampleEntry(std::vector<std::uint8_t> const& sampleEntryAtom);
  std::vector<std::uint8_t> makeHdlrAtom(std::string_view handlerType);
  std::vector<std::uint8_t> makeMdhdAtom(std::uint32_t timescale = 44100, std::uint32_t duration = 44100);
  std::vector<std::uint8_t> makeMdhdVersion1Atom(std::uint32_t timescale, std::uint64_t duration);
  std::vector<std::uint8_t> makeStszAtom(std::uint32_t sampleSize = 4, std::uint32_t sampleCount = 1);
  std::vector<std::uint8_t> makeSttsAtom(std::uint32_t sampleCount = 1, std::uint32_t sampleDelta = 1024);
  std::vector<std::uint8_t> makeStscAtom(std::uint32_t samplesPerChunk = 1,
                                         std::uint32_t sampleDescriptionIndex = 1,
                                         std::uint32_t firstChunk = 1);
  std::vector<std::uint8_t> makeStcoAtom(std::uint32_t chunkOffset = 0);
  std::vector<std::uint8_t> makeSampleTableAtom(std::vector<std::uint8_t> const& stsdAtom,
                                                std::uint32_t sampleSize = 4,
                                                std::uint32_t sampleDelta = 1024,
                                                std::uint32_t chunkOffset = 0);
  std::vector<std::uint8_t> makeTrackAtom(std::string_view handlerType,
                                          std::vector<std::uint8_t> const& stblAtom,
                                          std::uint32_t timescale = 44100,
                                          std::uint32_t duration = 44100);
  std::vector<std::uint8_t> makeTrackAtomWithMdhd(std::string_view handlerType,
                                                  std::vector<std::uint8_t> const& stblAtom,
                                                  std::vector<std::uint8_t> const& mdhdAtom);
  std::vector<std::uint8_t> makeAudioTrackAtom(std::string_view sampleEntryType,
                                               std::vector<std::uint8_t> const& sampleEntryExtensions = {});
  std::vector<std::uint8_t> makeCompleteAudioTrackAtom(std::string_view sampleEntryType,
                                                       std::vector<std::uint8_t> const& sampleEntryExtensions = {},
                                                       std::uint32_t timescale = 44100,
                                                       std::uint32_t duration = 44100,
                                                       std::uint32_t sampleSize = 4,
                                                       std::uint32_t sampleDelta = 1024,
                                                       std::uint32_t chunkOffset = 0);
  std::vector<std::uint8_t> makeVideoTrackAtom(std::string_view sampleEntryType = "avc1");
  std::vector<std::uint8_t> makeMinimalAudioMp4(std::string_view sampleEntryType,
                                                std::vector<std::uint8_t> const& sampleEntryExtensions = {});
} // namespace ao::test::mp4
