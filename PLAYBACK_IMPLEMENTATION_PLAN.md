# RockStudio Playback Implementation Plan

## Scope And Decisions

This document captures the implementation plan for turning the current GTK app into a real player with a global playback bar inserted between the menu bar and the main content area.

Confirmed product and technical constraints:

- First backend is `PipeWire`.
- First version does not implement `Next/Previous`.
- First version does not implement software volume.
- Playback is global to the app, not owned by a specific list page.
- FFmpeg provides decode and PCM normalization.
- A real-time-safe PCM handoff is required between decode and output.
- ALSA exclusive mode remains a later milestone, not the first delivery.

The coding style and naming in this plan follow [CONTRIBUTING.md](CONTRIBUTING.md).

## Current Baseline

The existing GTK app already provides enough metadata and structure to host playback:

- `app/MainWindow.cpp` currently builds a vertical container with `_menuBar` and `_paned`.
- `app/TrackViewPage.*` provides selection and list browsing but no activation signal for playback.
- `app/model/TrackRowDataProvider.*` can already resolve track file paths and cover art.
- `include/rs/core/TrackView.h` already exposes duration, sample rate, channels, bit depth, URI, and cover art.
- `CMakeLists.txt` currently has no playback dependencies.

The natural UI insertion point for the playback bar is between `_menuBar` and `_paned`.

## Architecture Overview

The first solid architecture should be:

- `MainWindow`
  - owns `PlaybackBar`
  - owns `PlaybackController`
  - resolves current selection into a playback descriptor
- `PlaybackBar`
  - pure GTK UI widget
  - emits play, pause, stop, seek requests
  - renders playback snapshot state
- `PlaybackController`
  - main-thread facade for playback commands
  - owns `PlaybackEngine`
- `PlaybackEngine`
  - owns decode session, ring buffer, and backend
  - manages decode thread and transport state
- `FfmpegDecoderSession`
  - wraps FFmpeg C API with RAII
  - returns normalized interleaved PCM blocks
- `PcmRingBuffer`
  - lock-free SPSC PCM channel between producer and consumer
- `PipeWireBackend`
  - first output backend
- `FormatNegotiator`
  - later stage, handles capability matching and fallback policy
- `AlsaExclusiveBackend`
  - later stage, explicit audiophile mode

## Directory And File Layout

Planned new files:

- `app/PlaybackBar.h`
- `app/PlaybackBar.cpp`
- `app/playback/PlaybackTypes.h`
- `app/playback/PlaybackController.h`
- `app/playback/PlaybackController.cpp`
- `app/playback/PlaybackEngine.h`
- `app/playback/PlaybackEngine.cpp`
- `app/playback/PcmRingBuffer.h`
- `app/playback/PcmRingBuffer.cpp`
- `app/playback/FfmpegDecoderSession.h`
- `app/playback/FfmpegDecoderSession.cpp`
- `app/playback/IAudioBackend.h`
- `app/playback/PipeWireBackend.h`
- `app/playback/PipeWireBackend.cpp`
- `app/playback/FormatNegotiator.h`
- `app/playback/FormatNegotiator.cpp`
- `app/playback/AlsaExclusiveBackend.h`
- `app/playback/AlsaExclusiveBackend.cpp`
- `test/playback/PcmRingBufferTest.cpp`
- `test/playback/FfmpegDecoderSessionTest.cpp`
- `test/playback/FormatNegotiatorTest.cpp`

Planned modified files:

- `CMakeLists.txt`
- `app/MainWindow.h`
- `app/MainWindow.cpp`
- `app/TrackViewPage.h`
- `app/TrackViewPage.cpp`
- `app/model/TrackRowDataProvider.h`
- `app/model/TrackRowDataProvider.cpp`

## Rules For The First Working Version

- No `Next/Previous` buttons or queue navigation in the first release.
- No software volume in the first release.
- Playback must survive list page switching.
- Audio callback must not touch GTK, LMDB, FFmpeg, filesystem, mutexes, or heap allocation.
- PCM output path must be interleaved.
- FFmpeg planar output must be normalized through `libswresample`.
- `PipeWire` is the only required backend for the first version.
- ALSA exclusive is implemented only after PipeWire playback is stable.

## Milestone 1: Playback Bar Shell And Build Wiring

### Objective

Create the visible playback bar, wire new source files into the build, and establish type boundaries without real playback yet.

