#pragma once

// ============================================================================
// 10. MemberOrderCheck (Requires Header File)
// ============================================================================

// NEGATIVE: Correct order (public -> protected -> private)
class CorrectOrderDemo
{
public:
  void doPublic();

protected:
  void doProtected();

private:
  int _val1;
};

// POSITIVE: Incorrect order (private -> public)
class OrderDemo
{
private:
  int _val1;

public: // Warning: 'public' access section appears after 'private'; expected public before protected before private
        // (Rule 2.5.2)
  void doPublic();
};

// POSITIVE: Incorrect order (protected -> public)
class StructOrderDemo
{
protected:
  int val1;

public: // Warning: 'public' access section appears after 'protected'; expected public before protected before private
        // (Rule 2.5.2)
  void doPublic();
};
