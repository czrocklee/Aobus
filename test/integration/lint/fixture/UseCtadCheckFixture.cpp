// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "TestHelpers.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <random>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

template<typename T>
struct Box
{
  Box(T /*val*/) {}
  Box(T /*a*/, T /*b*/) {}
};

template<typename K, typename V>
struct KeyValue
{
  KeyValue(K /*k*/, V /*v*/) {}
};

struct Sink
{
  virtual ~Sink() = default;
};

struct ConsoleSink final : Sink
{};

struct FileSink final : Sink
{};

struct Row final
{
  std::string album;
  std::uint16_t disc;
  std::uint16_t track;
};

namespace llvm
{
  template<typename T>
  class StringSwitch
  {
  public:
    explicit StringSwitch(std::string_view /*name*/) {}
  };
} // namespace llvm

void noopDelete(char* /*ptr*/)
{
}

void ctadPositiveCases()
{
  // POSITIVE
  [[maybe_unused]] auto const v1 = std::vector<std::byte>{std::byte{1}};

  // POSITIVE
  [[maybe_unused]] auto const v2 = std::vector<int>{1, 2, 3};

  // POSITIVE
  [[maybe_unused]] auto const p1 = std::pair<int, double>{1, 2.0};

  // POSITIVE
  [[maybe_unused]] auto const p2 = std::pair<std::string, std::string>{std::string{"key"}, std::string{"value"}};

  // POSITIVE
  [[maybe_unused]] auto const t1 = std::tuple<int, double, char>{1, 2.0, 'a'};

  // POSITIVE
  [[maybe_unused]] auto const b1 = Box<int>{42};

  // POSITIVE
  [[maybe_unused]] auto const kv = KeyValue<std::string, int>{std::string{"key"}, 1};

  // POSITIVE
  [[maybe_unused]] auto const s1 = std::set<int>{1, 2, 3};

  // POSITIVE
  [[maybe_unused]] auto const rows = std::vector<Row>{Row{"Gamma", 1, 2}, Row{"Alpha", 1, 3}};
}

void ctadNegativeCases()
{
  [[maybe_unused]] auto const v1 = std::vector{std::byte{1}};
  [[maybe_unused]] auto const v2 = std::vector<int>{};
  [[maybe_unused]] auto const v3 = std::vector<int>();
  [[maybe_unused]] auto const v4 = std::vector{1, 2, 3};

  [[maybe_unused]] Foo const f1{10};
  [[maybe_unused]] Foo const f2(10, 20);

  [[maybe_unused]] auto const p1 = std::pair{1, 2.0};
  [[maybe_unused]] auto const p2 = std::pair<std::string, std::string>{"key", "value"};
  [[maybe_unused]] auto const t1 = std::tuple{1, 2.0, 'a'};
  [[maybe_unused]] auto const b1 = Box{42};

  [[maybe_unused]] auto const v5 = std::vector<int>(10, 0);
  [[maybe_unused]] auto const v6 = std::vector<std::byte>(100, std::byte{0});
  [[maybe_unused]] auto const v7 = std::vector<std::vector<int>>{10};
  [[maybe_unused]] auto const v8 =
    std::vector<std::shared_ptr<Sink>>{std::make_shared<ConsoleSink>(), std::make_shared<FileSink>()};
  [[maybe_unused]] auto const v9 = std::vector<int>{v4};
  [[maybe_unused]] auto const v10 = std::vector{std::pair<std::string, std::string>{"key", "value"}};
  [[maybe_unused]] auto const v11 = std::vector<Row>{{"Gamma", 1, 2}, {"Alpha", 1, 3}};

  std::array<int, 3> arr{1, 2, 3};
  [[maybe_unused]] auto const s2 = std::span<int>{arr.data(), arr.size()};
  [[maybe_unused]] auto const s3 = std::span<int const>{arr};

  [[maybe_unused]] auto const m1 = std::map<int, std::string>{{1, std::string{"one"}}, {2, std::string{"two"}}};
  [[maybe_unused]] auto const m2 = std::map<std::string, int>{{"one", 1}, {"two", 2}};
  [[maybe_unused]] auto const opt = std::optional<std::uint64_t>{std::uint32_t{1}};
  [[maybe_unused]] auto dist = std::uniform_int_distribution<std::size_t>{0, arr.size()};
  [[maybe_unused]] auto ptr = std::unique_ptr<char, void (*)(char*)>{nullptr, noopDelete};
  [[maybe_unused]] auto switcher = llvm::StringSwitch<bool>{"name"};
}