### Files To Add

- `app/PlaybackBar.h`
- `app/PlaybackBar.cpp`
- `app/playback/PlaybackTypes.h`
- `app/playback/PlaybackController.h`
- `app/playback/PlaybackController.cpp`

### Files To Modify

- `CMakeLists.txt`
- `app/MainWindow.h`
- `app/MainWindow.cpp`

### Required Changes

- Add pkg-config lookup for FFmpeg libraries:
  - `libavformat`
  - `libavcodec`
  - `libavutil`
  - `libswresample`
- Add pkg-config lookup for `libpipewire-0.3`.
- Optionally prepare ALSA dependency lookup now, but do not wire ALSA implementation yet.
- Add playback files to the `RockStudio` target.
- Insert `PlaybackBar` between `_menuBar` and `_paned` in `MainWindow::setupLayout()`.
- Add `PlaybackController` ownership to `MainWindow`.
- Keep all controls disabled unless a playable track selection exists.

### Key Class Skeletons

#### `app/playback/PlaybackTypes.h`

```cpp
#pragma once

#include <rs/core/Type.h>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

namespace app::playback
{

  enum class TransportState
  {
    Idle,
    Opening,
    Buffering,
    Playing,
    Paused,
    Seeking,
    Stopping,
    Error,
  };

  enum class BackendKind
  {
    None,
    PipeWire,
    AlsaExclusive,
  };

  struct StreamFormat final
  {
    std::uint32_t sampleRate = 0;
    std::uint8_t channels = 0;
    std::uint8_t bitDepth = 0;
    bool isFloat = false;
    bool isInterleaved = true;
  };

  struct TrackPlaybackDescriptor final
  {
    rs::core::TrackId trackId{};
    std::filesystem::path filePath;
    std::string title;
    std::string artist;
    std::string album;
    std::optional<rs::core::ResourceId> coverArtId;
    std::uint32_t durationMs = 0;
    std::uint32_t sampleRateHint = 0;
    std::uint8_t channelsHint = 0;
    std::uint8_t bitDepthHint = 0;
  };

  struct PlaybackSnapshot final
  {
    TransportState state = TransportState::Idle;
    BackendKind backend = BackendKind::None;
    std::string trackTitle;
    std::string trackArtist;
    std::string statusText;
    std::uint32_t durationMs = 0;
    std::uint32_t positionMs = 0;
    std::uint32_t bufferedMs = 0;
    std::uint32_t underrunCount = 0;
    std::optional<StreamFormat> activeFormat;
  };

} // namespace app::playback
```

#### `app/PlaybackBar.h`

```cpp
#pragma once

#include "playback/PlaybackTypes.h"

#include <gtkmm.h>

#include <cstdint>

class PlaybackBar final : public Gtk::Box
{
public:
  using PlaySignal = sigc::signal<void()>;
  using PauseSignal = sigc::signal<void()>;
  using StopSignal = sigc::signal<void()>;
  using SeekSignal = sigc::signal<void(std::uint32_t)>;

  PlaybackBar();
  ~PlaybackBar() override;

  void setSnapshot(app::playback::PlaybackSnapshot const& snapshot);
  void setInteractive(bool enabled);

  PlaySignal& signalPlayRequested();
  PauseSignal& signalPauseRequested();
  StopSignal& signalStopRequested();
  SeekSignal& signalSeekRequested();

private:
  void setupLayout();
  void setupSignals();
  void updateTransportButtons(app::playback::TransportState state);

  Gtk::Box _metaBox;
  Gtk::Label _titleLabel;
  Gtk::Label _artistLabel;
  Gtk::Box _transportBox;
  Gtk::Button _playButton;
  Gtk::Button _pauseButton;
  Gtk::Button _stopButton;
  Gtk::Scale _seekScale;
  Gtk::Label _timeLabel;
  Gtk::Label _statusLabel;
  Gtk::Label _formatLabel;

  PlaySignal _playRequested;
  PauseSignal _pauseRequested;
  StopSignal _stopRequested;
  SeekSignal _seekRequested;
};
```

#### `app/playback/PlaybackController.h`

