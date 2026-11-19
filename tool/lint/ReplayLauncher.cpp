// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

// Bazel's expanded actions require an executable artifact. Run the admitted
// interpreter at its installed path: a copied python.exe cannot locate its DLLs.
#include <cstdio>

#ifdef _WIN32
#include <windows.h>

#include <cstdint>
#include <print>
#include <string>
#include <string_view>

int main()
{
  // Bazel supplies the fully qualified interpreter path, quoted when needed.
  // Take the raw UTF-16 tail: CreateProcessW reads the interpreter name from
  // its first token and passes that same command line to the child. No shell
  // or narrow argv conversion is involved.
  auto command = std::wstring_view{::GetCommandLineW()};
  bool quoted = false;

  while (!command.empty())
  {
    auto const ch = command.front();

    if (!quoted && (ch == L' ' || ch == L'\t'))
    {
      break;
    }

    if (ch == L'"')
    {
      quoted = !quoted;
    }

    command.remove_prefix(1);
  }

  auto const first = command.find_first_not_of(L" \t");

  if (first == std::wstring_view::npos)
  {
    return 1;
  }

  auto childCommand = std::wstring{command.substr(first)};
  auto startup = STARTUPINFOW{};
  startup.cb = sizeof(startup);
  auto process = PROCESS_INFORMATION{};

  if (::CreateProcessW(nullptr, childCommand.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr, &startup, &process) ==
      0)
  {
    std::println(stderr, "Cannot start the lint replay interpreter (Win32 error {})", ::GetLastError());
    return 1;
  }

  ::CloseHandle(process.hThread);
  DWORD status = 1;

  if (::WaitForSingleObject(process.hProcess, INFINITE) != WAIT_OBJECT_0 ||
      ::GetExitCodeProcess(process.hProcess, &status) == 0)
  {
    std::println(stderr, "Cannot collect the lint replay interpreter (Win32 error {})", ::GetLastError());
    status = 1;
  }

  ::CloseHandle(process.hProcess);
  return static_cast<std::int32_t>(status);
}
#else
#include <unistd.h>

int main(int argc, char** argv)
{
  if (argc < 2)
  {
    return 1;
  }

  ::execv(argv[1], argv + 1);
  std::perror("Cannot start the lint replay interpreter");
  return 1;
}
#endif
