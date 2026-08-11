// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#ifdef _WIN32

#include "test/unit/TestFixtureSupport.h"
#include <ao/desktop/DetachedProcessLauncher.h>
#include <ao/utility/Path.h>
#include <ao/utility/ScopedRegistration.h>

#include <catch2/catch_test_macros.hpp>
#include <windows.h>

#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <ios>
#include <iterator>
#include <string>
#include <tuple>

namespace ao::desktop::test
{
  namespace
  {
    std::filesystem::path processProbePath()
    {
      auto buffer = std::wstring(MAX_PATH, L'\0');

      while (true)
      {
        auto const written = ::GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        REQUIRE(written != 0);

        if (written < buffer.size())
        {
          buffer.resize(written);
          return std::filesystem::path{buffer}.parent_path() / "ao_detached_process_probe.exe";
        }

        buffer.resize(buffer.size() * 2);
      }
    }
  } // namespace

  TEST_CASE("DetachedProcessLauncher - Windows preserves UTF-8 argv without inheriting unrelated handles",
            "[runtime][integration][desktop-process]")
  {
    auto const fixture = ao::test::TempDir{};
    auto const outputPath = fixture.path() / "child output.txt";
    auto const eventName = std::format(L"Local\\AobusDetachedProcessProbe-{}", ::GetCurrentProcessId());
    auto* const completionEvent = ::CreateEventW(nullptr, TRUE, FALSE, eventName.c_str());
    REQUIRE(completionEvent != nullptr);
    [[maybe_unused]] auto completionRegistration =
      utility::ScopedRegistration{[completionEvent] { std::ignore = ::CloseHandle(completionEvent); }};

    auto attributes = SECURITY_ATTRIBUTES{
      .nLength = sizeof(SECURITY_ATTRIBUTES), .lpSecurityDescriptor = nullptr, .bInheritHandle = TRUE};
    HANDLE readHandle = nullptr;
    HANDLE writeHandle = nullptr;
    REQUIRE(::CreatePipe(&readHandle, &writeHandle, &attributes, 0) != FALSE);
    REQUIRE(::SetHandleInformation(readHandle, HANDLE_FLAG_INHERIT, 0) != FALSE);
    [[maybe_unused]] auto pipeRegistration = utility::ScopedRegistration{[readHandle, writeHandle]
                                                                         {
                                                                           std::ignore = ::CloseHandle(readHandle);
                                                                           std::ignore = ::CloseHandle(writeHandle);
                                                                         }};

    auto const launch = DetachedProcessLaunch{
      .executable = processProbePath(),
      .arguments = {utility::pathToUtf8(outputPath),
                    std::format("Local\\AobusDetachedProcessProbe-{}", ::GetCurrentProcessId()),
                    std::to_string(reinterpret_cast<std::uintptr_t>(writeHandle)),
                    "space value",
                    "say \"hi\"",
                    "C:\\Music Folder\\",
                    "\xE6\xB5\xB7\xE5\xA4\x96"},
    };

    REQUIRE(launchDetachedProcess(launch));
    REQUIRE(::WaitForSingleObject(completionEvent, 5'000) == WAIT_OBJECT_0);

    auto output = std::ifstream{outputPath, std::ios::binary};
    REQUIRE(output);
    auto const content = std::string{std::istreambuf_iterator{output}, std::istreambuf_iterator<char>{}};
    CHECK(content == std::format("argc=8\n"
                                 "inherited=0\n"
                                 "1={}\n"
                                 "2=Local\\AobusDetachedProcessProbe-{}\n"
                                 "3={}\n"
                                 "4=space value\n"
                                 "5=say \"hi\"\n"
                                 "6=C:\\Music Folder\\\n"
                                 "7=\xE6\xB5\xB7\xE5\xA4\x96\n",
                                 utility::pathToUtf8(outputPath),
                                 ::GetCurrentProcessId(),
                                 reinterpret_cast<std::uintptr_t>(writeHandle)));

    DWORD available = 0;
    REQUIRE(::PeekNamedPipe(readHandle, nullptr, 0, nullptr, &available, nullptr) != FALSE);
    CHECK(available == 0);
  }
} // namespace ao::desktop::test

#endif