```cpp
#pragma once

#include "PlaybackTypes.h"

#include <memory>

namespace app::playback
{
  class PlaybackEngine;
}

namespace app::playback
{

  class PlaybackController final
  {
  public:
    PlaybackController();
    ~PlaybackController();

    void play(TrackPlaybackDescriptor descriptor);
    void pause();
    void resume();
    void stop();
    void seek(std::uint32_t positionMs);

    PlaybackSnapshot snapshot() const;

  private:
    std::unique_ptr<PlaybackEngine> _engine;
  };

} // namespace app::playback
```

### Verification

- `nix-shell --run "cmake --preset linux-debug"`
- `nix-shell --run "cmake --build --preset linux-debug --parallel"`
- App launches successfully.
- Playback bar is visible between menu and content.
- Existing import, list, and tag workflows still work.
- Controls are disabled when there is no playable selection.

## Milestone 2: Playback Descriptor And Track Activation

### Objective

Convert the current selection into a fully owned playback descriptor and add activation semantics to track pages.

### Files To Add

- `app/playback/PlaybackSelection.h`

### Files To Modify

- `app/TrackViewPage.h`
- `app/TrackViewPage.cpp`
- `app/model/TrackRowDataProvider.h`
- `app/model/TrackRowDataProvider.cpp`
- `app/MainWindow.h`
- `app/MainWindow.cpp`

### Required Changes

- Add `TrackRowDataProvider::getPlaybackDescriptor()`.
- Resolve path, title, artist, album, duration, sample rate, channels, bit depth, and optional cover art in one owned DTO.
- Extend `TrackViewPage` with a `trackActivated(trackId)` signal.
- Add a `getPrimarySelectedTrackId()` helper to avoid ambiguity.
- Wire `MainWindow` play action to current page selection.
- Keep playback global to the app.

### Key Class Skeletons

#### `app/model/TrackRowDataProvider.h`

```cpp
namespace app::playback
{
  struct TrackPlaybackDescriptor;
}

namespace app::model
{

  class TrackRowDataProvider final
  {
  public:
    explicit TrackRowDataProvider(rs::core::MusicLibrary& ml);

    std::optional<RowData> getRow(TrackId id);
    std::optional<std::uint32_t> getCoverArtId(TrackId id);
    std::optional<std::filesystem::path> getUriPath(TrackId id);
    std::optional<app::playback::TrackPlaybackDescriptor> getPlaybackDescriptor(TrackId id);

    void invalidateHot(TrackId id);
    void invalidateFull(TrackId id);
    void remove(TrackId id);

  private:
    std::string resolveDictionaryString(rs::core::DictionaryId id);

    rs::core::MusicLibrary* _ml;
    rs::core::TrackStore* _store;
    rs::core::DictionaryStore* _dict;
    std::unordered_map<TrackId, RowData> _rowCache;
    std::unordered_map<rs::core::DictionaryId, std::string> _stringCache;
  };

} // namespace app::model
```

#### `app/TrackViewPage.h`

```cpp
class TrackViewPage final : public Gtk::Box
{
public:
  using TrackId = TrackListAdapter::TrackId;
  using SelectionChangedSignal = sigc::signal<void()>;
  using TrackActivatedSignal = sigc::signal<void(TrackId)>;

  explicit TrackViewPage(Glib::RefPtr<TrackListAdapter> const& adapter);
  ~TrackViewPage() override;

  std::vector<TrackId> getSelectedTrackIds() const;
  std::optional<TrackId> getPrimarySelectedTrackId() const;

  SelectionChangedSignal& signalSelectionChanged();
  TrackActivatedSignal& signalTrackActivated();

private:
  void setupColumns();
  void setupStatusBar();
  void setupActivation();
  void onFilterChanged();
  void onSelectionChanged(std::uint32_t position, std::uint32_t nItems);
  void onActivateCurrentSelection();

  Gtk::Entry _filterEntry;
  Gtk::Label _statusLabel;
  Gtk::ScrolledWindow _scrolledWindow;
  Gtk::ColumnView _columnView;
  Glib::RefPtr<TrackListAdapter> _adapter;
  Glib::RefPtr<Gtk::MultiSelection> _selectionModel;
  SelectionChangedSignal _selectionChanged;
  TrackActivatedSignal _trackActivated;
};
```

#### `app/MainWindow.h`

```cpp
private:
  void setupPlayback();
  void refreshPlaybackBar();
  void onPlayRequested();
  void onPauseRequested();
  void onStopRequested();
  void onSeekRequested(std::uint32_t positionMs);
  std::optional<app::playback::TrackPlaybackDescriptor> currentSelectionPlaybackDescriptor() const;
```

