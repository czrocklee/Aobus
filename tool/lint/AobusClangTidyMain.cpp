// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <clang-tidy/tool/ClangTidyMain.h>

int main(int argc, char const** argv)
{
  return clang::tidy::clangTidyMain(argc, argv);
}
