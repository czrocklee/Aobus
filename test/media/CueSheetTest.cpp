// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include <ao/media/cue/Sheet.h>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ao::media::cue::test
{
  namespace
  {
    // Array extent preserves an embedded NUL; a pointer constructor would drop it.
    constexpr std::string_view cueLiteral(auto const& text) noexcept
    {
      return {std::data(text), std::size(text) - 1};
    }

    constexpr auto kFile = cueLiteral("FILE \"ignored.wav\" WAVE\n");

    // Matches quoteDiagnosticExcerpt in the parser: short tokens stay intact.
    constexpr std::size_t kDiagnosticExcerptBytes = 128;

    Sheet makeTwoTrackSheet(std::uint64_t const first, std::uint64_t const second)
    {
      return Sheet{.tracks = {Track{.ordinal = 1, .optIndex01 = first}, Track{.ordinal = 3, .optIndex01 = second}}};
    }
  } // namespace

  TEST_CASE("CUE sheet - preserves global and track metadata without applying carrier fallback")
  {
    auto const parsedRes = parseSheet(R"cue(CATALOG 0123456789012
CDTEXTFILE "album text.bin"
TITLE "Complete works"
PERFORMER "Orchestra"
SONGWRITER "Composer"
REM GENRE Classical
REM DATE 2026-10-18
REM DISCNUMBER 2
REM TOTALDISCS 3
REM COMMENT "Unknown comments may contain unmatched quotes
REM
REM "Even the comment's opening quote is free text
FILE "C:\old rip\disc.wav" WAVE
  TRACK 01 AUDIO
    TITLE "Ouverture 日本語"
    PERFORMER Soloist
    SONGWRITER "Track composer"
    REM GENRE Chamber music
    REM DATE 1901
    REM DISCNUMBER 1
    REM TOTALDISCS 2
    ISRC USABC2600001
    FLAGS DCP PRE 4CH SCMS
    INDEX 01 00:00:00
  TRACK 03 AUDIO
    TITLE Finale without quotes
    INDEX 01 01:02:03
)cue");

    REQUIRE(parsedRes);
    CHECK(parsedRes->metadata.optTitle == "Complete works");
    CHECK(parsedRes->metadata.optPerformer == "Orchestra");
    CHECK(parsedRes->metadata.optSongwriter == "Composer");
    CHECK(parsedRes->metadata.optGenre == "Classical");
    CHECK(parsedRes->metadata.optDate == "2026-10-18");
    CHECK(parsedRes->metadata.optDiscNumber == "2");
    CHECK(parsedRes->metadata.optTotalDiscs == "3");
    REQUIRE(parsedRes->tracks.size() == 2);
    auto const& first = parsedRes->tracks[0];
    CHECK(first.ordinal == 1);
    CHECK(first.type == TrackType::Audio);
    CHECK(first.optIndex01 == 0);
    CHECK(first.metadata.optTitle == "Ouverture 日本語");
    CHECK(first.metadata.optPerformer == "Soloist");
    CHECK(first.metadata.optSongwriter == "Track composer");
    CHECK(first.metadata.optGenre == "Chamber music");
    CHECK(first.metadata.optDate == "1901");
    CHECK(first.metadata.optDiscNumber == "1");
    CHECK(first.metadata.optTotalDiscs == "2");
    auto const& second = parsedRes->tracks[1];
    CHECK(second.ordinal == 3);
    CHECK(second.optIndex01 == 4653);
    CHECK(second.metadata.optTitle == "Finale without quotes");
    CHECK_FALSE(second.metadata.optPerformer);
    CHECK_FALSE(second.metadata.optSongwriter);
    CHECK_FALSE(second.metadata.optGenre);
    CHECK_FALSE(second.metadata.optDate);
    CHECK_FALSE(second.metadata.optDiscNumber);
    CHECK_FALSE(second.metadata.optTotalDiscs);
  }

  TEST_CASE("CUE sheet - accepts UTF-8 BOM CRLF tabs and unquoted tokens")
  {
    auto const parsedRes = parseSheet("\xEF\xBB\xBF\tTITLE \"Été\"\r\n\r\nfile disc.flac WAVE\r\n"
                                      "\ttrack 01 audio\r\n\tTITLE \"\"\r\n\tindex 01 00:00:00");

    REQUIRE(parsedRes);
    CHECK(parsedRes->metadata.optTitle == "Été");
    REQUIRE(parsedRes->tracks.size() == 1);
    CHECK(parsedRes->tracks[0].metadata.optTitle == "");
    CHECK(parsedRes->tracks[0].optIndex01 == 0);
  }

  TEST_CASE("CUE sheet - a lone CR ends a line like LF and CRLF")
  {
    auto const parsedRes = parseSheet("TITLE \"Old Mac\"\rFILE disc.wav WAVE\rTRACK 01 AUDIO\r\nINDEX 01 00:00:00\n"
                                      "TRACK 02 AUDIO\rINDEX 01 00:01:00\r");
    REQUIRE(parsedRes);
    CHECK(parsedRes->metadata.optTitle == "Old Mac");
    REQUIRE(parsedRes->tracks.size() == 2);
    CHECK(parsedRes->tracks[1].optIndex01 == 75);

    // CRLF is one line ending, so diagnostics count the same lines in every style.
    auto const lineRes = parseSheet(std::string{kFile} + "\r\n\rTRACK 01 AUDIO\r\nUNKNOWN\r");
    REQUIRE_FALSE(lineRes);
    CHECK(lineRes.error().message == "CUE line 5: Unknown CUE directive 'UNKNOWN'");
  }

  TEST_CASE("CUE sheet - recognized metadata repeats within one scope only with the same value")
  {
    for (auto const* const repeated : {"TITLE \"One\"\nTITLE \"Two\"\n", "REM DATE 2001\nrem date 2002\n"})
    {
      auto const sheetRes =
        parseSheet(std::string{repeated} + std::string{kFile} + "TRACK 01 AUDIO\nINDEX 01 00:00:00\n");
      REQUIRE_FALSE(sheetRes);
      CHECK(sheetRes.error().message == "CUE line 2: Metadata is repeated with a different value within one CUE scope");
      auto const trackRes = parseSheet(std::string{kFile} + "TRACK 01 AUDIO\n" + repeated + "INDEX 01 00:00:00\n");
      REQUIRE_FALSE(trackRes);
      CHECK(trackRes.error().message == "CUE line 4: Metadata is repeated with a different value within one CUE scope");
    }

    // Quoting does not change a value, so these copies agree.
    auto const identicalRes =
      parseSheet("TITLE \"Album\"\nTITLE Album\nREM DATE 2001\nrem date \"2001\"\n" + std::string{kFile} +
                 "TRACK 01 AUDIO\nPERFORMER Solo\nPERFORMER Solo\n"
                 "INDEX 01 00:00:00\n");
    REQUIRE(identicalRes);
    CHECK(identicalRes->metadata.optTitle == "Album");
    CHECK(identicalRes->metadata.optDate == "2001");
    CHECK(identicalRes->tracks[0].metadata.optPerformer == "Solo");

    // The sheet and each track are separate scopes, and comments may repeat.
    auto const scopedRes = parseSheet("TITLE Album\nREM COMMENT one\nREM COMMENT two\n" + std::string{kFile} +
                                      "TRACK 01 AUDIO\nTITLE First\nINDEX 01 00:00:00\n"
                                      "TRACK 02 AUDIO\nTITLE Second\nINDEX 01 00:01:00\n");
    REQUIRE(scopedRes);
    CHECK(scopedRes->metadata.optTitle == "Album");
    CHECK(scopedRes->tracks[1].metadata.optTitle == "Second");
  }

  TEST_CASE("CUE sheet - diagnostics escape control bytes from the sheet")
  {
    auto const parsedRes = parseSheet("UNKNOWN\x1B[2J\x7F\n" + std::string{kFile});
    REQUIRE_FALSE(parsedRes);
    CHECK(parsedRes.error().message == "CUE line 1: Unknown CUE directive 'UNKNOWN\\x1B[2J\\x7F'");
  }

  TEST_CASE("CUE sheet - invalid UTF-8 requests an explicit UTF-8 save")
  {
    for (auto const invalid : {cueLiteral("\xFF"),
                               cueLiteral("\xC0\xAF"),
                               cueLiteral("\xED\xA0\x80"),
                               cueLiteral("\xF4\x90\x80\x80"),
                               cueLiteral("\xE3\x81"),
                               cueLiteral("\xFF\xFE"),
                               cueLiteral("\0")})
    {
      INFO("invalid byte count: " << invalid.size());
      auto const parsedRes =
        parseSheet(std::string{kFile} + "TRACK 01 AUDIO\nINDEX 01 00:00:00\nREM NOTE " + std::string{invalid});

      REQUIRE_FALSE(parsedRes);
      CHECK(parsedRes.error().code == Error::Code::InvalidInput);
      CHECK(parsedRes.error().message.contains("save the sheet as UTF-8"));
    }
  }

  TEST_CASE("CUE sheet - exactly one FILE must precede every TRACK")
  {
    for (auto const text : {cueLiteral(""),
                            cueLiteral("TITLE Album\n"),
                            cueLiteral("TRACK 01 AUDIO\nINDEX 01 00:00:00"),
                            cueLiteral("FILE a.wav WAVE\nFILE b.wav WAVE\nTRACK 01 AUDIO\nINDEX 01 00:00:00"),
                            cueLiteral("FILE a.wav WAVE\nTRACK 01 AUDIO\nINDEX 01 00:00:00\nFILE b.wav WAVE\n"
                                       "TRACK 02 AUDIO\nINDEX 01 00:01:00")})
    {
      INFO(text);
      auto const parsedRes = parseSheet(text);
      REQUIRE_FALSE(parsedRes);
      CHECK(parsedRes.error().code == Error::Code::InvalidInput);
    }
  }

  TEST_CASE("CUE sheet - no audio is malformed but one audio remains a valid unbound layout")
  {
    auto const emptyRes = parseSheet(kFile);
    REQUIRE_FALSE(emptyRes);
    CHECK(emptyRes.error().message.contains("no audio"));
    auto const dataRes = parseSheet(std::string{kFile} + "TRACK 01 MODE1/2352\nINDEX 01 00:00:00");
    REQUIRE_FALSE(dataRes);
    CHECK(dataRes.error().message.contains("no audio"));

    auto const singleRes = parseSheet(std::string{kFile} + "TRACK 09 AUDIO\nINDEX 01 00:00:00");
    REQUIRE(singleRes);
    REQUIRE(singleRes->tracks.size() == 1);
    CHECK(singleRes->tracks[0].ordinal == 9);
  }

  TEST_CASE("CUE sheet - TRACK ordinals strictly increase within 1 to 99")
  {
    for (auto const tracks : {cueLiteral("TRACK 00 AUDIO\nINDEX 01 00:00:00"),
                              cueLiteral("TRACK 100 AUDIO\nINDEX 01 00:00:00"),
                              cueLiteral("TRACK -1 AUDIO\nINDEX 01 00:00:00"),
                              cueLiteral("TRACK 01 AUDIO\nINDEX 01 00:00:00\nTRACK 01 AUDIO\nINDEX 01 00:01:00"),
                              cueLiteral("TRACK 03 AUDIO\nINDEX 01 00:00:00\nTRACK 02 AUDIO\nINDEX 01 00:01:00"),
                              cueLiteral("TRACK 18446744073709551616 AUDIO\nINDEX 01 00:00:00")})
    {
      INFO(tracks);
      auto const parsedRes = parseSheet(std::string{kFile} + std::string{tracks});
      REQUIRE_FALSE(parsedRes);
      CHECK(parsedRes.error().code == Error::Code::InvalidInput);
    }
  }

  TEST_CASE("CUE sheet - indexes require a track and ordered numbers and positions")
  {
    for (auto const tracks : {cueLiteral("INDEX 01 00:00:00"),
                              cueLiteral("TRACK 01 AUDIO"),
                              cueLiteral("TRACK 01 AUDIO\nINDEX 00 00:00:00"),
                              cueLiteral("TRACK 01 AUDIO\nINDEX 01 00:00:00\nINDEX 01 00:01:00"),
                              cueLiteral("TRACK 01 AUDIO\nINDEX 01 00:00:00\nINDEX 00 00:01:00"),
                              cueLiteral("TRACK 01 AUDIO\nINDEX 00 00:01:00\nINDEX 01 00:00:00"),
                              cueLiteral("TRACK 01 AUDIO\nINDEX 01 00:01:00\nINDEX 02 00:00:00"),
                              cueLiteral("TRACK 01 AUDIO\nINDEX 01 00:00:00\nINDEX 100 00:01:00"),
                              cueLiteral("TRACK 01 AUDIO\nINDEX 01 00:01:00\nTRACK 02 AUDIO\nINDEX 01 00:01:00"),
                              cueLiteral("TRACK 01 AUDIO\nINDEX 01 00:02:00\nTRACK 02 MODE1/2352\nINDEX 01 00:01:00")})
    {
      INFO(tracks);
      auto const parsedRes = parseSheet(std::string{kFile} + std::string{tracks});
      REQUIRE_FALSE(parsedRes);
      CHECK(parsedRes.error().code == Error::Code::InvalidInput);
    }
  }

  TEST_CASE("CUE sheet - standard data track and FILE types remain valid syntax")
  {
    for (auto const type : {cueLiteral("CDG"),
                            cueLiteral("MODE1/2048"),
                            cueLiteral("MODE1/2352"),
                            cueLiteral("MODE2/2048"),
                            cueLiteral("MODE2/2324"),
                            cueLiteral("MODE2/2336"),
                            cueLiteral("MODE2/2352"),
                            cueLiteral("CDI/2336"),
                            cueLiteral("CDI/2352")})
    {
      INFO(type);
      auto const parsedRes = parseSheet(std::string{kFile} + "TRACK 01 AUDIO\nINDEX 01 00:00:00\nTRACK 99 " +
                                        std::string{type} + "\nINDEX 01 00:01:00");
      REQUIRE(parsedRes);
      REQUIRE(parsedRes->tracks.size() == 2);
      CHECK(parsedRes->tracks[1].ordinal == 99);
      CHECK(parsedRes->tracks[1].type == TrackType::Data);
    }

    for (auto const type : {cueLiteral("WAVE"),
                            cueLiteral("BINARY"),
                            cueLiteral("MOTOROLA"),
                            cueLiteral("AIFF"),
                            cueLiteral("MP3"),
                            cueLiteral("FLAC")})
    {
      INFO(type);
      auto const parsedRes =
        parseSheet("FILE unrelated.ext " + std::string{type} + "\nTRACK 01 AUDIO\nINDEX 01 00:00:00");
      REQUIRE(parsedRes);
      REQUIRE(parsedRes->tracks.size() == 1);
      CHECK(parsedRes->tracks[0].type == TrackType::Audio);
    }
  }

  TEST_CASE("CUE sheet - index subtracks are accepted without moving audio boundaries")
  {
    auto const parsedRes = parseSheet(std::string{kFile} + "TRACK 01 AUDIO\nINDEX 00 00:00:00\n"
                                                           "INDEX 01 00:00:01\nINDEX 02 00:00:02\nINDEX 99 00:00:03");
    REQUIRE(parsedRes);
    REQUIRE(parsedRes->tracks.size() == 1);
    CHECK(parsedRes->tracks[0].optIndex00 == 0);
    CHECK(parsedRes->tracks[0].optIndex01 == 1);
  }

  TEST_CASE("CUE sheet - malformed directives and tokens fail with line context")
  {
    for (auto const line : {cueLiteral("FILE \"unterminated WAVE"),
                            cueLiteral("FILE \"a.wav\"WAVE"),
                            cueLiteral("FILE a.wav UNKNOWN"),
                            cueLiteral("FILE a.wav WAVE extra"),
                            cueLiteral("FILE \"\" WAVE"),
                            cueLiteral("TITLE \"quoted\" extra"),
                            cueLiteral("TITLE bad\"quote"),
                            cueLiteral("TITLE"),
                            cueLiteral("REM DATE"),
                            cueLiteral("UNKNOWN ignored"),
                            cueLiteral("TRACK 01 UNKNOWN"),
                            cueLiteral("TRACK 01 AUDIO extra"),
                            cueLiteral("PREGAP 00:00:00"),
                            cueLiteral("POSTGAP 00:00:00"),
                            cueLiteral("ISRC abc"),
                            cueLiteral("FLAGS DCP")})
    {
      INFO(line);
      auto const parsedRes =
        parseSheet(std::string{line} + "\n" + std::string{kFile} + "TRACK 01 AUDIO\nINDEX 01 00:00:00");
      REQUIRE_FALSE(parsedRes);
      CHECK(parsedRes.error().code == Error::Code::InvalidInput);
      CHECK(parsedRes.error().message.contains("CUE line"));
    }
  }

  TEST_CASE("CUE sheet - short unknown directive diagnostic stays unchanged")
  {
    auto const parsedRes = parseSheet("UNKNOWN ignored\n" + std::string{kFile} + "TRACK 01 AUDIO\nINDEX 01 00:00:00\n");
    REQUIRE_FALSE(parsedRes);
    CHECK(parsedRes.error().code == Error::Code::InvalidInput);
    CHECK(parsedRes.error().message == "CUE line 1: Unknown CUE directive 'UNKNOWN'");

    auto const laterRes = parseSheet(std::string{kFile} + "not-a-command\n");
    REQUIRE_FALSE(laterRes);
    CHECK(laterRes.error().message == "CUE line 2: Unknown CUE directive 'not-a-command'");
  }

  TEST_CASE("CUE sheet - unknown directive diagnostic quotes at most 128 bytes")
  {
    auto const exactRes = parseSheet(std::string(kDiagnosticExcerptBytes, 'Q'));
    REQUIRE_FALSE(exactRes);
    CHECK(exactRes.error().message ==
          "CUE line 1: Unknown CUE directive '" + std::string(kDiagnosticExcerptBytes, 'Q') + "'");
    CHECK_FALSE(exactRes.error().message.contains("... ("));

    auto const overRes = parseSheet(std::string(kDiagnosticExcerptBytes + 1, 'Q'));
    REQUIRE_FALSE(overRes);
    CHECK(overRes.error().message ==
          "CUE line 1: Unknown CUE directive '" + std::string(kDiagnosticExcerptBytes, 'Q') + "'... (129 bytes)");

    constexpr std::size_t kHugeBytes = 2'097'152;
    auto const hugeRes = parseSheet(std::string(kHugeBytes, 'Q'));
    REQUIRE_FALSE(hugeRes);
    CHECK(hugeRes.error().code == Error::Code::InvalidInput);
    CHECK(hugeRes.error().message ==
          "CUE line 1: Unknown CUE directive '" + std::string(kDiagnosticExcerptBytes, 'Q') + "'... (2097152 bytes)");
    CHECK(hugeRes.error().message.size() < 256);
    CHECK_FALSE(hugeRes.error().message.contains(std::string(kDiagnosticExcerptBytes + 1, 'Q')));

    auto const quotedRes = parseSheet("\"" + std::string(200, 'Q') + "\"");
    REQUIRE_FALSE(quotedRes);
    CHECK(quotedRes.error().message ==
          "CUE line 1: Unknown CUE directive '" + std::string(kDiagnosticExcerptBytes, 'Q') + "'... (200 bytes)");
  }

  TEST_CASE("CUE sheet - unknown directive diagnostic cuts Unicode on a code-point boundary")
  {
    auto const grin = std::string{"\U0001F600"};
    REQUIRE(grin.size() == 4);
    auto misaligned = std::string{"A"};

    for (std::size_t i = 0; i < 32; ++i)
    {
      misaligned += grin;
    }

    auto const misalignedRes = parseSheet(misaligned);
    REQUIRE_FALSE(misalignedRes);
    auto expectedMisaligned = std::string{"CUE line 1: Unknown CUE directive 'A"};

    for (std::size_t i = 0; i < 31; ++i)
    {
      expectedMisaligned += grin;
    }

    expectedMisaligned += "'... (129 bytes)";
    CHECK(misalignedRes.error().message == expectedMisaligned);

    auto const eAcute = std::string{"\u00E9"};
    REQUIRE(eAcute.size() == 2);
    auto aligned = std::string{};
    aligned.reserve(65 * eAcute.size());

    for (std::size_t i = 0; i < 65; ++i)
    {
      aligned += eAcute;
    }

    auto const alignedRes = parseSheet(aligned);
    REQUIRE_FALSE(alignedRes);
    auto expectedAligned = std::string{"CUE line 1: Unknown CUE directive '"};

    for (std::size_t i = 0; i < 64; ++i)
    {
      expectedAligned += eAcute;
    }

    expectedAligned += "'... (130 bytes)";
    CHECK(alignedRes.error().message == expectedAligned);
  }

  TEST_CASE("CUE sheet - other diagnostics do not echo an oversized token")
  {
    auto const token = std::string(300, 'Z');
    auto const samples = std::to_array<std::pair<std::string, std::string_view>>({
      {std::string{kFile} + "TRACK 01 " + token, "CUE line 2: Unknown CUE TRACK type"},
      {std::string{kFile} + "TRACK 01 AUDIO\nFLAGS " + token, "CUE line 3: Unknown CUE flag"},
      {std::string{kFile} + "FLAGS " + token, "CUE line 2: FLAGS requires a TRACK and at least one flag"},
      {"FILE name " + token, "CUE line 1: Expected FILE filename followed by a supported CUE file type"},
      {std::string{kFile} + "TRACK " + token + " AUDIO",
       "CUE line 2: TRACK numbers must strictly increase from 1 to 99"},
      {std::string{kFile} + "TRACK 01 AUDIO\nINDEX 01 " + token,
       "CUE line 3: Expected CUE time mm:ss:ff with ss < 60 and ff < 75"},
      {"\"" + token, "CUE line 1: Unterminated or incorrectly delimited quoted CUE token"},
      {"ISRC " + token, "CUE line 1: CUE directive appears at the wrong sheet or TRACK scope"},
    });

    for (auto const& [sheet, message] : samples)
    {
      INFO(message);
      auto const parsedRes = parseSheet(sheet);
      REQUIRE_FALSE(parsedRes);
      CHECK(parsedRes.error().code == Error::Code::InvalidInput);
      CHECK(parsedRes.error().message == message);
      CHECK_FALSE(parsedRes.error().message.contains(token));
    }
  }

  TEST_CASE("CUE sheet - malformed track directives reject rather than being silently ignored")
  {
    for (auto const directive : {cueLiteral("INDEX 01"),
                                 cueLiteral("INDEX 01 00:00:00 extra"),
                                 cueLiteral("INDEX -1 00:00:00"),
                                 cueLiteral("PREGAP 00:60:00"),
                                 cueLiteral("PREGAP 00:00:00\nPREGAP 00:00:01"),
                                 cueLiteral("POSTGAP 00:00:75"),
                                 cueLiteral("POSTGAP 00:00:00\nPOSTGAP 00:00:01"),
                                 cueLiteral("FLAGS"),
                                 cueLiteral("FLAGS UNKNOWN"),
                                 cueLiteral("ISRC value extra"),
                                 cueLiteral("CATALOG 0123456789012"),
                                 cueLiteral("CDTEXTFILE text.bin"),
                                 cueLiteral("TRACK 02 UNKNOWN"),
                                 cueLiteral("TRACK 02 AUDIO extra")})
    {
      INFO(directive);
      auto const parsedRes =
        parseSheet(std::string{kFile} + "TRACK 01 AUDIO\n" + std::string{directive} + "\nINDEX 01 00:01:00");
      REQUIRE_FALSE(parsedRes);
      CHECK(parsedRes.error().code == Error::Code::InvalidInput);
      CHECK(parsedRes.error().message.contains("CUE line"));
    }
  }

  TEST_CASE("CUE sheet - timestamp arithmetic rejects invalid fields and overflow")
  {
    for (auto const time : {cueLiteral("00:60:00"),
                            cueLiteral("00:00:75"),
                            cueLiteral("-1:00:00"),
                            cueLiteral("00:0:00"),
                            cueLiteral("00:00:0"),
                            cueLiteral("00:00:00:00"),
                            cueLiteral("00:aa:00"),
                            cueLiteral("18446744073709551616:00:00"),
                            cueLiteral("4099276460824344:48:16"),
                            cueLiteral("4099276460824345:00:00")})
    {
      INFO(time);
      auto const parsedRes = parseSheet(std::string{kFile} + "TRACK 01 AUDIO\nINDEX 01 " + std::string{time});
      REQUIRE_FALSE(parsedRes);
      CHECK(parsedRes.error().code == Error::Code::InvalidInput);
    }

    auto const maximumRes = parseSheet(std::string{kFile} + "TRACK 99 AUDIO\nINDEX 01 4099276460824344:48:15");
    REQUIRE(maximumRes);
    REQUIRE(maximumRes->tracks.size() == 1);
    CHECK(maximumRes->tracks[0].optIndex01 == std::numeric_limits<std::uint64_t>::max());
  }

  TEST_CASE("CUE segments - pregaps belong to the previous audio and synthetic silence moves no boundary")
  {
    auto const parsedRes = parseSheet(std::string{kFile} + R"cue(TRACK 01 AUDIO
PREGAP 00:02:00
INDEX 00 00:00:00
INDEX 01 00:02:00
POSTGAP 00:01:00
TRACK 02 AUDIO
PREGAP 00:03:00
INDEX 00 00:08:00
INDEX 01 00:10:00
POSTGAP 00:04:00
)cue");
    REQUIRE(parsedRes);
    auto const segmentsRes = resolveSegments(*parsedRes, 44100, 882000);
    REQUIRE(segmentsRes);
    REQUIRE(segmentsRes->size() == 2);
    CHECK((*segmentsRes)[0].ordinal == 1);
    CHECK((*segmentsRes)[0].startFrame == 88200);
    CHECK((*segmentsRes)[0].frameCount == 352800);
    CHECK((*segmentsRes)[1].ordinal == 2);
    CHECK((*segmentsRes)[1].startFrame == 441000);
    CHECK((*segmentsRes)[1].frameCount == 441000);
  }

  TEST_CASE("CUE segments - data tracks end audio at INDEX 00 else INDEX 01")
  {
    for (auto const dataIndex : {cueLiteral("INDEX 00 00:03:00\nINDEX 01 00:04:00"),
                                 cueLiteral("INDEX 01 00:03:00"),
                                 cueLiteral("INDEX 00 00:03:00")})
    {
      INFO(dataIndex);
      auto const parsedRes = parseSheet(std::string{kFile} +
                                        "TRACK 01 AUDIO\nINDEX 01 00:00:00\n"
                                        "TRACK 02 MODE1/2352\n" +
                                        std::string{dataIndex} + "\nTRACK 03 AUDIO\nINDEX 01 00:05:00");
      REQUIRE(parsedRes);
      REQUIRE(parsedRes->tracks.size() == 3);
      CHECK(parsedRes->tracks[1].type == TrackType::Data);
      auto const segmentsRes = resolveSegments(*parsedRes, 48000, 480000);
      REQUIRE(segmentsRes);
      REQUIRE(segmentsRes->size() == 2);
      CHECK((*segmentsRes)[0].ordinal == 1);
      CHECK((*segmentsRes)[0].startFrame == 0);
      CHECK((*segmentsRes)[0].frameCount == 144000);
      CHECK((*segmentsRes)[1].ordinal == 3);
      CHECK((*segmentsRes)[1].startFrame == 240000);
      CHECK((*segmentsRes)[1].frameCount == 240000);
    }
  }

  TEST_CASE("CUE segments - final data boundary clamps to the authoritative extent")
  {
    auto const parsedRes = parseSheet(std::string{kFile} + "TRACK 01 MODE1/2048\nINDEX 01 00:00:00\n"
                                                           "TRACK 02 AUDIO\nINDEX 01 00:02:00\n"
                                                           "TRACK 03 MODE2/2352\nINDEX 00 00:10:00\nINDEX 01 00:11:00");
    REQUIRE(parsedRes);
    auto const segmentsRes = resolveSegments(*parsedRes, 44100, 200000);
    REQUIRE(segmentsRes);
    REQUIRE(segmentsRes->size() == 1);
    CHECK((*segmentsRes)[0].ordinal == 2);
    CHECK((*segmentsRes)[0].startFrame == 88200);
    CHECK((*segmentsRes)[0].frameCount == 111800);
  }

  TEST_CASE("CUE segments - common PCM rates convert CD frames exactly")
  {
    auto const sheet = makeTwoTrackSheet(1, 2);

    for (auto const [rate, framesPerCdFrame] : std::to_array<std::array<std::uint32_t, 2>>(
           {{44100, 588}, {48000, 640}, {88200, 1176}, {96000, 1280}, {176400, 2352}, {192000, 2560}}))
    {
      INFO(rate);
      auto const segmentsRes = resolveSegments(sheet, rate, 10000);
      REQUIRE(segmentsRes);
      REQUIRE(segmentsRes->size() == 2);
      CHECK((*segmentsRes)[0].ordinal == 1);
      CHECK((*segmentsRes)[0].startFrame == framesPerCdFrame);
      CHECK((*segmentsRes)[0].frameCount == framesPerCdFrame);
      CHECK((*segmentsRes)[1].ordinal == 3);
      CHECK((*segmentsRes)[1].startFrame == static_cast<std::uint64_t>(2) * framesPerCdFrame);
      CHECK((*segmentsRes)[1].frameCount == 10000 - (2 * framesPerCdFrame));
    }
  }

  TEST_CASE("CUE segments - nonexact rates floor absolute positions before subtracting")
  {
    auto const segmentsRes = resolveSegments(makeTwoTrackSheet(1, 2), 1000, 100);
    REQUIRE(segmentsRes);
    REQUIRE(segmentsRes->size() == 2);
    CHECK((*segmentsRes)[0].startFrame == 13);
    CHECK((*segmentsRes)[0].frameCount == 13);
    CHECK((*segmentsRes)[1].startFrame == 26);
    CHECK((*segmentsRes)[1].frameCount == 74);

    auto const remainderRes = resolveSegments(makeTwoTrackSheet(2, 3), 1000, 100);
    REQUIRE(remainderRes);
    REQUIRE(remainderRes->size() == 2);
    CHECK((*remainderRes)[0].startFrame == 26);
    CHECK((*remainderRes)[0].frameCount == 14);
  }

  TEST_CASE("CUE segments - multiplication overflow is avoided when the final frame position fits")
  {
    constexpr auto kMaximum = std::numeric_limits<std::uint64_t>::max();
    auto const sheet = makeTwoTrackSheet(0, kMaximum - 1);
    auto const segmentsRes = resolveSegments(sheet, 75, kMaximum);
    REQUIRE(segmentsRes);
    REQUIRE(segmentsRes->size() == 2);
    CHECK((*segmentsRes)[0].startFrame == 0);
    CHECK((*segmentsRes)[0].frameCount == kMaximum - 1);
    CHECK((*segmentsRes)[1].startFrame == kMaximum - 1);
    CHECK((*segmentsRes)[1].frameCount == 1);

    auto const overflowRes = resolveSegments(sheet, 76, kMaximum);
    REQUIRE_FALSE(overflowRes);
    CHECK(overflowRes.error().code == Error::Code::InvalidInput);
    CHECK(overflowRes.error().message.contains("PCM-frame range"));
  }

  TEST_CASE("CUE segments - zero rate extent empty rounding and out-of-extent starts are rejected")
  {
    auto const sheet = makeTwoTrackSheet(0, 75);

    for (auto const [rate, extent] :
         std::to_array<std::array<std::uint32_t, 2>>({{0, 100}, {1000, 0}, {1000, 1000}, {1000, 999}}))
    {
      INFO(rate << ", " << extent);
      auto const segmentsRes = resolveSegments(sheet, rate, extent);
      REQUIRE_FALSE(segmentsRes);
      CHECK(segmentsRes.error().code == Error::Code::InvalidInput);
    }

    auto const roundedRes = resolveSegments(makeTwoTrackSheet(0, 1), 1, 100);
    REQUIRE_FALSE(roundedRes);
    CHECK(roundedRes.error().message.contains("empty"));
  }

  TEST_CASE("CUE segments - public aggregates are revalidated and unusable data boundaries fail")
  {
    auto const valid = makeTwoTrackSheet(0, 75);

    SECTION("Ordinal ordering is not trusted")
    {
      auto sheet = valid;
      sheet.tracks[1].ordinal = 1;
      auto const segmentsRes = resolveSegments(sheet, 44100, 100000);
      REQUIRE_FALSE(segmentsRes);
      CHECK(segmentsRes.error().message.contains("TRACK numbers"));
    }

    SECTION("Audio requires INDEX 01")
    {
      auto sheet = valid;
      sheet.tracks[1].optIndex01.reset();
      auto const segmentsRes = resolveSegments(sheet, 44100, 100000);
      REQUIRE_FALSE(segmentsRes);
      CHECK(segmentsRes.error().message.contains("requires INDEX 01"));
    }

    SECTION("INDEX 01 positions are not trusted")
    {
      auto sheet = valid;
      sheet.tracks[1].optIndex01 = 0;
      auto const segmentsRes = resolveSegments(sheet, 44100, 100000);
      REQUIRE_FALSE(segmentsRes);
      CHECK(segmentsRes.error().message.contains("strictly increase"));
    }

    SECTION("Data pregap cannot end audio before its start")
    {
      auto sheet = valid;
      sheet.tracks[1].type = TrackType::Data;
      sheet.tracks[1].optIndex00 = 0;
      auto const segmentsRes = resolveSegments(sheet, 44100, 100000);
      REQUIRE_FALSE(segmentsRes);
      CHECK(segmentsRes.error().message.contains("empty"));
    }

    SECTION("Data without positions cannot determine an audio end")
    {
      auto sheet = valid;
      sheet.tracks[1].type = TrackType::Data;
      sheet.tracks[1].optIndex01.reset();
      auto const segmentsRes = resolveSegments(sheet, 44100, 100000);
      REQUIRE_FALSE(segmentsRes);
      CHECK(segmentsRes.error().message.contains("following data TRACK"));
    }

    SECTION("INDEX 00 cannot follow INDEX 01")
    {
      auto sheet = valid;
      sheet.tracks[1].optIndex00 = 76;
      auto const segmentsRes = resolveSegments(sheet, 44100, 100000);
      REQUIRE_FALSE(segmentsRes);
      CHECK(segmentsRes.error().message.contains("INDEX 00"));
    }
  }

  TEST_CASE("CUE binding - normalized stems are exact and only extensions ignore ASCII case")
  {
    CHECK(classifySheetCandidate("file:///music/Été.FLAC", "file:///music/Été.CuE") == SheetCandidate::General);
    CHECK(classifySheetCandidate("file:///music/Été.FLAC", "file:///music/Été.flac.cue") ==
          SheetCandidate::ExtensionSpecific);
    CHECK(classifySheetCandidate("file:///music/Été.flac", "file:///music/Été.FLAC.CUE") ==
          SheetCandidate::ExtensionSpecific);
    CHECK(classifySheetCandidate("X.wav", "X.cue") == SheetCandidate::General);
    CHECK(classifySheetCandidate("X.flac", "X.cue") == SheetCandidate::General);
    CHECK(classifySheetCandidate("X.wav", "X.flac.cue") == SheetCandidate::None);
    CHECK(classifySheetCandidate("x.flac", "X.cue") == SheetCandidate::None);
    CHECK(classifySheetCandidate("Été.flac", "été.cue") == SheetCandidate::None);
    CHECK(classifySheetCandidate("dir/X.flac", "other/X.cue") == SheetCandidate::None);
    CHECK(classifySheetCandidate("dir/X.flac", "Dir/X.cue") == SheetCandidate::None);
    CHECK(classifySheetCandidate("X.flac", "X.cue.bak") == SheetCandidate::None);
    CHECK(classifySheetCandidate("X.flac", "Y.cue") == SheetCandidate::None);
    CHECK(classifySheetCandidate("X", "X.cue") == SheetCandidate::None);
    CHECK(classifySheetCandidate("X.", "X.cue") == SheetCandidate::None);
    CHECK(classifySheetCandidate("dir.cue/X", "dir.cue/X.cue") == SheetCandidate::None);
    CHECK(classifySheetCandidate("X.flac", "X") == SheetCandidate::None);
    CHECK(classifySheetCandidate("X.flac.wav", "X.flac.cue") == SheetCandidate::General);
    CHECK(classifySheetCandidate("X.FLAC.wav", "X.flac.cue") == SheetCandidate::None);
    CHECK(classifySheetCandidate(".hidden.flac", ".hidden.CUE") == SheetCandidate::General);
  }

  TEST_CASE("CUE binding - candidate sets preserve competing case spellings at both precedences")
  {
    auto const names = std::vector<std::string>{
      "X.flac.cue", "Y.cue", "X.CUE", "X.FLAC.cue", "X.cue", "X.flac.cue", "X.wav.cue", "x.FLAC.cue"};
    auto const candidates = collectSheetCandidates("X.flac", names);
    REQUIRE(candidates.general.size() == 2);
    CHECK(candidates.general[0] == "X.CUE");
    CHECK(candidates.general[1] == "X.cue");
    REQUIRE(candidates.extensionSpecific.size() == 2);
    CHECK(candidates.extensionSpecific[0] == "X.FLAC.cue");
    CHECK(candidates.extensionSpecific[1] == "X.flac.cue");

    auto const wavCandidates = collectSheetCandidates("X.WAV", names);
    REQUIRE(wavCandidates.general.size() == 2);
    REQUIRE(wavCandidates.extensionSpecific.size() == 1);
    CHECK(wavCandidates.extensionSpecific[0] == "X.wav.cue");

    auto const noCandidates = collectSheetCandidates("Z.flac", names);
    CHECK(noCandidates.general.empty());
    CHECK(noCandidates.extensionSpecific.empty());
  }
} // namespace ao::media::cue::test