### Verification

- Unit tests for `getPlaybackDescriptor()` return values.
- Manual test: select a track and trigger Play.
- Manual test: switching pages does not invalidate the current descriptor.
- Build still passes in debug mode.

## Milestone 3: FFmpeg Decode Layer And PCM Normalization

### Objective

Implement a modern FFmpeg wrapper that returns normalized interleaved PCM and hides FFmpeg state machine details.

### Files To Add

- `app/playback/FfmpegDecoderSession.h`
- `app/playback/FfmpegDecoderSession.cpp`
- `test/playback/FfmpegDecoderSessionTest.cpp`

### Files To Modify

- `CMakeLists.txt`

### Required Changes

- Wrap `AVFormatContext`, `AVCodecContext`, `AVPacket`, `AVFrame`, and `SwrContext` with `std::unique_ptr` and custom deleters.
- Provide a small API:
  - `open(path)`
  - `close()`
  - `seek(ms)`
  - `flush()`
  - `readNextBlock()`
  - `streamInfo()`
- Internally hide `avcodec_send_packet()` and `avcodec_receive_frame()` semantics.
- Always normalize decode output to interleaved PCM.
- Use `libswresample` even for no-rate-change paths that still require planar-to-interleaved conversion.
- Support MP3, FLAC, and M4A.

### Key Class Skeleton

#### `app/playback/FfmpegDecoderSession.h`

```cpp
#pragma once

#include "PlaybackTypes.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <vector>

struct AVCodecContext;
struct AVFormatContext;
struct AVFrame;
struct AVPacket;
struct SwrContext;

namespace app::playback
{

  struct PcmBlock final
  {
    std::vector<std::byte> bytes;
    std::uint32_t frames = 0;
    std::uint64_t firstFrameIndex = 0;
    bool endOfStream = false;
  };

  struct DecodedStreamInfo final
  {
    StreamFormat sourceFormat;
    StreamFormat outputFormat;
    std::uint32_t durationMs = 0;
  };

  class FfmpegDecoderSession final
  {
  public:
    explicit FfmpegDecoderSession(StreamFormat outputFormat);
    ~FfmpegDecoderSession();

    void open(std::filesystem::path const& filePath);
    void close();
    void seek(std::uint32_t positionMs);
    void flush();

    std::optional<PcmBlock> readNextBlock();
    DecodedStreamInfo streamInfo() const;

  private:
    struct FormatContextDeleter
    {
      void operator()(AVFormatContext* ptr) const noexcept;
    };

    struct CodecContextDeleter
    {
      void operator()(AVCodecContext* ptr) const noexcept;
    };

    struct PacketDeleter
    {
      void operator()(AVPacket* ptr) const noexcept;
    };

    struct FrameDeleter
    {
      void operator()(AVFrame* ptr) const noexcept;
    };

    struct SwrContextDeleter
    {
      void operator()(SwrContext* ptr) const noexcept;
    };

    using FormatContextPtr = std::unique_ptr<AVFormatContext, FormatContextDeleter>;
    using CodecContextPtr = std::unique_ptr<AVCodecContext, CodecContextDeleter>;
    using PacketPtr = std::unique_ptr<AVPacket, PacketDeleter>;
    using FramePtr = std::unique_ptr<AVFrame, FrameDeleter>;
    using SwrContextPtr = std::unique_ptr<SwrContext, SwrContextDeleter>;

    void openInput(std::filesystem::path const& filePath);
    void openAudioStream();
    void configureResampler();
    std::optional<PcmBlock> drainDecoder();
    std::optional<PcmBlock> convertFrameToInterleavedPcm() const;

    StreamFormat _outputFormat;
    DecodedStreamInfo _streamInfo;
    FormatContextPtr _formatContext;
    CodecContextPtr _codecContext;
    PacketPtr _packet;
    FramePtr _frame;
    SwrContextPtr _swrContext;
    int _audioStreamIndex = -1;
    bool _inputEof = false;
    bool _decoderEof = false;
    std::uint64_t _decodedFrameCursor = 0;
  };

} // namespace app::playback
```

### Verification

- Decode test fixtures for MP3, FLAC, and M4A.
- Manual and automated seek validation.
- Confirm planar FLAC output becomes interleaved PCM.
- Run build in debug mode.
- Prefer validation under ASan or UBSan if available in the local build flow.

