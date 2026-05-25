#include "TestHelpers.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

class BracedDemoBase
{
public:
  BracedDemoBase(std::int32_t /*val*/) {}
};

class BracedDemo final : public BracedDemoBase
{
  static constexpr std::int32_t kInitVal1 = 10;
  static constexpr std::int32_t kInitVal2 = 20;

public:
  BracedDemo()
    : BracedDemoBase{kInitVal1}, _data{kInitVal1}, _intData{kInitVal2}
  {
  }

  BracedDemo(double /*val*/)
    // POSITIVE
    : BracedDemoBase(kInitVal1)
    ,
    // POSITIVE
    _data(kInitVal1)
    ,
    // POSITIVE
    _intData(kInitVal2)
  {
  }

  void testCases()
  {
    // NEGATIVE
    [[maybe_unused]] auto const s1 = std::string{"foo"};

    // POSITIVE
    [[maybe_unused]] auto const s2 = std::string("bar");

    // NEGATIVE
    auto* const s3 = new std::string{"baz"};

    // POSITIVE
    auto* const s4 = new std::string("qux");

    // NEGATIVE
    [[maybe_unused]] Foo const s5{10};

    // POSITIVE
    [[maybe_unused]] Foo const s6(10);

    // POSITIVE
    [[maybe_unused]] auto const s7 = std::string(s1);

    // POSITIVE
    [[maybe_unused]] auto const p1 = std::filesystem::path(s1);

    // NEGATIVE: DANGEROUS cases (Should be IGNORED)
    [[maybe_unused]] auto const v1 = std::vector<int>(kInitVal1);       // size constructor
    [[maybe_unused]] auto const v2 = std::vector<int>(kInitVal1, 1);    // size + value
    [[maybe_unused]] auto const s8 = std::string(kInitVal1, 'a');       // count + char
    [[maybe_unused]] auto const s9 = std::string(s1.begin(), s1.end()); // iterator range

    // NEGATIVE: Scalar Casts (Should be IGNORED)
    [[maybe_unused]] auto const u1 = std::uintptr_t(&s1);
    [[maybe_unused]] auto const i1 = std::int32_t(1.5);
  }

private:
  Foo _data;
  std::int32_t _intData;
};
