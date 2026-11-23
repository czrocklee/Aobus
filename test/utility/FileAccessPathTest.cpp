// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include <ao/utility/FileAccessPath.h>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>

#ifdef _WIN32
#include <ao/Error.h>

#include <catch2/catch_message.hpp>

#include <string_view>
#endif

namespace ao::utility::test
{
#ifdef _WIN32
  namespace
  {
    void checkWindowsDiskPath(std::wstring_view input, std::wstring_view verbatim, std::wstring_view ordinary)
    {
      auto const path = std::filesystem::path{input};
      INFO(path);
      auto const accessRes = fileAccessPath(path);
      INFO((accessRes ? "Windows disk path admitted" : accessRes.error().message));
      REQUIRE(accessRes);
      CHECK(accessRes->native() == verbatim);
      CHECK(ordinaryFilePath(*accessRes).native() == ordinary);
    }

    void checkRejectedWindowsPath(std::wstring_view input)
    {
      auto const path = std::filesystem::path{input};
      INFO(path);
      auto const accessRes = fileAccessPath(path);
      CHECK_FALSE(accessRes);

      if (!accessRes)
      {
        CHECK(accessRes.error().code == Error::Code::InvalidInput);
      }
    }
  } // namespace

  TEST_CASE("FileAccessPath - admits ordinary and verbatim Windows disk paths")
  {
    SECTION("drive path normalizes separators and dot segments without reserving COM10")
    {
      checkWindowsDiskPath(L"C:/music/./COM10.wav", LR"(\\?\C:\music\COM10.wav)", LR"(C:\music\COM10.wav)");
    }

    SECTION("a bare drive root stays rooted without touching the drive")
    {
      checkWindowsDiskPath(L"C:/", LR"(\\?\C:\)", LR"(C:\)");
    }

    SECTION("a share root does not require a trailing separator or a file")
    {
      checkWindowsDiskPath(LR"(\\server\share)", LR"(\\?\UNC\server\share)", LR"(\\server\share)");
    }

    SECTION("UNC path normalizes parent segments below the share")
    {
      checkWindowsDiskPath(
        LR"(\\server\share\album\..\track.wav)", LR"(\\?\UNC\server\share\track.wav)", LR"(\\server\share\track.wav)");
    }

    SECTION("a verbatim UNC disk path keeps its prefix only for I/O")
    {
      checkWindowsDiskPath(
        LR"(\\?\UNC\server\share\track.wav)", LR"(\\?\UNC\server\share\track.wav)", LR"(\\server\share\track.wav)");
      checkWindowsDiskPath(
        LR"(\\?\uNc\server\share\track.wav)", LR"(\\?\UNC\server\share\track.wav)", LR"(\\server\share\track.wav)");
    }
  }

  TEST_CASE("FileAccessPath - resolves ordinary relative names but rejects partial Windows roots")
  {
    auto const relativeRes = fileAccessPath(L"album/../track.wav");
    REQUIRE(relativeRes);
    auto const expected = (std::filesystem::current_path() / L"track.wav").make_preferred();
    CHECK(ordinaryFilePath(*relativeRes) == expected);

    checkRejectedWindowsPath(L"C:");
    checkRejectedWindowsPath(L"C:track.wav");
    checkRejectedWindowsPath(LR"(\track.wav)");
    checkRejectedWindowsPath(LR"(\\server)");
    checkRejectedWindowsPath(LR"(\\server\)");
    checkRejectedWindowsPath(LR"(\\?\UNC\server)");
    checkRejectedWindowsPath(LR"(\\?\UNC\server\\share\track.wav)");
  }

  TEST_CASE("FileAccessPath - rejects UNC traversal above a share even when its name is restored")
  {
    // Comparing only the normalized first component with the original share would accept both paths.
    checkRejectedWindowsPath(LR"(\\server\share\..\share\track.wav)");
    checkRejectedWindowsPath(LR"(\\server\share\album\..\..\share\track.wav)");
  }

  TEST_CASE("FileAccessPath - rejects Windows device names and namespaces")
  {
    checkRejectedWindowsPath(LR"(C:\music\NuL.wav)");
    checkRejectedWindowsPath(L"C:\\music\\LPT\u00B2.wav");
    checkRejectedWindowsPath(LR"(\\?\C:\music\AUX.wav)");
    checkRejectedWindowsPath(LR"(\\.\C:\music\track.wav)");
    checkRejectedWindowsPath(LR"(\\?\GLOBALROOT\Device\HarddiskVolume1\track.wav)");
    checkRejectedWindowsPath(LR"(\\?\UNC\server\pIpE\track.wav)");

    // A synthetic identifier exercises spelling admission only, not any volume lookup.
    auto const volumeRes = checkedFileSpelling(LR"(\\?\VoLuMe{00000000-0000-0000-0000-000000000000}\track.wav)");
    REQUIRE_FALSE(volumeRes);
    CHECK(volumeRes.error().code == Error::Code::InvalidInput);
    CHECK(volumeRes.error().message.contains("volume GUID"));
  }

  TEST_CASE("FileAccessPath - rejects ambiguous Windows path components and verbatim dot segments")
  {
    checkRejectedWindowsPath(LR"(C:\music.\track.wav)");
    checkRejectedWindowsPath(LR"(C:\music\track.wav )");
    checkRejectedWindowsPath(LR"(C:\music\track.wav:stream)");
    // Validate the original relative spelling before making it absolute.
    checkRejectedWindowsPath(L"track.wav.");
    checkRejectedWindowsPath(L"track.wav ");
    checkRejectedWindowsPath(LR"(\\?\C:\music\album\..\track.wav)");
    checkRejectedWindowsPath(LR"(\\?\C:\music/track.wav)");
  }
#else
  TEST_CASE("FileAccessPath - preserves POSIX spelling for filesystem resolution")
  {
    auto const path = std::filesystem::path{"music/link/../track.wav. "};
    auto const accessRes = fileAccessPath(path);
    REQUIRE(accessRes);
    CHECK(accessRes->native() == path.native());
    CHECK(ordinaryFilePath(*accessRes).native() == path.native());
  }
#endif
} // namespace ao::utility::test
