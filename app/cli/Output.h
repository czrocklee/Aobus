// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/yaml/Reflect.h>

#include <cstdint>
#include <ostream>
#include <string>

namespace ao::cli
{
  enum class OutputFormat : std::uint8_t
  {
    Plain,
    Yaml,
    Json,
  };

  namespace detail
  {
    inline void emitGeneratedDocument(std::ostream& os, std::string const& text)
    {
      os << text;

      if (text.empty() || text.back() != '\n')
      {
        os << '\n';
      }
    }
  } // namespace detail

  template<typename T>
  void emitDocument(std::ostream& os, OutputFormat format, T const& dto)
  {
    if (format == OutputFormat::Yaml)
    {
      detail::emitGeneratedDocument(os, yaml::toYamlString(dto));
    }
    else if (format == OutputFormat::Json)
    {
      detail::emitGeneratedDocument(os, yaml::toJsonString(dto));
    }
  }
} // namespace ao::cli