## Milestone 4: PCM Ring Buffer And Playback Engine

### Objective

Separate decode and audio output through a real-time-safe PCM pipeline.

### Files To Add

- `app/playback/PcmRingBuffer.h`
- `app/playback/PcmRingBuffer.cpp`
- `app/playback/PlaybackEngine.h`
- `app/playback/PlaybackEngine.cpp`
- `test/playback/PcmRingBufferTest.cpp`

### Required Changes

- Implement a fixed-capacity lock-free SPSC ring buffer for PCM bytes.
- Producer is the decode thread.
- Consumer is the audio backend callback or writer thread.
- Add playback state machine handling.
- Add decode thread lifecycle management.
- Add prebuffering threshold before entering `Playing`.
- Track underrun count and expose it in snapshots.

### Key Class Skeletons

#### `app/playback/PcmRingBuffer.h`

```cpp
#pragma once

#include <atomic>
#include <cstddef>
#include <memory>
#include <span>

namespace app::playback
{

  class PcmRingBuffer final
  {
  public:
    explicit PcmRingBuffer(std::size_t capacityBytes);

    std::size_t write(std::span<std::byte const> input) noexcept;
    std::size_t read(std::span<std::byte> output) noexcept;
    void clear() noexcept;

    std::size_t size() const noexcept;
    std::size_t capacity() const noexcept;
    std::size_t freeSpace() const noexcept;

  private:
    std::unique_ptr<std::byte[]> _storage;
    std::size_t _capacityBytes = 0;
    alignas(64) std::atomic<std::size_t> _head = 0;
    alignas(64) std::atomic<std::size_t> _tail = 0;
  };

} // namespace app::playback
```

#### `app/playback/PlaybackEngine.h`

```cpp
#pragma once

#include "FfmpegDecoderSession.h"
#include "IAudioBackend.h"
#include "PcmRingBuffer.h"
#include "PlaybackTypes.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>

namespace app::playback
{

  class PlaybackEngine final
  {
  public:
    explicit PlaybackEngine(std::unique_ptr<IAudioBackend> backend);
    ~PlaybackEngine();

    void play(TrackPlaybackDescriptor descriptor);
    void pause();
    void resume();
    void stop();
    void seek(std::uint32_t positionMs);

    PlaybackSnapshot snapshot() const;

  private:
    void openTrack(TrackPlaybackDescriptor descriptor);
    void stopDecodeThread();
    void decodeLoop(std::stop_token stopToken);

    std::unique_ptr<IAudioBackend> _backend;
    std::optional<FfmpegDecoderSession> _decoder;
    PcmRingBuffer _ringBuffer;
    std::jthread _decodeThread;

    mutable std::mutex _stateMutex;
    PlaybackSnapshot _snapshot;
    std::optional<TrackPlaybackDescriptor> _currentTrack;

    std::atomic<TransportState> _state = TransportState::Idle;
    std::atomic<std::uint32_t> _bufferedMs = 0;
    std::atomic<std::uint32_t> _underrunCount = 0;
  };

} // namespace app::playback
```

### Real-Time Restrictions

In the consumer audio path, do not allow:

- `std::mutex`
- condition variable waiting
- `new` or `delete`
- LMDB transactions
- FFmpeg calls
- GTK calls
- filesystem I/O

### Verification

- Ring buffer unit tests for empty, full, and wrap-around cases.
- Repeated `play -> stop -> play` cycles.
- Inject decode slowdown and verify underruns do not deadlock the app.
- App shutdown must not hang.

## Milestone 5: PipeWire Output Backend

### Objective

Implement the first real audio backend and make PipeWire the default playback path.

### Files To Add

- `app/playback/IAudioBackend.h`
- `app/playback/PipeWireBackend.h`
- `app/playback/PipeWireBackend.cpp`

### Files To Modify

- `CMakeLists.txt`
- `app/playback/PlaybackController.cpp`

### Required Changes

- Define a minimal backend interface.
- Open a PipeWire stream with explicit audio format metadata.
- Provide backend callbacks for PCM reads, underrun notifications, and frame progress.
- Keep callback implementation real-time-safe.
- Surface backend failures through controller snapshots.

### Key Class Skeletons

#### `app/playback/IAudioBackend.h`

