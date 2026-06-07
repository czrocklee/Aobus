// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <cstdint>

namespace
{
  // POSITIVE
  class UnmarkedConcrete
  {
  public:
    void doWork() {}
  };

  // POSITIVE
  struct UnmarkedStruct
  {
    std::int32_t val;
  };

  // NEGATIVE
  class CorrectConcrete final
  {
  public:
    void doWork() {}
  };

  // NEGATIVE
  class DesignedForInheritance
  {
  protected:
    DesignedForInheritance() = default;
  };

  // NEGATIVE
  class PolymorphicClass
  {
  public:
    virtual ~PolymorphicClass() = default;
    virtual void doSomething() = 0;
  };

  // NEGATIVE
  class DerivedClass : public DesignedForInheritance
  {
  public:
    void run() {}
  };

  // NEGATIVE
  class IProcessor
  {
  public:
    void process() {}
  };

  // NEGATIVE
  class ProcessorBase
  {
  public:
    void process() {}
  };

  // NEGATIVE
  class ProtectedDtorClass
  {
  public:
    void run() {}

  protected:
    ~ProtectedDtorClass() = default;
  };

  // POSITIVE
  class OuterClass
  {
  private:
    // POSITIVE
    class PrivateNested
    {
    public:
      void run() {}
    };

  public:
    // NEGATIVE
    class PublicNested
    {
    public:
      void run() {}
    };
  };
}
