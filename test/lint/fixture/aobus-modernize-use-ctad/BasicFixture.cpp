// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "TestHelpers.h"
#include <ao/Error.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <forward_list>
#include <initializer_list>
#include <list>
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

// The deducibility fixtures use non-primitive arguments: explicit primitive
// arguments are exempt before deducibility is considered.
template<typename T = std::string>
struct SameDefault
{
  SameDefault(std::string /*val*/) {}
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

  template<typename... Us>
  Packed(Us&&... /*values*/)
  {
  }
};

template<typename T>
struct IdentityInput
{
  IdentityInput(std::type_identity_t<T> /*val*/) {}
  IdentityInput(T /*value*/, std::type_identity_t<T> /*fallback*/) {}
};

template<typename T = std::string const>
struct CvDefault
{
  CvDefault(std::string /*val*/) {}
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

template<typename T>
void deduceInTemplate(T value)
{
  // NEGATIVE - the dependent definition and its instantiations are not diagnosed.
  [[maybe_unused]] auto const boxed = Box{value};
}

void ctadPositiveCases()
{
  // POSITIVE
  [[maybe_unused]] auto const v1 = std::vector<std::byte>{std::byte{1}};

  // POSITIVE
  [[maybe_unused]] auto const p2 = std::pair<std::string, std::string>{std::string{"key"}, std::string{"value"}};

  // POSITIVE
  [[maybe_unused]] auto const b1 = Box<std::string>{std::string{"value"}};

  // POSITIVE
  [[maybe_unused]] auto const kv = KeyValue<std::string, std::byte>{std::string{"key"}, std::byte{1}};

  // POSITIVE
  [[maybe_unused]] auto const rows = std::vector<Row>{Row{"Gamma", 1, 2}, Row{"Alpha", 1, 3}};

  std::string_view sv = "hello";

  // POSITIVE
  [[maybe_unused]] auto const defaultInput = StringInput<DefaultEncoding>{sv};

  // POSITIVE
  [[maybe_unused]] auto const sameDefault = SameDefault<std::string>{std::string{"value"}};
}

// CTAD must not deduce a primitive argument; the arguments are spelled instead.
#define CTAD_FIXTURE_CHECK(expr) static_cast<void>(expr)
#define CTAD_FIXTURE_DEDUCED_BODY() static_cast<void>(std::vector{1})

void ctadPrimitiveDeductionCases()
{
  // POSITIVE
  [[maybe_unused]] auto const v4 = std::vector{1, 2, 3};

  // POSITIVE
  [[maybe_unused]] auto const p1 = std::pair{1, 2.0};

  // POSITIVE
  [[maybe_unused]] auto const t1 = std::tuple{std::string{"key"}, 'a'};

  // POSITIVE
  [[maybe_unused]] auto const b1 = Box{42};

  // POSITIVE
  [[maybe_unused]] auto const b2 = Box(42);

  // POSITIVE
  [[maybe_unused]] Box const b3{42};

  // POSITIVE
  [[maybe_unused]] auto const count = std::atomic{std::size_t{0}};

  // POSITIVE
  [[maybe_unused]] auto const flag = std::atomic{false};

  using Count = std::size_t;

  // POSITIVE - an alias of a primitive is still primitive.
  [[maybe_unused]] auto const aliasCount = std::atomic{Count{0}};

  // POSITIVE
  [[maybe_unused]] auto const maybe = std::optional{1};

  // POSITIVE - the std::array finding points at std::to_array.
  [[maybe_unused]] auto const values = std::array{1, 2, 3};

  // POSITIVE
  [[maybe_unused]] std::array const moreValues{1.0F, 2.0F};

  // POSITIVE
  [[maybe_unused]] auto const view = std::span{values};

  // POSITIVE - macro arguments are written by the author.
  CTAD_FIXTURE_CHECK((std::vector{1, 2} == std::vector<int>{1, 2}));

  // NEGATIVE - macro bodies are not.
  CTAD_FIXTURE_DEDUCED_BODY();

  // NEGATIVE - every deduced argument is non-primitive.
  [[maybe_unused]] auto const t2 = std::tuple{std::string{"key"}, std::byte{1}};

  // NEGATIVE - explicit primitive arguments are the required spelling.
  [[maybe_unused]] auto const explicitCount = std::atomic<std::size_t>{0};

  // NEGATIVE - explicit alias of a primitive stays spelled.
  [[maybe_unused]] auto const explicitAliasCount = std::atomic<Count>{0};

  // NEGATIVE - std::to_array keeps the element type spelled and the size deduced.
  [[maybe_unused]] auto const explicitValues = std::to_array<int>({1, 2, 3});

  deduceInTemplate(1);
}

void ctadPointerCases(Sink* sink)
{
  // NEGATIVE - nullptr does not deduce Sink*.
  [[maybe_unused]] static constinit auto nullSink = std::atomic<Sink*>{nullptr};

  // NEGATIVE - a braced argument cannot supply the atomic's pointer type.
  [[maybe_unused]] auto const bracedNull = std::atomic<Sink*>{{nullptr}};

  // POSITIVE - the first argument supplies T; the braced second argument only initializes it.
  [[maybe_unused]] auto const bracedFallback = Box<Sink*>{sink, {nullptr}};

  // NEGATIVE - the two pair elements must be deduced independently.
  [[maybe_unused]] auto const bracedPair = std::pair<Sink*, Sink*>{sink, {nullptr}};

  // NEGATIVE - integer zero also requires a null-to-pointer conversion.
  [[maybe_unused]] std::atomic<Sink*> zeroSink{0};

  // NEGATIVE - the two arguments would deduce conflicting types for T.
  [[maybe_unused]] auto const boxed = Box<Sink*>{sink, nullptr};

  // POSITIVE - only the first argument deduces T; the fallback converts afterward.
  [[maybe_unused]] auto const withFallback = IdentityInput<Sink*>{sink, nullptr};

  // POSITIVE - a null value already typed as Sink* still deduces Sink*.
  [[maybe_unused]] auto const typedNullSink = std::atomic<Sink*>{static_cast<Sink*>(nullptr)};

  // NEGATIVE - tuple deduction would change the second element type.
  [[maybe_unused]] auto const tuple = std::tuple<Sink*, Sink*>{sink, nullptr};

  // POSITIVE - both tuple elements already have the intended pointer type.
  [[maybe_unused]] auto const typedTuple = std::tuple<Sink*, Sink*>{sink, static_cast<Sink*>(nullptr)};

  // POSITIVE - the second element already has the intended std::nullptr_t type.
  [[maybe_unused]] auto const mixedTuple = std::tuple<Sink*, std::nullptr_t>{sink, nullptr};

  // NEGATIVE - the copy deduction candidate would unwrap the inner tuple.
  [[maybe_unused]] auto const nested = std::tuple<std::tuple<Sink*, std::nullptr_t>>{mixedTuple};

  // NEGATIVE - the guide from pair would unpack it into two tuple elements.
  [[maybe_unused]] auto const nestedPair = std::tuple<std::pair<Sink*, Sink*>>{std::pair{sink, sink}};

  // NEGATIVE - forwarding constructor parameters do not deduce the class pack.
  [[maybe_unused]] auto const forwarded = Packed<Sink*, Sink*>{sink, nullptr};

  // NEGATIVE - the initializer list would deduce std::nullptr_t, not Sink*.
  [[maybe_unused]] auto const deque = std::deque<Sink*>{nullptr};
  [[maybe_unused]] auto const list = std::list<Sink*>{sink, nullptr};          // NEGATIVE
  [[maybe_unused]] auto const forwardList = std::forward_list<Sink*>{nullptr}; // NEGATIVE

  // NEGATIVE - the second list element conflicts with the first for deduction.
  [[maybe_unused]] auto const bag = IlBag<Sink*>{sink, nullptr};

  // NEGATIVE - IlBag{{nullptr}} deduces IlBag<std::nullptr_t>, not IlBag<Sink*>.
  [[maybe_unused]] auto const bracedElement = IlBag<Sink*>{{nullptr}};

  // NEGATIVE - Box takes T, not initializer_list<T>; the list type is only contextual.
  [[maybe_unused]] auto const contextualList = Box<std::initializer_list<Sink*>>{{sink}};

  // POSITIVE - a custom initializer-list constructor can still deduce typed pointers.
  [[maybe_unused]] auto const typedBag = IlBag<Sink*>{sink, static_cast<Sink*>(nullptr)};

  // NEGATIVE - nullptr does not deduce a pointer-to-member type.
  [[maybe_unused]] auto const nullMember = std::atomic<std::uint16_t Row::*>{nullptr};

  // POSITIVE - the member address already has the intended pointer-to-member type.
  [[maybe_unused]] auto const member = Box<std::uint16_t Row::*>{&Row::disc};
}

void ctadNegativeCases()
{
  [[maybe_unused]] auto const v1 = std::vector{std::byte{1}};
  [[maybe_unused]] auto const v2 = std::vector<int>{};
  [[maybe_unused]] auto const v3 = std::vector<int>();

  [[maybe_unused]] Foo const f1{10};
  [[maybe_unused]] Foo const f2(10, 20);

  [[maybe_unused]] auto const p2 = std::pair<std::string, std::string>{"key", "value"};
  [[maybe_unused]] auto const b1 = Box{std::string{"value"}};

  // NEGATIVE - explicit primitive arguments stay spelled.
  [[maybe_unused]] auto const v2Explicit = std::vector<int>{1, 2, 3};
  [[maybe_unused]] auto const p1Explicit = std::pair<int, double>{1, 2.0};
  [[maybe_unused]] auto const t1Explicit = std::tuple<int, double, char>{1, 2.0, 'a'};
  [[maybe_unused]] auto const b1Explicit = Box<int>{42};
  [[maybe_unused]] auto const kvExplicit = KeyValue<std::string, int>{std::string{"key"}, 1};
  [[maybe_unused]] auto const s1Explicit = std::set<int>{1, 2, 3};
  [[maybe_unused]] auto const flag = std::atomic<bool>{false};

  [[maybe_unused]] auto const v5 = std::vector<int>(10, 0);
  [[maybe_unused]] auto const v6 = std::vector<std::byte>(100, std::byte{0});
  [[maybe_unused]] auto const v7 = std::vector<std::vector<int>>{10};
  [[maybe_unused]] auto const v8 =
    std::vector<std::shared_ptr<Sink>>{std::make_shared<ConsoleSink>(), std::make_shared<FileSink>()};
  auto const v4 = std::vector<int>{1, 2, 3};
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
  [[maybe_unused]] auto const nonType = NonTypeDefault<std::string, 5>{std::string{"value"}};

  // NEGATIVE - explicit parameter pack not reachable from constructor parameters.
  [[maybe_unused]] auto const packed = Packed<std::string, std::byte>{42};

  // NEGATIVE - type parameter only appears inside a non-deduced alias.
  [[maybe_unused]] auto const identity = IdentityInput<std::string>{std::string{"value"}};

  // NEGATIVE - explicit argument differs from the default by cv-qualifier only.
  [[maybe_unused]] auto const cv = CvDefault<std::string>{std::string{"value"}};

  // NEGATIVE - explicit non-type argument of Declaration kind is not deducible.
  [[maybe_unused]] auto const refKey = RefKey<&g_global>{42};

  // NEGATIVE - explicit template arguments on ao::Result carry semantic value
  [[maybe_unused]] auto const explicitResult = ao::Result<int>{42};
}
