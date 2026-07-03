// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <cstdint>
#include <ostream>
#include <string>
#include <string_view>

namespace ao::cli
{
  enum class OutputFormat : std::uint8_t
  {
    Plain,
    Yaml,
    Json,
  };

  std::string yamlQuote(std::string_view value);
  std::string jsonQuote(std::string_view value);
  std::string quote(OutputFormat format, std::string_view value);

  class JsonObject final
  {
  public:
    explicit JsonObject(std::ostream& os);
    ~JsonObject();

    JsonObject(JsonObject const&) = delete;
    JsonObject& operator=(JsonObject const&) = delete;
    JsonObject(JsonObject&&) = delete;
    JsonObject& operator=(JsonObject&&) = delete;

    void field(std::string_view key);
    void stringField(std::string_view key, std::string_view value);
    void uintField(std::string_view key, std::uint64_t value);
    void boolField(std::string_view key, bool value);
    void close();

  private:
    void closeQuietly() noexcept;

    std::ostream& _os;
    bool _first = true;
    bool _closed = false;
  };

  class JsonArray final
  {
  public:
    explicit JsonArray(std::ostream& os);
    ~JsonArray();

    JsonArray(JsonArray const&) = delete;
    JsonArray& operator=(JsonArray const&) = delete;
    JsonArray(JsonArray&&) = delete;
    JsonArray& operator=(JsonArray&&) = delete;

    void element();
    void close();

  private:
    void closeQuietly() noexcept;

    std::ostream& _os;
    bool _first = true;
    bool _closed = false;
  };

  class YamlSequence final
  {
  public:
    YamlSequence(std::ostream& os, std::int32_t indent, std::string_view key);
    ~YamlSequence();

    YamlSequence(YamlSequence const&) = delete;
    YamlSequence& operator=(YamlSequence const&) = delete;
    YamlSequence(YamlSequence&&) = delete;
    YamlSequence& operator=(YamlSequence&&) = delete;

    void itemPrefix();
    void close();

  private:
    void ensureStarted();
    void closeQuietly() noexcept;

    std::ostream& _os;
    std::int32_t _indent;
    std::string_view _key;
    bool _started = false;
    bool _closed = false;
  };

  void yamlKeyValue(std::ostream& os, std::int32_t indent, std::string_view key, std::string_view value);
  void yamlKeyValue(std::ostream& os, std::int32_t indent, std::string_view key, char const* value);
  void yamlKeyValue(std::ostream& os, std::int32_t indent, std::string_view key, std::uint64_t value);
  void yamlKeyValue(std::ostream& os, std::int32_t indent, std::string_view key, bool value);
} // namespace ao::cli