```cpp
#pragma once

#include "PlaybackTypes.h"

#include <cstddef>
#include <cstdint>
#include <span>

namespace app::playback
{

  struct AudioRenderCallbacks final
  {
    void* userData = nullptr;
    std::size_t (*readPcm)(void* userData, std::span<std::byte> output) noexcept = nullptr;
    void (*onUnderrun)(void* userData) noexcept = nullptr;
    void (*onPositionAdvanced)(void* userData, std::uint32_t frames) noexcept = nullptr;
  };

  class IAudioBackend
  {
  public:
    virtual ~IAudioBackend() = default;

    virtual void open(StreamFormat const& format, AudioRenderCallbacks callbacks) = 0;
    virtual void start() = 0;
    virtual void pause() = 0;
    virtual void resume() = 0;
    virtual void flush() = 0;
    virtual void stop() = 0;
    virtual BackendKind kind() const noexcept = 0;
  };

} // namespace app::playback
```

#### `app/playback/PipeWireBackend.h`

```cpp
#pragma once

#include "IAudioBackend.h"

struct pw_context;
struct pw_stream;
struct pw_thread_loop;

namespace app::playback
{

  class PipeWireBackend final : public IAudioBackend
  {
  public:
    PipeWireBackend();
    ~PipeWireBackend() override;

    void open(StreamFormat const& format, AudioRenderCallbacks callbacks) override;
    void start() override;
    void pause() override;
    void resume() override;
    void flush() override;
    void stop() override;
    BackendKind kind() const noexcept override { return BackendKind::PipeWire; }

  private:
    static void onProcess(void* userData);
    void process();

    AudioRenderCallbacks _callbacks;
    StreamFormat _format;
    pw_thread_loop* _threadLoop = nullptr;
    pw_context* _context = nullptr;
    pw_stream* _stream = nullptr;
  };

} // namespace app::playback
```

### Verification

- Verify PipeWire backend with a synthetic source first.
- Then connect it to the ring buffer and real decode output.
- Test start, pause, resume, stop repeatedly.
- Show clear user-facing error if PipeWire is unavailable.

## Milestone 6: End-To-End GTK Integration

### Objective

Connect the playback bar, current selection, controller, engine, decoder, and PipeWire into the first truly usable player.

### Files To Modify

- `app/MainWindow.h`
- `app/MainWindow.cpp`
- `app/PlaybackBar.h`
- `app/PlaybackBar.cpp`

### Required Changes

- Bind playback bar signals to main window handlers.
- Resolve the current selected track into a descriptor when Play is requested.
- Connect each `TrackViewPage` activation signal to playback start.
- Poll `PlaybackSnapshot` on a GTK timer.
- Show current playback metadata and format information in the bar.
- Keep playback state separate from selection-driven cover art in the left panel.

### MainWindow Skeleton

#### `app/MainWindow.h`

```cpp
private:
  void setupPlayback();
  void bindTrackPagePlayback(TrackViewPage& page);
  void refreshPlaybackBar();
  std::optional<app::playback::TrackPlaybackDescriptor> currentSelectionPlaybackDescriptor() const;
  void playCurrentSelection();
  void pausePlayback();
  void stopPlayback();
  void seekPlayback(std::uint32_t positionMs);
```

#### `app/MainWindow.cpp`

```cpp
void MainWindow::setupPlayback()
{
  _playbackBar = std::make_unique<PlaybackBar>();
  _playbackController = std::make_unique<app::playback::PlaybackController>();

  _playbackBar->signalPlayRequested().connect(sigc::mem_fun(*this, &MainWindow::playCurrentSelection));
  _playbackBar->signalPauseRequested().connect(sigc::mem_fun(*this, &MainWindow::pausePlayback));
  _playbackBar->signalStopRequested().connect(sigc::mem_fun(*this, &MainWindow::stopPlayback));
  _playbackBar->signalSeekRequested().connect(sigc::mem_fun(*this, &MainWindow::seekPlayback));
}
```

### Verification

- Select a track and press Play to hear audio.
- Pause, resume, and stop work.
- Seek works.
- Switching list pages does not stop playback.
- Browsing other tracks does not overwrite playback bar state.

## Milestone 7: Format Negotiation, Fallback, And ALSA Exclusive Mode

### Objective

Add hardware capability probing, controlled fallback, and optional ALSA exclusive mode.

