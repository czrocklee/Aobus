// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "CueBinding.h"

#include "MediaTrack.h"
#include <ao/AudioCodec.h>
#include <ao/Contract.h>
#include <ao/Error.h>
#include <ao/library/FileTimeConversion.h>
#include <ao/library/LibraryUri.h>
#include <ao/library/TrackBuilder.h>
#include <ao/library/TrackLayout.h>
#include <ao/media/cue/Sheet.h>
#include <ao/rt/library/ScanPlan.h>
#include <ao/utility/Path.h>
#include <ao/utility/UnicodeText.h>
#include <ao/utility/Xxh3.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <ios>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace ao::rt
{
  namespace
  {
    std::uint16_t numberOr(std::optional<std::string> const& optText, std::uint16_t fallback)
    {
      if (!optText)
      {
        return fallback;
      }

      std::uint16_t value = 0;
      auto const [end, error] = std::from_chars(optText->data(), optText->data() + optText->size(), value);
      return error == std::errc{} && end != optText->data() ? value : fallback;
    }

    // An empty sheet value usually means unknown, so it keeps the fallback.
    std::string_view textOr(std::optional<std::string> const& optText, std::string_view fallback)
    {
      return optText && !optText->empty() ? std::string_view{*optText} : fallback;
    }

    constexpr std::size_t kMaxExternalCueBytes = 4'194'304;

    Result<std::string> readSheetBytes(std::filesystem::path const& path, std::uint64_t fileSize)
    {
      if (fileSize > kMaxExternalCueBytes)
      {
        return makeError(
          Error::Code::ValueTooLarge, std::format("CUE sheet exceeds the maximum of {} bytes", kMaxExternalCueBytes));
      }

      auto stream = std::ifstream{path, std::ios::binary};

      if (!stream)
      {
        return makeError(Error::Code::IoError, "Cannot read CUE sheet; check permissions and file availability");
      }

      auto bytes = std::string{};
      bytes.reserve(static_cast<std::size_t>(fileSize));
      constexpr std::size_t kReadBufferBytes = 8192;
      auto buffer = std::array<char, kReadBufferBytes>{};

      while (stream)
      {
        stream.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        auto const count = static_cast<std::size_t>(stream.gcount());

        // A file can grow after stat; retain and parse at most the same byte limit.
        if (count > kMaxExternalCueBytes - bytes.size())
        {
          return makeError(
            Error::Code::ValueTooLarge, std::format("CUE sheet exceeds the maximum of {} bytes", kMaxExternalCueBytes));
        }

        bytes.append(buffer.data(), count);
      }

      if (!stream.eof() || stream.bad())
      {
        return makeError(Error::Code::IoError, "Cannot read CUE sheet; check permissions and file availability");
      }

      return bytes;
    }

    CueSheetSnapshot readSheet(std::filesystem::path const& root, std::filesystem::path const& path, std::string name)
    {
      auto snapshot = CueSheetSnapshot{
        .evidence = {.name = std::move(name), .cueDiagnostic = ScanCueDiagnostic::SheetUnreadableOrMalformed}};
      auto uriRes = library::LibraryUri::parse(utility::pathToGenericUtf8(path.lexically_relative(root)));

      if (!uriRes)
      {
        snapshot.evidence.errorMessage = uriRes.error().message;
        return snapshot;
      }

      auto resolvedPathRes = uriRes->resolveUnder(root);

      if (!resolvedPathRes)
      {
        snapshot.evidence.errorMessage = resolvedPathRes.error().message;
        return snapshot;
      }

      auto const& resolvedPath = *resolvedPathRes;
      auto ec = std::error_code{};

      if (!std::filesystem::is_regular_file(resolvedPath, ec) || ec)
      {
        snapshot.evidence.errorMessage = "CUE sheet is not an accessible regular file";
        return snapshot;
      }

      auto const fileSize = std::filesystem::file_size(resolvedPath, ec);
      auto timestampRes = library::lastWriteTimestamp(resolvedPath);

      if (ec || !timestampRes)
      {
        snapshot.evidence.errorMessage = "Cannot inspect CUE sheet; check permissions and file availability";
        return snapshot;
      }

      snapshot.evidence.fileSize = fileSize;
      snapshot.evidence.optMtime = *timestampRes;
      auto bytesRes = readSheetBytes(resolvedPath, fileSize);

      if (!bytesRes)
      {
        snapshot.evidence.errorMessage = bytesRes.error().message;
        return snapshot;
      }

      auto const& bytes = *bytesRes;
      snapshot.evidence.digest = utility::xxh3Hash128(bytes);

      if (auto sheetRes = media::cue::parseSheet(bytes); !sheetRes)
      {
        snapshot.evidence.errorMessage = sheetRes.error().message;
        snapshot.evidence.cueDiagnostic = utility::validateUtf8(bytes) ? ScanCueDiagnostic::SheetUnreadableOrMalformed
                                                                       : ScanCueDiagnostic::InvalidEncoding;
      }
      else
      {
        snapshot.optSheet = std::move(*sheetRes);
        snapshot.evidence.cueDiagnostic = ScanCueDiagnostic::None;
      }

      return snapshot;
    }

    // Names the first imported range that the sheet no longer reproduces.
    std::optional<std::string> describeCueLayoutMismatch(ScanManifestEvidence const& stored,
                                                         std::span<library::TrackSegment const> layout)
    {
      for (auto const& member : stored.members)
      {
        auto const& imported = member.segment;
        auto const candidate = std::ranges::find(layout, imported.ordinal, &library::TrackSegment::ordinal);

        if (candidate == layout.end())
        {
          return std::format(
            "CUE sheet differs from the imported segments: it has no audio TRACK {}; segments kept", imported.ordinal);
        }

        if (*candidate != imported)
        {
          return std::format("CUE sheet differs from the imported segments: TRACK {} starts at frame {} for {} "
                             "frames, but was imported at frame {} for {} frames; segments kept",
                             imported.ordinal,
                             candidate->startFrame,
                             candidate->frameCount,
                             imported.startFrame,
                             imported.frameCount);
        }
      }

      return std::nullopt;
    }
  } // namespace

  bool isCueSheet(std::filesystem::path const& path)
  {
    auto extension = utility::pathToUtf8(path.extension());

    for (auto& character : extension)
    {
      if (character >= 'A' && character <= 'Z')
      {
        character = static_cast<char>(character - 'A' + 'a');
      }
    }

    return extension == ".cue";
  }

  Result<std::vector<CueSheetSnapshot>> readCueDirectory(std::filesystem::path const& root,
                                                         std::filesystem::path const& directory)
  {
    auto ec = std::error_code{};
    auto const resolvedRoot = std::filesystem::weakly_canonical(root, ec);

    if (ec)
    {
      return makeError(Error::Code::IoError, "Cannot resolve CUE root: " + ec.message());
    }

    auto const resolvedDirectory = std::filesystem::weakly_canonical(directory, ec);

    if (ec)
    {
      return makeError(Error::Code::IoError, "Cannot resolve CUE directory: " + ec.message());
    }

    auto const relative = resolvedDirectory.lexically_relative(resolvedRoot);

    if (relative.empty() || relative.is_absolute() ||
        std::ranges::any_of(relative, [](auto const& component) { return component == ".."; }))
    {
      return makeError(Error::Code::InvalidInput, "CUE directory resolves outside the library root");
    }

    auto sheets = std::vector<CueSheetSnapshot>{};
    auto it = std::filesystem::directory_iterator{resolvedDirectory, ec};

    while (!ec && it != std::filesystem::directory_iterator{})
    {
      if (auto const path = it->path(); isCueSheet(path))
      {
        // Keep every entry named like a sheet, including a FIFO, a directory, or an
        // entry that fails stat: dropping one could expose an embedded sheet or import
        // the carrier whole. readSheet reports one that is not a regular file without opening it.
        // A name that fails the URI parse can name no audio file in the library, since
        // the same stem fails that parse for the audio file too.
        if (auto nameRes = library::LibraryUri::parse(utility::pathToGenericUtf8(path.filename())); nameRes)
        {
          sheets.push_back(readSheet(resolvedRoot, path, std::string{nameRes->value()}));
        }
      }

      it.increment(ec);
    }

    if (ec)
    {
      return makeError(Error::Code::IoError, "Cannot list CUE directory: " + ec.message());
    }

    std::ranges::sort(sheets, {}, [](auto const& sheet) { return sheet.evidence.name; });
    return sheets;
  }

  ScanCueEvidence cueEvidenceFor(std::string_view audioName, std::span<CueSheetSnapshot const> sheets)
  {
    auto evidence = ScanCueEvidence{};

    for (auto const& sheet : sheets)
    {
      switch (media::cue::classifySheetCandidate(audioName, sheet.evidence.name))
      {
        case media::cue::SheetCandidate::General: evidence.general.push_back(sheet.evidence); break;
        case media::cue::SheetCandidate::ExtensionSpecific: evidence.extensionSpecific.push_back(sheet.evidence); break;
        case media::cue::SheetCandidate::None: break;
      }
    }

    return evidence;
  }

  CueBinding bindCueSheet(std::string_view audioName, CueMediaFacts media, std::span<CueSheetSnapshot const> sheets)
  {
    auto binding = CueBinding{.evidence = cueEvidenceFor(audioName, sheets)};
    auto const& candidates =
      binding.evidence.extensionSpecific.empty() ? binding.evidence.general : binding.evidence.extensionSpecific;

    if (candidates.size() > 1)
    {
      binding.state = CueBindingState::Undetermined;
      binding.errorMessage = "Competing CUE sheets:";
      binding.cueDiagnostic = ScanCueDiagnostic::AmbiguousSheets;

      for (auto const& candidate : candidates)
      {
        binding.errorMessage += " " + candidate.name;

        if (!binding.cueDetail.empty())
        {
          binding.cueDetail += ", ";
        }

        binding.cueDetail += candidate.name;
      }

      return binding;
    }

    if (!candidates.empty())
    {
      auto const& candidate = candidates.front();
      auto const sheet =
        std::ranges::find(sheets, candidate.name, [](auto const& value) { return value.evidence.name; });
      binding.optSheet = sheet->optSheet;
      binding.errorMessage = candidate.errorMessage;
      binding.cueDiagnostic = candidate.cueDiagnostic;
    }
    else if (media.codec == AudioCodec::Flac && media.embeddedSheets.size() > 1)
    {
      // Differing comments compete like same-name sheets; neither is chosen.
      binding.state = CueBindingState::Undetermined;
      binding.errorMessage = "Competing embedded CUESHEET comments";
      binding.cueDiagnostic = ScanCueDiagnostic::AmbiguousSheets;
      binding.cueDetail = "CUESHEET";
      return binding;
    }
    else if (media.codec == AudioCodec::Flac && !media.embeddedSheets.empty())
    {
      // Vorbis dispatch may surface CUESHEET for any codec. Only a FLAC comment
      // is an embedded binding source; other codecs stay unbound unless an external sheet applies.
      auto const embeddedSheet = media.embeddedSheets.front();

      if (auto sheetRes = media::cue::parseSheet(embeddedSheet); sheetRes)
      {
        binding.optSheet = std::move(*sheetRes);
      }
      else
      {
        binding.errorMessage = sheetRes.error().message;
        binding.cueDiagnostic = utility::validateUtf8(embeddedSheet) ? ScanCueDiagnostic::SheetUnreadableOrMalformed
                                                                     : ScanCueDiagnostic::InvalidEncoding;
      }
    }
    else
    {
      return binding;
    }

    if (!binding.optSheet)
    {
      binding.state = CueBindingState::Undetermined;
      return binding;
    }

    auto const audioCount =
      std::ranges::count(binding.optSheet->tracks, media::cue::TrackType::Audio, &media::cue::Track::type);

    if (audioCount < 2)
    {
      return binding;
    }

    if ((media.codec != AudioCodec::Flac && media.codec != AudioCodec::Wav) || !media.optTotalFrames)
    {
      binding.state = CueBindingState::Undetermined;
      binding.errorMessage = "This file cannot be a CUE carrier; remove or rename the sheet to import the file whole";
      binding.cueDiagnostic = ScanCueDiagnostic::UnsupportedCarrier;
      return binding;
    }

    auto layoutRes = media::cue::resolveSegments(*binding.optSheet, media.sampleRate, *media.optTotalFrames);

    if (!layoutRes)
    {
      binding.state = CueBindingState::Undetermined;
      binding.errorMessage = layoutRes.error().message;
      binding.cueDiagnostic = ScanCueDiagnostic::LayoutMismatch;
      return binding;
    }

    for (auto const& segment : *layoutRes)
    {
      binding.layout.push_back(library::TrackSegment{
        .startFrame = segment.startFrame, .frameCount = segment.frameCount, .ordinal = segment.ordinal});
    }

    binding.state = CueBindingState::Bound;
    return binding;
  }

  CueBinding bindCueSheet(std::string_view audioName, MediaTrack const& media, std::span<CueSheetSnapshot const> sheets)
  {
    return bindCueSheet(audioName,
                        CueMediaFacts{.codec = media.builder().property().codec(),
                                      .sampleRate = media.builder().property().sampleRate().raw(),
                                      .optTotalFrames = media.optTotalFrames(),
                                      .embeddedSheets = media.cueSheets()},
                        sheets);
  }

  std::vector<library::TrackBuilder> makeCueTracks(MediaTrack const& media, CueBinding const& binding)
  {
    auto tracks = std::vector<library::TrackBuilder>{};

    if (binding.state != CueBindingState::Bound)
    {
      return tracks;
    }

    AO_INVARIANT(binding.optSheet, "Bound CUE metadata has no parsed sheet");
    auto const& sheet = *binding.optSheet;
    auto const& carrier = media.builder().metadata();

    for (auto const& segment : binding.layout)
    {
      auto const& track = *std::ranges::find(sheet.tracks, segment.ordinal, &media::cue::Track::ordinal);
      auto builder = media.builder();
      builder.property().segment(segment);
      builder.metadata()
        .title(textOr(track.metadata.optTitle, carrier.title()))
        .artist(textOr(track.metadata.optPerformer, textOr(sheet.metadata.optPerformer, carrier.artist())))
        .album(textOr(sheet.metadata.optTitle, carrier.album()))
        .albumArtist(textOr(sheet.metadata.optPerformer, carrier.albumArtist()))
        .composer(textOr(track.metadata.optSongwriter, textOr(sheet.metadata.optSongwriter, carrier.composer())))
        .genre(textOr(track.metadata.optGenre, textOr(sheet.metadata.optGenre, carrier.genre())))
        .year(numberOr(track.metadata.optDate, numberOr(sheet.metadata.optDate, carrier.year())))
        .discNumber(
          numberOr(track.metadata.optDiscNumber, numberOr(sheet.metadata.optDiscNumber, carrier.discNumber())))
        .discTotal(numberOr(track.metadata.optTotalDiscs, numberOr(sheet.metadata.optTotalDiscs, carrier.discTotal())))
        .trackNumber(segment.ordinal)
        .trackTotal(static_cast<std::uint16_t>(binding.layout.size()));
      tracks.push_back(std::move(builder));
    }

    return tracks;
  }

  void classifyStoredCueBinding(ScanItem& item)
  {
    if (!item.optManifestEvidence)
    {
      return;
    }

    auto const& stored = *item.optManifestEvidence;

    if (!stored.members.empty() && item.hasExactMediaFacts &&
        (item.totalFrames != stored.totalFrames || item.sampleRate != stored.sampleRate))
    {
      item.classification = ScanClassification::Error;
      item.errorMessage = "Carrier extent or sample rate differs from imported segments";
      item.cueDiagnostic = ScanCueDiagnostic::CarrierFactsMismatch;
      return;
    }

    if (item.cueDiagnostic != ScanCueDiagnostic::None)
    {
      return;
    }

    if (stored.members.empty())
    {
      if (!item.cueLayout.empty())
      {
        item.errorMessage = "Imported as a whole file; remove the Track to split it";
        item.cueDiagnostic = ScanCueDiagnostic::WholeFileAlreadyImported;
      }
    }
    else if (item.cueLayout.empty())
    {
      item.errorMessage = "No sheet divides this file; imported segments kept";
      item.cueDiagnostic = ScanCueDiagnostic::SheetMissingForCarrier;
    }
    else if (auto optMismatch = describeCueLayoutMismatch(stored, item.cueLayout); optMismatch)
    {
      item.errorMessage = std::move(*optMismatch);
      item.cueDiagnostic = ScanCueDiagnostic::LayoutMismatch;
    }
    else if (item.classification == ScanClassification::Unchanged && item.cueLayout.size() > stored.members.size())
    {
      item.classification = ScanClassification::MissingSegments;
    }
  }
} // namespace ao::rt
