#include "TestHelpers.h"

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
    int val;
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
