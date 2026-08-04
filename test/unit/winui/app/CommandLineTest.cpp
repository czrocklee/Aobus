// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include <ao/winui/app/CommandLine.h>

#include <catch2/catch_test_macros.hpp>

namespace ao::winui::test
{
  TEST_CASE("CommandLine - quotes empty and ordinary arguments", "[winui][unit][app]")
  {
    CHECK(quoteCommandLineArgument(L"") == L"\"\"");
    CHECK(quoteCommandLineArgument(L"--library-root") == L"\"--library-root\"");
    CHECK(quoteCommandLineArgument(L"C:\\Music") == L"\"C:\\Music\"");
  }

  TEST_CASE("CommandLine - preserves spaces Unicode and trailing backslashes", "[winui][unit][app]")
  {
    CHECK(quoteCommandLineArgument(L"C:\\My Music\\\u6d77\u5916") == L"\"C:\\My Music\\\u6d77\u5916\"");
    CHECK(quoteCommandLineArgument(L"C:\\Music Folder\\") == L"\"C:\\Music Folder\\\\\"");
  }

  TEST_CASE("CommandLine - escapes embedded quotes according to Windows argv rules", "[winui][unit][app]")
  {
    CHECK(quoteCommandLineArgument(L"say \"hi\"") == L"\"say \\\"hi\\\"\"");
  }
} // namespace ao::winui::test
