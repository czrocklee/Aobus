// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "TestHelpers.h"
#include <ao/Error.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <map>
#include <memory>
#include <optional>
#include <random>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
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

// Non-std container with an initializer_list constructor: braced construction
// that resolves to the iterator-pair constructor is unsafe for CTAD because
// removing the template arguments would re-deduce via initializer_list.
template<typename T>
struct IlBag
{
  IlBag(std::initializer_list<T> /*il*/) {}
  IlBag(T const* /*first*/, T const* /*last*/) {}
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

// A class template where the first template parameter (Encoding) is NOT
// deducible from the constructor arguments: the constructor takes a
// generic View, not something parameterised on Encoding.  A deduction
// guide maps View to a *different* default encoding. Suggesting CTAD
// would silently switch the encoding, so the checker must stay quiet.
struct DefaultEncoding
{};
struct Utf8Encoding
{};

template<typename Encoding = DefaultEncoding>
class StringInput
{
public:
  using char_type = char;

  explicit StringInput(char const* /*data*/, std::size_t /*size*/) {}

  template<typename View>
  explicit StringInput(View const& /*view*/)
  {
  }
};

template<typename View>
StringInput(View const&) -> StringInput<DefaultEncoding>;

template<typename T = int>
struct SameDefault
{
  SameDefault(int /*val*/) {}
};

template<typename T, int N = 10>
struct NonTypeDefault
{
  NonTypeDefault(T /*val*/) {}
};

template<typename... Ts>
struct Packed
{
  Packed(int /*val*/) {}
};

template<typename T>
struct IdentityInput
{
  IdentityInput(std::type_identity_t<T> /*val*/) {}
};

template<typename T = int const>
struct CvDefault
{
  CvDefault(int /*val*/) {}
};

int g_global = 0;

template<int* P>
struct RefKey
{
  RefKey(int /*val*/) {}
};

void noopDelete(char* /*ptr*/)
{
}

void ctadPositiveCases()
{
  // POSITIVE: FIX-TO: [[maybe_unused]] auto const v1 = std::vector{std::byte{1}};
  [[maybe_unused]] auto const v1 = std::vector<std::byte>{std::byte{1}};

  // POSITIVE: FIX-TO: [[maybe_unused]] auto const v2 = std::vector{1, 2, 3};
  [[maybe_unused]] auto const v2 = std::vector<int>{1, 2, 3};

  // POSITIVE: FIX-TO: [[maybe_unused]] auto const p1 = std::pair{1, 2.0};
  [[maybe_unused]] auto const p1 = std::pair<int, double>{1, 2.0};

  // POSITIVE: FIX-TO: [[maybe_unused]] auto const p2 = std::pair{std::string{"key"}, std::string{"value"}};
  [[maybe_unused]] auto const p2 = std::pair<std::string, std::string>{std::string{"key"}, std::string{"value"}};

  // POSITIVE: FIX-TO: [[maybe_unused]] auto const t1 = std::tuple{1, 2.0, 'a'};
  [[maybe_unused]] auto const t1 = std::tuple<int, double, char>{1, 2.0, 'a'};

  // POSITIVE: FIX-TO: [[maybe_unused]] auto const b1 = Box{42};
  [[maybe_unused]] auto const b1 = Box<int>{42};

  // POSITIVE: FIX-TO: [[maybe_unused]] auto const kv = KeyValue{std::string{"key"}, 1};
  [[maybe_unused]] auto const kv = KeyValue<std::string, int>{std::string{"key"}, 1};

  // POSITIVE: FIX-TO: [[maybe_unused]] auto const s1 = std::set{1, 2, 3};
  [[maybe_unused]] auto const s1 = std::set<int>{1, 2, 3};

  // POSITIVE: FIX-TO: [[maybe_unused]] auto const rows = std::vector{Row{"Gamma", 1, 2}, Row{"Alpha", 1, 3}};
  [[maybe_unused]] auto const rows = std::vector<Row>{Row{"Gamma", 1, 2}, Row{"Alpha", 1, 3}};

  std::string_view sv = "hello";

  // POSITIVE: FIX-TO: [[maybe_unused]] auto const defaultInput = StringInput{sv};
  [[maybe_unused]] auto const defaultInput = StringInput<DefaultEncoding>{sv};

  // POSITIVE: FIX-TO: [[maybe_unused]] auto const sameDefault = SameDefault{42};
  [[maybe_unused]] auto const sameDefault = SameDefault<int>{42};
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
  [[maybe_unused]] auto const v12 = std::vector<std::int32_t>{1, 2, 3};
  [[maybe_unused]] auto const p3 = std::pair<std::uint32_t, std::uint32_t>{1, 2};

  std::array<int, 3> arr{1, 2, 3};
  [[maybe_unused]] auto const s2 = std::span<int>{arr.data(), arr.size()};
  [[maybe_unused]] auto const s3 = std::span<int const>{arr};
  [[maybe_unused]] auto const bag = IlBag<int>{arr.data(), arr.data() + arr.size()};

  [[maybe_unused]] auto const m1 = std::map<int, std::string>{{1, std::string{"one"}}, {2, std::string{"two"}}};
  [[maybe_unused]] auto const m2 = std::map<std::string, int>{{"one", 1}, {"two", 2}};
  [[maybe_unused]] auto const opt = std::optional<std::uint64_t>{std::uint32_t{1}};
  [[maybe_unused]] auto dist = std::uniform_int_distribution<std::size_t>{0, arr.size()};
  [[maybe_unused]] auto ptr = std::unique_ptr<char, void (*)(char*)>{nullptr, noopDelete};
  [[maybe_unused]] auto switcher = llvm::StringSwitch<bool>{"name"};

  // NEGATIVE - non-deducible template parameter: CTAD would deduce DefaultEncoding
  // but we explicitly want Utf8Encoding, which is not reachable from the constructor's
  // parameter list.  The checker must not fire here.
  std::string_view sv = "hello";
  [[maybe_unused]] auto const input = StringInput<Utf8Encoding>{sv};

  // NEGATIVE - explicit non-type argument not reachable from constructor parameters.
  [[maybe_unused]] auto const nonType = NonTypeDefault<int, 5>{42};

  // NEGATIVE - explicit parameter pack not reachable from constructor parameters.
  [[maybe_unused]] auto const packed = Packed<int, double>{42};

  // NEGATIVE - type parameter only appears inside a non-deduced alias.
  [[maybe_unused]] auto const identity = IdentityInput<int>{42};

  // NEGATIVE - explicit argument differs from the default by cv-qualifier only.
  [[maybe_unused]] auto const cv = CvDefault<int>{42};

  // NEGATIVE - explicit non-type argument of Declaration kind is not deducible.
  [[maybe_unused]] auto const refKey = RefKey<&g_global>{42};

  // NEGATIVE - explicit template arguments on ao::Result carry semantic value
  [[maybe_unused]] auto const explicitResult = ao::Result<int>{42};
}