### Files To Add

- `app/playback/FormatNegotiator.h`
- `app/playback/FormatNegotiator.cpp`
- `app/playback/AlsaExclusiveBackend.h`
- `app/playback/AlsaExclusiveBackend.cpp`
- `test/playback/FormatNegotiatorTest.cpp`

### Files To Modify

- `app/playback/FfmpegDecoderSession.h`
- `app/playback/FfmpegDecoderSession.cpp`
- `app/playback/PlaybackController.cpp`

### Required Changes

- Probe ALSA hardware capabilities for sample rates, bit depths, and channel counts.
- Introduce a render plan describing source format, device format, and decoder output format.
- Do not resample unless the device requires it.
- For fallback, use high-quality `libswresample` settings first.
- Make ALSA exclusive an explicit mode, not silent fallback.
- On mode failure, report an error clearly instead of silently switching to PipeWire.

### Key Class Skeletons

#### `app/playback/FormatNegotiator.h`

```cpp
#pragma once

#include "PlaybackTypes.h"

#include <vector>

namespace app::playback
{

  struct DeviceCapabilities final
  {
    std::vector<std::uint32_t> sampleRates;
    std::vector<std::uint8_t> bitDepths;
    std::vector<std::uint8_t> channelCounts;
  };

  struct RenderPlan final
  {
    StreamFormat sourceFormat;
    StreamFormat deviceFormat;
    StreamFormat decoderOutputFormat;
    bool requiresResample = false;
    bool requiresBitDepthConversion = false;
    bool requiresChannelRemap = false;
    std::string reason;
  };

  class FormatNegotiator final
  {
  public:
    static RenderPlan buildPlan(StreamFormat sourceFormat, DeviceCapabilities const& caps);
  };

} // namespace app::playback
```

#### `app/playback/AlsaExclusiveBackend.h`

```cpp
#pragma once

#include "IAudioBackend.h"
#include "FormatNegotiator.h"

struct snd_pcm_t;

namespace app::playback
{

  class AlsaExclusiveBackend final : public IAudioBackend
  {
  public:
    explicit AlsaExclusiveBackend(std::string deviceName);
    ~AlsaExclusiveBackend() override;

    void open(StreamFormat const& format, AudioRenderCallbacks callbacks) override;
    void start() override;
    void pause() override;
    void resume() override;
    void flush() override;
    void stop() override;
    BackendKind kind() const noexcept override { return BackendKind::AlsaExclusive; }

    DeviceCapabilities queryCapabilities() const;

  private:
    std::string _deviceName;
    snd_pcm_t* _pcm = nullptr;
    AudioRenderCallbacks _callbacks;
    StreamFormat _format;
  };

} // namespace app::playback
```

### Verification

- Native-format playback succeeds when the device supports it.
- Unsupported sample rates enter controlled resample fallback.
- ALSA exclusive failures are explicit and visible.
- Cross-sample-rate playback transitions do not leave stale audio in buffers.

## Recommended Build Order

Recommended implementation order:

1. Milestone 1
2. Milestone 2
3. Milestone 3
4. Milestone 5
5. Milestone 4
6. Milestone 6
7. Milestone 7

This order front-loads user-visible structure and decode correctness, then validates PipeWire independently before fully locking down the decode-output pipeline.

## First Version Done Criteria

The first version is complete when all of the following are true:

- Playback bar is present between menu and content.
- A selected track can be played from the current page.
- MP3, FLAC, and M4A play correctly.
- Pause, resume, stop, and seek work.
- Playback survives page switching.
- The consumer audio path is real-time-safe.
- PipeWire is the default and stable backend.
- Playback state is shown in the UI without being overwritten by browsing state.

## Validation Commands

For milestones that touch buildable code, validate inside `nix-shell`:

```bash
nix-shell --run "cmake --preset linux-debug"
nix-shell --run "cmake --build --preset linux-debug --parallel"
```

When playback-related core logic is added, also run the test target once the new tests exist:

```bash
nix-shell --run "ctest --test-dir /tmp/build --output-on-failure"
```

## Notes On Non-Goals For V1

These are explicitly out of scope for the first working version:

- Next track navigation
- Previous track navigation
- Software volume
- ReplayGain
- Gapless playback
- Device selection UI
- PipeWire and ALSA parity in the first milestone set

These can be added later after the baseline player is stable.
