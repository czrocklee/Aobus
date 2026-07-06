#include <chrono>

struct NonChronoType
{
  // NEGATIVE
  int timeoutMs = 0;
  // NEGATIVE
  double delaySec = 0.0;
  // NEGATIVE
  long startTimeMs = 0;
};

class DurationFixture
{
public:
  // NEGATIVE
  std::chrono::milliseconds timeout;

  // Media playback position is a duration from the start of the stream.
  // NEGATIVE
  std::chrono::milliseconds position;

  // NEGATIVE
  std::chrono::seconds duration;

  // POSITIVE
  std::chrono::milliseconds gems;

  // POSITIVE
  std::chrono::milliseconds timeoutMs;

  // POSITIVE
  std::chrono::seconds duration_sec;

  // A point noun is not valid for a span.
  // POSITIVE
  std::chrono::milliseconds deadline;

  // Redundant: 'elapsed' already names a span, so 'Duration' is dead weight.
  // POSITIVE
  std::chrono::milliseconds elapsedDuration;

  // Redundant: 'track position' already names a stream-relative span.
  // POSITIVE
  std::chrono::milliseconds trackPositionDuration;

  // Redundant: 'elapsed' already names a span, so 'Time' is dead weight.
  // POSITIVE
  std::chrono::milliseconds elapsedTime;

  // 'buffered' is a qualifier, not a time noun; the 'Duration' suffix carries it.
  // NEGATIVE
  std::chrono::milliseconds bufferedDuration;

  // NEGATIVE
  std::chrono::milliseconds getTimeout() const { return timeout; }

  // POSITIVE
  std::chrono::milliseconds getTimeoutMs() const { return timeout; }

  // NEGATIVE
  std::chrono::seconds getGracePeriod() const { return duration; }
};

class TimePointFixture
{
public:
  // NEGATIVE
  std::chrono::steady_clock::time_point startTime;

  // NEGATIVE
  std::chrono::steady_clock::time_point deadline;

  // NEGATIVE
  std::chrono::system_clock::time_point epoch;

  // POSITIVE
  std::chrono::steady_clock::time_point value;

  // POSITIVE
  std::chrono::steady_clock::time_point startTimeMs;

  // A span noun is not valid for an instant.
  // POSITIVE
  std::chrono::steady_clock::time_point elapsed;

  // Redundant: 'now' already names an instant, so 'Time' is dead weight.
  // POSITIVE
  std::chrono::steady_clock::time_point nowTime;

  // 'frame' is a qualifier, not a complete time noun; the 'Time' suffix carries it.
  // NEGATIVE
  std::chrono::steady_clock::time_point frameTime;

  // NEGATIVE
  std::chrono::steady_clock::time_point currentTime() const { return startTime; }

  // POSITIVE
  std::chrono::steady_clock::time_point current() const { return startTime; }

  // Conversion / factory functions are exempt even without an approved suffix.
  // NEGATIVE
  static std::chrono::steady_clock::time_point fromMicros(long micros) { return {}; }
};

// NEGATIVE
void processDuration(std::chrono::seconds duration)
{
}

// POSITIVE
void processDurationInvalid(std::chrono::seconds durationMs)
{
}

// NEGATIVE
void schedule(std::chrono::steady_clock::time_point deadline)
{
}

// POSITIVE
void scheduleAt(std::chrono::steady_clock::time_point value)
{
}

void testLocalVariables()
{
  // POSITIVE
  std::chrono::milliseconds timeoutMs = std::chrono::seconds{1};

  // NEGATIVE
  std::chrono::milliseconds timeout = std::chrono::seconds{1};

  // NEGATIVE
  std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();

  // Redundant: 'now' already names an instant, so 'Time' is dead weight.
  // POSITIVE
  std::chrono::steady_clock::time_point nowTime = std::chrono::steady_clock::now();

  // POSITIVE
  std::chrono::steady_clock::time_point value = std::chrono::steady_clock::now();

  // Short conventional instant names are accepted: 'tp' and ordered samples 't0', 't1', ...
  // NEGATIVE
  std::chrono::steady_clock::time_point tp = std::chrono::steady_clock::now();

  // NEGATIVE
  std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();

  // NEGATIVE
  std::chrono::steady_clock::time_point t1 = std::chrono::steady_clock::now();

  // Multi-digit sample indices are fine too.
  // NEGATIVE
  std::chrono::steady_clock::time_point t12 = std::chrono::steady_clock::now();

  // The 't<n>' form must be all digits after 't', so this is still rejected.
  // POSITIVE
  std::chrono::steady_clock::time_point t1a = std::chrono::steady_clock::now();
}
