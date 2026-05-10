// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include <cxxabi.h>

#include <cstdlib>
#include <memory>
#include <source_location>
#include <string>
#include <string_view>
#include <typeinfo>

namespace ao::app::detail
{
  inline std::string demangle(char const* mangled)
  {
    int status = 0;
    std::unique_ptr<char, void (*)(void*)> result{abi::__cxa_demangle(mangled, nullptr, nullptr, &status), std::free};
    return status == 0 ? std::string{result.get()} : std::string{mangled};
  }

  template<typename T>
  std::string busTypeName()
  {
    auto const full = demangle(typeid(T).name());
    auto const pos = full.rfind("::");
    return pos != std::string::npos ? full.substr(pos + 2) : full;
  }

  inline std::string_view shortFileName(std::source_location const& src)
  {
    auto const path = std::string_view{src.file_name()};
    auto const pos = path.rfind('/');
    return pos != std::string_view::npos ? path.substr(pos + 1) : path;
  }
}
