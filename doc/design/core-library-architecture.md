# Aobus Core Library (`lib/`) Architecture

## 1. Executive Summary

The Aobus core library (`lib/` and public API in `include/ao/`) forms the foundational engine of the Aobus ecosystem. Written in modern C++23, it encapsulates audio playback, robust metadata extraction, query execution, and lightning-fast local persistence. The architecture is strictly decoupled from any UI framework, enabling both the GTK4 desktop application and CLI tools to drive the engine natively.

## 2. Core Architectural Principles

1.  **Strict PImpl & Facade Pattern:** High-level entities like `ao::audio::Player` and `ao::library::MusicLibrary` expose clean interfaces hiding all low-level dependencies via the Pointer-to-Implementation (PImpl) idiom. This ensures a stable ABI and dramatic reduction in compile times.
2.  **Zero-Copy IO & Memory Mapping:** Performance is treated as a first-class citizen. Disk I/O leverages memory mapping (`boost::interprocess::file_mapping`) and raw byte spans (`std::span<std::byte const>`) to parse tags without unnecessary heap allocations.
3.  **Reactive Paradigm:** Frontend state is driven by a reactive abstraction layer. The library provides `Observable` primitives to which UI components can bind, ensuring the UI remains perfectly synchronized with the underlying database state automatically.
4.  **Lock-Free Real-Time Audio:** Passing audio data from decoding threads to OS audio server callbacks avoids mutexes entirely, utilizing single-producer single-consumer lock-free ring buffers to prevent audio dropouts.

---

## 3. Component Deep-Dive & Implementation Highlights

### 3.1. Audio Engine & Decoding (`ao::audio`)

The audio module is responsible for the entire DSP lifecycle, from disk reading to pushing PCM frames to hardware backends (ALSA, PipeWire).

**Implementation Highlights:**
*   **`PcmRingBuffer`**: To safely cross the thread boundary between the non-real-time decoder loop and the real-time backend callback, Aobus uses a highly optimized SPSC (Single-Producer Single-Consumer) lock-free queue based on `boost::lockfree::spsc_queue`. It handles raw bytes to cleanly support varying bit depths (16/24/32-bit).
*   **Modular Backends**: Backends inherit from `IBackendProvider`. The `Player` seamlessly manages multiple providers concurrently, negotiating format capabilities (`FormatNegotiator`) before activating a routing graph (`flow::Graph`).

### 3.2. Music Library & Persistence (`ao::library`, `ao::lmdb`)

Aobus persists tracks, playlists, and resource binaries inside the Symas LMDB (Lightning Memory-Mapped Database) key-value store. LMDB provides ACID semantics, multi-version concurrency control (MVCC), and high read performance.

**Implementation Highlights:**
*   **RAII Transactions (`ao::lmdb`)**: LMDB handles are wrapped in safe C++ classes (`Environment`, `Database`). Transactions (`ReadTransaction`, `WriteTransaction`) utilize `std::unique_ptr` with custom deleters (e.g., `mdb_txn_abort`) ensuring that uncommitted transactions are cleanly rolled back during stack unwinding.
*   **Segmented Stores**: Data is horizontally partitioned into specialized stores (`TrackStore` for metadata, `ListStore` for playlists, `ResourceStore` for art cache) all coordinated by the central `MusicLibrary` facade.

### 3.3. Tag Parsing & Demuxing (`ao::tag`, `ao::media`)

Extracting metadata from FLAC, MP4, and MP3 files must be exceedingly fast to handle large library imports seamlessly.

**Implementation Highlights:**
*   **`MappedFile`**: A safe wrapper around Boost Interprocess allows entire media files to be read as a `std::span<std::byte const>`. This permits "zero-copy" tag parsing where string views directly point to memory-mapped disk blocks.
*   **`gperf` Perfect Hashing**: To route metadata fields quickly (e.g., ID3v2 frames, MP4 Atoms, Vorbis Comments), Aobus avoids slow `std::map` lookups. Instead, it compiles `.gperf` definitions into O(1) perfect hash functions. (See: `VorbisCommentDispatch.gperf`, `AtomDispatch.gperf`).

### 3.4. Query Engine & Smart Lists (`ao::query`)

Aobus features a custom query language to generate dynamic playlists ("Smart Lists") like `Year > 2010 AND Genre == "Jazz"`.

**Implementation Highlights:**
*   **Custom AST (`Expression`)**: The parser tokenizes raw strings into a strict Abstract Syntax Tree (AST) constructed from `std::variant<VariableExpression, ConstantExpression, std::unique_ptr<BinaryExpression>, std::unique_ptr<UnaryExpression>>`.
*   **`PlanEvaluator`**: The AST is compiled into an `ExecutionPlan`. The query engine executes this plan natively against the LMDB data using high-speed property dispatches (again accelerated by `gperf`).

### 3.5. Reactive Data Model (`ao::reactive`, `ao::model`)

To bridge the backend LMDB data seamlessly to frontend grids and list views, the `model` module utilizes a publish-subscribe methodology.

**Implementation Highlights:**
*   **`Observerable<Id, Args...>`**: A foundational template class allowing observers to attach to lifecycle events like `beginInsert`, `endUpdate`, and `beginRemove`.
*   **`SmartListEngine`**: This acts as the glue between the `query` module and the `reactive` lists. It tracks mutations in the `TrackStore`, re-evaluates expressions against modified tracks, and dispatches UI updates atomically.

---

## 4. Key Workflows

### 4.1. The Audio Playback Lifecycle
1.  The UI requests playback via `Player::play(descriptor)`.
2.  The `DecoderFactory` instantiates the corresponding `IDecoderSession` (e.g., `FlacDecoderSession`).
3.  `FormatNegotiator` calculates the optimal bit depth and sample rate between the decoder output and the hardware backend capabilities.
4.  The active `Backend` runs a real-time thread, constantly polling the `PcmRingBuffer`.
5.  A secondary worker thread pumps frames from the decoder into the `PcmRingBuffer` until playback completes.

### 4.2. Background Library Ingestion
1.  The `ImportWorker` spins up a background thread to recursively crawl the target directory.
2.  Each identified audio file is opened via `MappedFile`.
3.  The `tag` module utilizes perfect hashing (`gperf`) to rip metadata instantly directly from mapped memory.
4.  Track models are constructed via `TrackBuilder`.
5.  Batches of tracks are grouped into an `ao::lmdb::WriteTransaction` and committed to the `TrackStore`.
6.  The commit triggers the `Observable` systems, notifying the UI to redraw the library grids without freezing the main application thread.