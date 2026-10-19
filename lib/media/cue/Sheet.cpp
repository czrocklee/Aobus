// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include <ao/media/cue/Sheet.h>

#include <ao/Contract.h>
#include <ao/Error.h>
#include <ao/utility/UnicodeText.h>

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <iterator>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace ao::media::cue
{
  namespace
  {
    constexpr std::uint64_t kCdFramesPerSecond = 75;
    constexpr std::uint64_t kCdFramesPerMinute = 4500;
    constexpr std::uint64_t kMaxCueTrackOrdinal = 99;

    bool isSpace(char const character) noexcept
    {
      return character == ' ' || character == '\t' || character == '\r';
    }

    std::string_view trim(std::string_view text) noexcept
    {
      while (!text.empty() && isSpace(text.front()))
      {
        text.remove_prefix(1);
      }

      while (!text.empty() && isSpace(text.back()))
      {
        text.remove_suffix(1);
      }

      return text;
    }

    char foldAscii(char const character) noexcept
    {
      return character >= 'A' && character <= 'Z' ? static_cast<char>(character + ('a' - 'A')) : character;
    }

    bool isEqualAscii(std::string_view const lhs, std::string_view const rhs) noexcept
    {
      return lhs.size() == rhs.size() && std::ranges::equal(lhs, rhs, {}, foldAscii, foldAscii);
    }

    // Backslashes in quoted CUE text are literal (not C/C++ escapes), notably
    // in ignored FILE paths. A closing quote must end its token.
    Result<std::string_view> takeToken(std::string_view& text)
    {
      text = trim(text);

      if (text.empty())
      {
        return makeError(Error::Code::InvalidInput, "Expected a CUE token");
      }

      if (text.front() == '"')
      {
        auto const closing = text.find('"', 1);

        if (closing == std::string_view::npos || (closing + 1 < text.size() && !isSpace(text[closing + 1])))
        {
          return makeError(Error::Code::InvalidInput, "Unterminated or incorrectly delimited quoted CUE token");
        }

        auto const token = text.substr(1, closing - 1);
        text = trim(text.substr(closing + 1));
        return token;
      }

      auto const end = text.find_first_of(" \t\r");
      auto const token = text.substr(0, end);

      if (token.contains('"'))
      {
        return makeError(Error::Code::InvalidInput, "Quote inside an unquoted CUE token");
      }

      text = end == std::string_view::npos ? std::string_view{} : trim(text.substr(end));
      return token;
    }

    Result<std::string> takeText(std::string_view& text)
    {
      text = trim(text);

      if (text.empty())
      {
        return makeError(Error::Code::InvalidInput, "Expected CUE text (use quotes for an empty value)");
      }

      if (text.front() == '"')
      {
        auto const tokenRes = takeToken(text);

        if (!tokenRes)
        {
          return std::unexpected{tokenRes.error()};
        }

        if (!text.empty())
        {
          return makeError(Error::Code::InvalidInput, "Unexpected text after quoted CUE value");
        }

        return std::string{*tokenRes};
      }

      if (text.contains('"'))
      {
        return makeError(Error::Code::InvalidInput, "Quote inside an unquoted CUE value");
      }

      auto value = std::string{text};
      text = {};
      return value;
    }

    Result<std::uint64_t> parseUnsigned(std::string_view const token)
    {
      if (token.empty() ||
          !std::ranges::all_of(token, [](char const character) { return character >= '0' && character <= '9'; }))
      {
        return makeError(Error::Code::InvalidInput, "Expected an unsigned decimal CUE number");
      }

      std::uint64_t value = 0;
      auto const converted = std::from_chars(token.data(), token.data() + token.size(), value);

      if (converted.ec != std::errc{} || converted.ptr != token.data() + token.size())
      {
        return makeError(Error::Code::InvalidInput, "CUE number exceeds the supported integer range");
      }

      return value;
    }

    Result<std::uint64_t> parseTime(std::string_view const token)
    {
      auto const firstColon = token.find(':');
      auto const secondColon = token.find(':', firstColon == std::string_view::npos ? 0 : firstColon + 1);

      if (firstColon == std::string_view::npos || secondColon == std::string_view::npos ||
          secondColon - firstColon != 3 || token.size() - secondColon != 3)
      {
        return makeError(Error::Code::InvalidInput, "Expected CUE time mm:ss:ff with ss < 60 and ff < 75");
      }

      auto const minutesRes = parseUnsigned(token.substr(0, firstColon));
      auto const secondsRes = parseUnsigned(token.substr(firstColon + 1, 2));
      auto const framesRes = parseUnsigned(token.substr(secondColon + 1));

      if (!minutesRes || !secondsRes || !framesRes || *secondsRes >= 60 || *framesRes >= kCdFramesPerSecond)
      {
        return makeError(Error::Code::InvalidInput, "Invalid CUE time; use mm:ss:ff with ss < 60 and ff < 75");
      }

      auto const tail = (*secondsRes * kCdFramesPerSecond) + *framesRes;

      if (*minutesRes > (std::numeric_limits<std::uint64_t>::max() - tail) / kCdFramesPerMinute)
      {
        return makeError(Error::Code::InvalidInput, "CUE time exceeds the supported CD-frame range");
      }

      return (*minutesRes * kCdFramesPerMinute) + tail;
    }

    Result<TrackType> parseTrackType(std::string_view const token)
    {
      if (isEqualAscii(token, "AUDIO"))
      {
        return TrackType::Audio;
      }

      for (auto const* const type : {"CDG",
                                     "MODE1/2048",
                                     "MODE1/2352",
                                     "MODE2/2048",
                                     "MODE2/2324",
                                     "MODE2/2336",
                                     "MODE2/2352",
                                     "CDI/2336",
                                     "CDI/2352"})
      {
        if (isEqualAscii(token, type))
        {
          return TrackType::Data;
        }
      }

      return makeError(Error::Code::InvalidInput, "Unknown CUE TRACK type");
    }

    bool isFileType(std::string_view const token) noexcept
    {
      return isEqualAscii(token, "BINARY") || isEqualAscii(token, "MOTOROLA") || isEqualAscii(token, "AIFF") ||
             isEqualAscii(token, "WAVE") || isEqualAscii(token, "MP3") || isEqualAscii(token, "FLAC");
    }

    struct ParseState final
    {
      Sheet sheet{};
      bool hasFile = false;
      std::optional<std::uint64_t> optLastIndex{};
      std::optional<std::uint64_t> optLastPosition{};
      bool hasPregap = false;
      bool hasPostgap = false;
    };

    Result<> parseMetadata(ParseState& state, std::string_view const command, std::string_view text)
    {
      auto& metadata = state.sheet.tracks.empty() ? state.sheet.metadata : state.sheet.tracks.back().metadata;
      std::optional<std::string>* target = nullptr;

      if (isEqualAscii(command, "TITLE"))
      {
        target = &metadata.optTitle;
      }
      else if (isEqualAscii(command, "PERFORMER"))
      {
        target = &metadata.optPerformer;
      }
      else if (isEqualAscii(command, "SONGWRITER"))
      {
        target = &metadata.optSongwriter;
      }
      else
      {
        // Unknown REM payloads are comments, not grammar: do not require their
        // quotes or tokens to follow the syntax of recognized metadata.
        auto const keyRes = takeToken(text);

        if (!keyRes)
        {
          return {};
        }

        if (auto const key = *keyRes; isEqualAscii(key, "GENRE"))
        {
          target = &metadata.optGenre;
        }
        else if (isEqualAscii(key, "DATE"))
        {
          target = &metadata.optDate;
        }
        else if (isEqualAscii(key, "DISCNUMBER"))
        {
          target = &metadata.optDiscNumber;
        }
        else if (isEqualAscii(key, "TOTALDISCS"))
        {
          target = &metadata.optTotalDiscs;
        }
        else
        {
          return {};
        }
      }

      auto valueRes = takeText(text);

      if (!valueRes)
      {
        return std::unexpected{valueRes.error()};
      }

      // An identical copy changes nothing; choosing between different ones
      // would silently drop one of them.
      if (target->has_value() && **target != *valueRes)
      {
        return makeError(Error::Code::InvalidInput, "Metadata is repeated with a different value within one CUE scope");
      }

      *target = std::move(*valueRes);
      return {};
    }

    Result<> parseFile(ParseState& state, std::string_view text)
    {
      if (state.hasFile || !state.sheet.tracks.empty())
      {
        return makeError(Error::Code::InvalidInput, "A CUE sheet must contain exactly one FILE before its TRACKs");
      }

      if (auto const filenameRes = takeToken(text), typeRes = takeToken(text);
          !filenameRes || filenameRes->empty() || !typeRes || !isFileType(*typeRes) || !text.empty())
      {
        return makeError(Error::Code::InvalidInput, "Expected FILE filename followed by a supported CUE file type");
      }

      state.hasFile = true;
      return {};
    }

    Result<> parseTrack(ParseState& state, std::string_view text)
    {
      if (!state.hasFile)
      {
        return makeError(Error::Code::InvalidInput, "FILE must precede TRACK");
      }

      auto const ordinalTokenRes = takeToken(text);
      auto const typeTokenRes = takeToken(text);

      if (!ordinalTokenRes || !typeTokenRes || !text.empty())
      {
        return makeError(Error::Code::InvalidInput, "Expected TRACK number and type");
      }

      auto const ordinalRes = parseUnsigned(*ordinalTokenRes);
      auto const typeRes = parseTrackType(*typeTokenRes);

      if (!ordinalRes || *ordinalRes < 1 || *ordinalRes > kMaxCueTrackOrdinal ||
          (!state.sheet.tracks.empty() && *ordinalRes <= state.sheet.tracks.back().ordinal))
      {
        return makeError(Error::Code::InvalidInput, "TRACK numbers must strictly increase from 1 to 99");
      }

      if (!typeRes)
      {
        return std::unexpected{typeRes.error()};
      }

      state.sheet.tracks.push_back(Track{.ordinal = static_cast<std::uint16_t>(*ordinalRes), .type = *typeRes});
      state.optLastIndex.reset();
      state.optLastPosition.reset();
      state.hasPregap = false;
      state.hasPostgap = false;
      return {};
    }

    Result<> parseIndex(ParseState& state, std::string_view text)
    {
      if (state.sheet.tracks.empty())
      {
        return makeError(Error::Code::InvalidInput, "INDEX requires a TRACK");
      }

      auto const indexTokenRes = takeToken(text);
      auto const timeTokenRes = takeToken(text);

      if (!indexTokenRes || !timeTokenRes || !text.empty())
      {
        return makeError(Error::Code::InvalidInput, "Expected INDEX number and mm:ss:ff");
      }

      auto const indexRes = parseUnsigned(*indexTokenRes);
      auto const positionRes = parseTime(*timeTokenRes);

      if (!indexRes || *indexRes > kMaxCueTrackOrdinal || (state.optLastIndex && *indexRes <= *state.optLastIndex))
      {
        return makeError(
          Error::Code::InvalidInput, "INDEX numbers must strictly increase from 00 to 99 within a TRACK");
      }

      if (!positionRes)
      {
        return std::unexpected{positionRes.error()};
      }

      if (state.optLastPosition && *positionRes < *state.optLastPosition)
      {
        return makeError(Error::Code::InvalidInput, "INDEX positions must not decrease within a TRACK");
      }

      state.optLastIndex = *indexRes;
      state.optLastPosition = *positionRes;

      if (auto& track = state.sheet.tracks.back(); *indexRes == 0)
      {
        track.optIndex00 = *positionRes;
      }
      else if (*indexRes == 1)
      {
        track.optIndex01 = *positionRes;
      }

      return {};
    }

    Result<> parseGap(ParseState& state, std::string_view const command, std::string_view text)
    {
      if (state.sheet.tracks.empty())
      {
        return makeError(Error::Code::InvalidInput, "PREGAP and POSTGAP require a TRACK");
      }

      auto& hasGap = isEqualAscii(command, "PREGAP") ? state.hasPregap : state.hasPostgap;
      auto const timeTokenRes = takeToken(text);

      if (hasGap || !timeTokenRes || !text.empty())
      {
        return makeError(Error::Code::InvalidInput, "Expected a single gap time per TRACK");
      }

      if (auto const timeRes = parseTime(*timeTokenRes); !timeRes)
      {
        return std::unexpected{timeRes.error()};
      }

      hasGap = true;
      return {};
    }

    Result<> parseFlags(ParseState& state, std::string_view text)
    {
      if (state.sheet.tracks.empty() || text.empty())
      {
        return makeError(Error::Code::InvalidInput, "FLAGS requires a TRACK and at least one flag");
      }

      while (!text.empty())
      {
        if (auto const flagRes = takeToken(text);
            !flagRes || !(isEqualAscii(*flagRes, "DCP") || isEqualAscii(*flagRes, "4CH") ||
                          isEqualAscii(*flagRes, "PRE") || isEqualAscii(*flagRes, "SCMS")))
        {
          return makeError(Error::Code::InvalidInput, "Unknown CUE flag");
        }
      }

      return {};
    }

    Result<> parseIgnoredDirective(ParseState& state, std::string_view const command, std::string_view text)
    {
      if ((isEqualAscii(command, "ISRC") && state.sheet.tracks.empty()) ||
          (!isEqualAscii(command, "ISRC") && !state.sheet.tracks.empty()))
      {
        return makeError(Error::Code::InvalidInput, "CUE directive appears at the wrong sheet or TRACK scope");
      }

      if (auto const valueRes = takeToken(text); !valueRes || valueRes->empty() || !text.empty())
      {
        return makeError(Error::Code::InvalidInput, "Expected one value for ignored CUE directive");
      }

      return {};
    }

    // Quote at most this many bytes of parser input. Legitimate CUE commands are
    // far shorter; 128 stops a multi-megabyte unknown token from being copied
    // into a diagnostic. Cuts move back to a UTF-8 code-point boundary.
    constexpr std::size_t kDiagnosticExcerptBytes = 128;
    constexpr std::uint8_t kUtf8ContinuationMask = 0xC0;
    constexpr std::uint8_t kUtf8ContinuationTag = 0x80;

    // Longest prefix of complete UTF-8 code points that fits in the excerpt budget.
    // Validated sheet text never splits a sequence here; continuation bytes are
    // still stepped back so a cut cannot land mid-scalar.
    std::size_t diagnosticExcerptEnd(std::string_view const text) noexcept
    {
      if (text.size() <= kDiagnosticExcerptBytes)
      {
        return text.size();
      }

      auto end = kDiagnosticExcerptBytes;

      // text[end] is the first byte past the budget. A continuation byte means
      // that index splits a code point, so step back to its lead byte.
      while (end > 0 && (static_cast<unsigned char>(text[end]) & kUtf8ContinuationMask) == kUtf8ContinuationTag)
      {
        --end;
      }

      return end;
    }

    // A raw control byte could rewrite or hide text in a terminal or log line,
    // so the excerpt spells it as a \xNN escape.
    std::string escapeControlBytes(std::string_view const text)
    {
      constexpr std::uint8_t kFirstPrintableAscii = 0x20;
      constexpr std::uint8_t kAsciiDelete = 0x7F;
      auto escaped = std::string{};
      escaped.reserve(text.size());

      for (auto const ch : text)
      {
        if (auto const byte = static_cast<std::uint8_t>(ch); byte < kFirstPrintableAscii || byte == kAsciiDelete)
        {
          std::format_to(std::back_inserter(escaped), "\\x{:02X}", byte);
        }
        else
        {
          escaped.push_back(ch);
        }
      }

      return escaped;
    }

    // Every diagnostic that quotes sheet text must use this helper. A token that
    // already fits keeps the historical quoted form, with no ellipsis or count.
    std::string quoteDiagnosticExcerpt(std::string_view const text)
    {
      if (text.size() <= kDiagnosticExcerptBytes)
      {
        return std::format("'{}'", escapeControlBytes(text));
      }

      auto const end = diagnosticExcerptEnd(text);
      return std::format("'{}'... ({} bytes)", escapeControlBytes(text.substr(0, end)), text.size());
    }

    Result<> parseLine(ParseState& state, std::string_view text)
    {
      auto const commandRes = takeToken(text);

      if (!commandRes)
      {
        return std::unexpected{commandRes.error()};
      }

      auto const command = *commandRes;

      if (isEqualAscii(command, "TITLE") || isEqualAscii(command, "PERFORMER") || isEqualAscii(command, "SONGWRITER") ||
          isEqualAscii(command, "REM"))
      {
        return parseMetadata(state, command, text);
      }

      if (isEqualAscii(command, "FILE"))
      {
        return parseFile(state, text);
      }

      if (isEqualAscii(command, "TRACK"))
      {
        return parseTrack(state, text);
      }

      if (isEqualAscii(command, "INDEX"))
      {
        return parseIndex(state, text);
      }

      if (isEqualAscii(command, "PREGAP") || isEqualAscii(command, "POSTGAP"))
      {
        return parseGap(state, command, text);
      }

      if (isEqualAscii(command, "FLAGS"))
      {
        return parseFlags(state, text);
      }

      if (isEqualAscii(command, "ISRC") || isEqualAscii(command, "CATALOG") || isEqualAscii(command, "CDTEXTFILE"))
      {
        return parseIgnoredDirective(state, command, text);
      }

      return makeError(
        Error::Code::InvalidInput, std::format("Unknown CUE directive {}", quoteDiagnosticExcerpt(command)));
    }

    Result<> validateSheet(Sheet const& sheet)
    {
      std::uint16_t previousOrdinal = 0;
      auto optPreviousIndex01 = std::optional<std::uint64_t>{};
      bool hasAudio = false;

      for (auto const& track : sheet.tracks)
      {
        if (track.ordinal <= previousOrdinal || track.ordinal > kMaxCueTrackOrdinal)
        {
          return makeError(Error::Code::InvalidInput, "TRACK numbers must strictly increase from 1 to 99");
        }

        if (track.type != TrackType::Audio && track.type != TrackType::Data)
        {
          return makeError(Error::Code::InvalidInput, "Invalid CUE TRACK type");
        }

        if (track.type == TrackType::Audio)
        {
          hasAudio = true;

          if (!track.optIndex01)
          {
            return makeError(Error::Code::InvalidInput, std::format("Audio TRACK {} requires INDEX 01", track.ordinal));
          }
        }

        if (track.optIndex00 && track.optIndex01 && *track.optIndex00 > *track.optIndex01)
        {
          return makeError(Error::Code::InvalidInput, "INDEX 00 must not follow INDEX 01");
        }

        if (track.optIndex01)
        {
          if (optPreviousIndex01 && *track.optIndex01 <= *optPreviousIndex01)
          {
            return makeError(Error::Code::InvalidInput, "INDEX 01 positions must strictly increase across TRACKs");
          }

          optPreviousIndex01 = track.optIndex01;
        }

        previousOrdinal = track.ordinal;
      }

      if (!hasAudio)
      {
        return makeError(Error::Code::InvalidInput, "CUE sheet contains no audio TRACKs");
      }

      return {};
    }

    Result<std::uint64_t> toPcmFrames(std::uint64_t const cdFrames, std::uint32_t const sampleRate)
    {
      // Splitting before multiplication accepts every representable result,
      // even when the naive cdFrames * sampleRate intermediate would overflow.
      auto const seconds = cdFrames / kCdFramesPerSecond;
      auto const tail = (cdFrames % kCdFramesPerSecond) * sampleRate / kCdFramesPerSecond;

      if (seconds > (std::numeric_limits<std::uint64_t>::max() - tail) / sampleRate)
      {
        return makeError(Error::Code::InvalidInput, "CUE position exceeds the supported PCM-frame range");
      }

      return (seconds * sampleRate) + tail;
    }

    std::size_t extensionOffset(std::string_view const name) noexcept
    {
      auto const dot = name.rfind('.');
      auto const slash = name.rfind('/');

      return dot == std::string_view::npos || dot + 1 == name.size() ||
                 (slash != std::string_view::npos && dot <= slash + 1) || dot == 0
               ? std::string_view::npos
               : dot;
    }
  } // namespace

  Result<Sheet> parseSheet(std::string_view text)
  {
    if (auto const utf8Res = utility::validateUtf8(text); !utf8Res)
    {
      return makeError(
        Error::Code::InvalidInput,
        "CUE sheet is not valid UTF-8; save the sheet as UTF-8 and scan again. " + utf8Res.error().message);
    }

    if (text.starts_with("\xEF\xBB\xBF"))
    {
      text.remove_prefix(3);
    }

    if (text.contains('\0'))
    {
      return makeError(
        Error::Code::InvalidInput, "CUE sheet contains a NUL byte; save the sheet as UTF-8 and scan again");
    }

    auto state = ParseState{};
    std::size_t lineNumber = 0;

    while (!text.empty())
    {
      ++lineNumber;
      // LF, CRLF, and a lone CR each end one line.
      auto const newline = text.find_first_of("\r\n");
      auto const line = trim(text.substr(0, newline));

      if (newline == std::string_view::npos)
      {
        text = {};
      }
      else
      {
        text.remove_prefix(newline + (text.substr(newline).starts_with("\r\n") ? 2 : 1));
      }

      if (line.empty())
      {
        continue;
      }

      if (auto const lineRes = parseLine(state, line); !lineRes)
      {
        return makeError(lineRes.error().code, std::format("CUE line {}: {}", lineNumber, lineRes.error().message));
      }
    }

    if (!state.hasFile)
    {
      return makeError(Error::Code::InvalidInput, "CUE sheet requires exactly one FILE");
    }

    if (auto const validationRes = validateSheet(state.sheet); !validationRes)
    {
      return std::unexpected{validationRes.error()};
    }

    return std::move(state.sheet);
  }

  Result<std::vector<Segment>> resolveSegments(Sheet const& sheet,
                                               std::uint32_t const sampleRate,
                                               std::uint64_t const totalFrames)
  {
    if (sampleRate == 0 || totalFrames == 0)
    {
      return makeError(Error::Code::InvalidInput, "CUE segments require a positive PCM rate and exact frame extent");
    }

    if (auto const validationRes = validateSheet(sheet); !validationRes)
    {
      return std::unexpected{validationRes.error()};
    }

    auto segments = std::vector<Segment>{};

    for (std::size_t i = 0; i < sheet.tracks.size(); ++i)
    {
      auto const& track = sheet.tracks[i];

      if (track.type != TrackType::Audio)
      {
        continue;
      }

      AO_INVARIANT(track.optIndex01, "Validated audio TRACK has INDEX 01");
      auto const startRes = toPcmFrames(*track.optIndex01, sampleRate);

      if (!startRes)
      {
        return std::unexpected{startRes.error()};
      }

      std::uint64_t endFrame = totalFrames;

      if (i + 1 < sheet.tracks.size())
      {
        auto const& next = sheet.tracks[i + 1];
        auto const optEnd = next.type == TrackType::Data && next.optIndex00 ? next.optIndex00 : next.optIndex01;

        if (!optEnd)
        {
          return makeError(
            Error::Code::InvalidInput, "A following data TRACK requires INDEX 00 or INDEX 01 to end audio");
        }

        auto const endRes = toPcmFrames(*optEnd, sampleRate);

        if (!endRes)
        {
          return std::unexpected{endRes.error()};
        }

        endFrame = std::min(totalFrames, *endRes);
      }

      if (*startRes >= endFrame ||
          (!segments.empty() && *startRes < segments.back().startFrame + segments.back().frameCount))
      {
        return makeError(Error::Code::InvalidInput,
                         std::format("CUE TRACK {} has an empty, overlapping, or out-of-extent range", track.ordinal));
      }

      segments.push_back(
        Segment{.ordinal = track.ordinal, .startFrame = *startRes, .frameCount = endFrame - *startRes});
    }

    return segments;
  }

  SheetCandidate classifySheetCandidate(std::string_view const audioName, std::string_view const sheetName) noexcept
  {
    auto const audioDot = extensionOffset(audioName);
    auto const sheetDot = extensionOffset(sheetName);

    if (audioDot == std::string_view::npos || sheetDot == std::string_view::npos ||
        !isEqualAscii(sheetName.substr(sheetDot + 1), "cue"))
    {
      return SheetCandidate::None;
    }

    auto const sheetStem = sheetName.substr(0, sheetDot);
    auto const audioStem = audioName.substr(0, audioDot);

    if (sheetStem == audioStem)
    {
      return SheetCandidate::General;
    }

    if (auto const specificDot = extensionOffset(sheetStem);
        specificDot != std::string_view::npos && sheetStem.substr(0, specificDot) == audioStem &&
        isEqualAscii(sheetStem.substr(specificDot + 1), audioName.substr(audioDot + 1)))
    {
      return SheetCandidate::ExtensionSpecific;
    }

    return SheetCandidate::None;
  }

  SheetCandidates collectSheetCandidates(std::string_view const audioName,
                                         std::span<std::string const> const sheetNames)
  {
    auto candidates = SheetCandidates{};

    for (auto const& name : sheetNames)
    {
      switch (classifySheetCandidate(audioName, name))
      {
        case SheetCandidate::None: break;
        case SheetCandidate::General: candidates.general.push_back(name); break;
        case SheetCandidate::ExtensionSpecific: candidates.extensionSpecific.push_back(name); break;
      }
    }

    for (auto* names : {&candidates.general, &candidates.extensionSpecific})
    {
      std::ranges::sort(*names);
      auto const duplicates = std::ranges::unique(*names);
      names->erase(duplicates.begin(), duplicates.end());
    }

    return candidates;
  }
} // namespace ao::media::cue
