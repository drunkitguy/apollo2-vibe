/**
 * @file src/frame_trace.h
 * @brief Declarations for the per-frame latency trace recorder.
 */
#pragma once

// standard includes
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>

// lib includes
#include <boost/endian/arithmetic.hpp>

/**
 * @brief Per-frame host-side latency instrumentation.
 *
 * The recorder collects one record per encoded video frame, keeps them in a fixed-size
 * in-memory ring buffer and writes the whole buffer out as CSV once, when the session
 * ends. Nothing touches the disk while a stream is running.
 *
 * The same host timestamps are also handed to the client in an extension on the video
 * frame header, so that a client-side trace can be joined against the host-side CSV on
 * the frame index. That extension is only ever sent to a client that advertised
 * `ML_FF_LATENCY_TRACE`; see `stream::config_t::frameTrace`.
 *
 * Everything here is inert unless `config::video.frame_trace` is set. When it isn't, the
 * cost on the streaming threads is a single relaxed load of `detail::active` per frame.
 *
 * ### Thread ownership
 *
 * - `detail::active` is written only by `begin_session()` / `end_session()`, which are
 *   called from the session start/teardown thread. Every other thread only reads it.
 * - `host_stamps_t` instances are not shared: the capture/encode thread fills one, moves
 *   it into `video::packet_raw_t`, and ownership travels with the packet to the video
 *   broadcast thread. No two threads ever touch the same instance.
 * - The ring buffer and the metadata block live in a single file-static object in
 *   frame_trace.cpp guarded by its own mutex. `submit()` (video broadcast thread) is the
 *   only producer; `begin_session()` and `end_session()` (session start/teardown thread)
 *   are the only other users and take the same mutex.
 */
namespace frame_trace {

  /**
   * @brief Host-side stage timestamps for one video frame.
   * @details Steady clock microseconds, matching the unit the client uses on the wire.
   *          Zero means the stage was not stamped, which happens for the stages a given
   *          capture path doesn't have a hook for; the synchronous capture path has no
   *          `capture_requested`, and a repeated frame has no `capture_complete`.
   *          Consumers must treat zero as missing rather than as a timestamp.
   * @details `capture_complete` comes from the capture backend and is the moment the
   *          frame was actually produced, while `capture_requested` is the moment the
   *          encode thread asked for the next frame. The two pipelines run independently,
   *          so a frame that was already waiting in the queue makes
   *          `capture_complete - capture_requested` negative. That is expected and means
   *          the encoder did not wait, not that a clock went backwards.
   */
  struct host_stamps_t {
    std::int64_t capture_requested = 0;
    std::int64_t capture_complete = 0;
    std::int64_t encode_submit = 0;
    std::int64_t encode_complete = 0;

    /// The moment the encoded frame was handed to the transmit pipeline, taken just before
    /// packetisation. This is what goes on the wire as `tx_pipeline_entry_us`, because the
    /// frame header is covered by the FEC parity computed during packetisation and so
    /// cannot be amended once the first packet has actually left. It is **not** the first
    /// packet's transmit time and the gap is not small — see the note on
    /// `frame_timestamp_ext_t` for what sits in between. The true first-send timestamp is
    /// recorded separately in the host CSV, which also reports the measured distribution of
    /// the difference.
    std::int64_t tx_pipeline_entry = 0;
  };

  /**
   * @brief Stream parameters recorded once per session, as required by the harness
   *        metadata block. A trace without these is not comparable against another run.
   */
  struct metadata_t {
    int width = 0;
    int height = 0;
    int framerate = 0;
    int bitrate_kbps = 0;
    int video_format = 0;  ///< 0 - H.264, 1 - HEVC, 2 - AV1
    int chroma_sampling_type = 0;  ///< 0 - 4:2:0, 1 - 4:4:4
    int dynamic_range = 0;  ///< 0 - 8-bit, 1 - 10-bit
    int slices_per_frame = 0;
    int num_ref_frames = 0;
    int encoder_csc_mode = 0;
    int intra_refresh = 0;
    std::string encoder;  ///< Encoder actually in use, e.g. "nvenc"
    std::string capture;  ///< Configured capture method, empty when autodetected
    std::string client;  ///< Client device name

    /// Whether the client negotiated the in-band extension at all. False means this run
    /// produced host columns only, with nothing for the client to join against.
    bool trace_ext_negotiated = false;

    /// Extension version agreed with the client. A joined dataset is uninterpretable
    /// without it, since two revisions of the same 48 bytes exist.
    int trace_ext_version = 0;
  };

  // Everything below this point is protocol. It mirrors moonlight-common-c
  // `src/Video.h` and `src/Limelight-internal.h` on the matching client branch and must
  // not be changed on one side alone.

  /// `x-ml-general.featureFlags` bit: the client understands the frame timestamp
  /// extension and the clock sync messages. Mirrors ML_FF_LATENCY_TRACE.
  constexpr std::uint32_t CLIENT_FF_LATENCY_TRACE = 0x04;

  /// `x-ss-general.featureFlags` bit: the host emits the extension and answers clock
  /// sync. Mirrors SS_FF_LATENCY_TRACE.
  constexpr std::uint32_t HOST_FF_LATENCY_TRACE = 0x04;

  /// Stock short frame header discriminator, an 8 byte header.
  constexpr std::uint8_t FRAME_HDR_DISC_SHORT = 0x01;

  /// Short frame header followed by `frame_timestamp_ext_t`. Must never be emitted to a
  /// client that did not advertise ML_FF_LATENCY_TRACE: such a client sizes the header
  /// from this byte and would mis-parse the frame.
  constexpr std::uint8_t FRAME_HDR_DISC_SHORT_TRACE = 0x02;

  /**
   * @brief Range of `frame_timestamp_ext_t::ext_version` this host can emit.
   * @details Version 1 is the original layout: no validity mask, and the last timestamp
   *          named `firstPacketTxUs`. Version 2 carves `validity_mask` out of the first
   *          reserved byte and renames that timestamp to `tx_pipeline_entry_us`, which is
   *          what it always actually held. Both are 48 bytes, so the version only changes
   *          how the bytes are read, never where the picture data starts.
   * @details The version is negotiated, not assumed: the peer states its maximum in the
   *          RTSP handshake and the host emits `min(peer, ours)`. A peer that says nothing
   *          is treated as version 1, so a client predating the attribute still gets usable
   *          host timestamps instead of an extension it will reject wholesale.
   */
  constexpr std::uint8_t FRAME_TIMESTAMP_EXT_VERSION_MIN = 1;
  constexpr std::uint8_t FRAME_TIMESTAMP_EXT_VERSION_MAX = 2;

  /// Version assumed for a peer that advertises no `traceExtVersion` attribute at all.
  constexpr std::uint8_t FRAME_TIMESTAMP_EXT_VERSION_ASSUMED = 1;

  /// First version carrying `validity_mask`. Below this the byte must be left zero.
  constexpr std::uint8_t FRAME_TIMESTAMP_EXT_VERSION_VALIDITY_MASK = 2;

  /**
   * @brief Bits in `frame_timestamp_ext_t::validity_mask`.
   * @details A clear bit means the host had no measurement for that stage on this frame,
   *          not that the stage took zero time. Consumers must skip an unflagged field
   *          rather than converting it; zero is a legal steady-clock value and there is no
   *          in-band sentinel for "missing".
   */
  constexpr std::uint8_t STAMP_VALID_CAPTURE_REQUESTED = 0x01;
  constexpr std::uint8_t STAMP_VALID_CAPTURE_COMPLETE = 0x02;
  constexpr std::uint8_t STAMP_VALID_ENCODE_SUBMIT = 0x04;
  constexpr std::uint8_t STAMP_VALID_ENCODE_COMPLETE = 0x08;
  constexpr std::uint8_t STAMP_VALID_TX_PIPELINE_ENTRY = 0x10;

  /// Control stream message types. Client sends the request, host answers with the
  /// response. Apollo's own extensions already occupy 0x3000-0x3002.
  constexpr std::uint16_t CLOCK_SYNC_REQUEST_PTYPE = 0x3010;
  constexpr std::uint16_t CLOCK_SYNC_RESPONSE_PTYPE = 0x3011;

#pragma pack(push, 1)

  /**
   * @brief In-band host timestamps, appended to the SOF frame header. SS_FRAME_TIMESTAMP_EXT.
   * @details Little-endian, host steady clock microseconds. `frame_index` is echoed so the
   *          client can reject a torn or mis-parsed extension instead of emitting a bogus
   *          trace row. `validity_mask` says which of the timestamps were actually measured
   *          for this frame; the host has no value for some stages on some frames and there
   *          is no timestamp it could substitute that would not be a fabrication.
   * @details `tx_pipeline_entry_us` is **not** the time the first packet left the host. It
   *          is the last instant at which this header can still be written, because
   *          everything after it is covered by the FEC parity computed during
   *          packetisation. Between it and the real first send the host generates
   *          Reed-Solomon parity for the whole frame, AES-GCM encrypts every shard when
   *          video encryption is on, and may sleep for intra-frame pacing. The true value
   *          is in the host CSV as `t_first_packet_tx`, together with per-session
   *          statistics of the difference.
   */
  struct frame_timestamp_ext_t {
    std::uint8_t ext_version;
    std::uint8_t validity_mask;
    std::uint8_t reserved[2];
    boost::endian::little_uint32_t frame_index;
    boost::endian::little_uint64_t capture_requested_us;
    boost::endian::little_uint64_t capture_complete_us;
    boost::endian::little_uint64_t encode_submit_us;
    boost::endian::little_uint64_t encode_complete_us;
    boost::endian::little_uint64_t tx_pipeline_entry_us;
  };

  static_assert(
    sizeof(frame_timestamp_ext_t) == 48,
    "Frame timestamp extension must be 48 bytes"
  );

  /// SS_CLOCK_SYNC_REQUEST. Client to host, `client_tx_us` is t1.
  struct clock_sync_request_t {
    boost::endian::little_uint32_t sequence_number;
    boost::endian::little_uint32_t reserved;
    boost::endian::little_uint64_t client_tx_us;
  };

  static_assert(
    sizeof(clock_sync_request_t) == 16,
    "Clock sync request must be 16 bytes"
  );

  /**
   * @brief SS_CLOCK_SYNC_RESPONSE. Host to client.
   * @details `client_tx_us` is t1 echoed verbatim, `host_rx_us` is t2 and `host_tx_us`
   *          is t3. Supplying both host timestamps lets the client subtract our own
   *          handling delay from the round trip instead of folding it into the RTT.
   */
  struct clock_sync_response_t {
    boost::endian::little_uint32_t sequence_number;
    boost::endian::little_uint32_t reserved;
    boost::endian::little_uint64_t client_tx_us;
    boost::endian::little_uint64_t host_rx_us;
    boost::endian::little_uint64_t host_tx_us;
  };

  static_assert(
    sizeof(clock_sync_response_t) == 32,
    "Clock sync response must be 32 bytes"
  );

#pragma pack(pop)

  namespace detail {
    /// Read through `enabled()`. See the thread ownership note above.
    extern std::atomic<bool> active;
  }  // namespace detail

  /**
   * @brief Whether a trace session is currently recording.
   * @return `true` if `submit()` will do anything.
   */
  inline bool enabled() {
    return detail::active.load(std::memory_order_relaxed);
  }

  /**
   * @brief Current value of the host reference clock.
   * @return Steady clock microseconds. Never wall clock, so it is immune to NTP steps.
   */
  std::int64_t now_us();

  /**
   * @brief Choose the frame timestamp extension version to emit for a peer.
   * @details `min(peer, ours)`, floored at the oldest version we can still speak. A peer
   *          that advertised nothing arrives here as
   *          `FRAME_TIMESTAMP_EXT_VERSION_ASSUMED` and gets version 1, which it can read,
   *          rather than a newer extension it would reject in full.
   * @param peer_max_version Highest version the peer says it understands.
   * @return Version to write into `frame_timestamp_ext_t::ext_version`.
   */
  std::uint8_t negotiate_ext_version(int peer_max_version);

  /**
   * @brief Which stages of `stamps` carry a real measurement.
   * @details Extracted from the packetisation path so the bit assignment is testable on its
   *          own; the caller only copies the result into the extension.
   * @param stamps Stage timestamps for one frame. Zero means the stage was never stamped.
   * @return `STAMP_VALID_*` bits ORed together.
   */
  std::uint8_t validity_mask_for(const host_stamps_t &stamps);

  /**
   * @brief Start recording, allocating the ring buffer up front.
   * @details Does nothing when `config::video.frame_trace` is disabled. Starting a second
   *          session while one is active is refused and logged, because the single global
   *          recorder cannot attribute records to a session.
   * @param meta Stream parameters for the CSV metadata block.
   * @return `true` if this caller now owns the trace and must call `end_session()`.
   */
  bool begin_session(const metadata_t &meta);

  /**
   * @brief Stop recording and write the ring buffer out as CSV.
   * @details Safe to call when no session was started. Returns after the file is closed.
   */
  void end_session();

  /**
   * @brief Append one completed frame record.
   * @details Called from the video broadcast thread once the frame's first packet has
   *          been handed to the socket. Cheap no-op when tracing is off.
   * @param frame_index Full 64-bit host frame index, free of the 32-bit wire wraparound.
   * @param stamps Capture and encode stage timestamps carried with the packet.
   * @param t_first_packet_tx Steady clock microseconds, taken right before the first send.
   * @param frame_bytes Size of the encoded frame before packetisation.
   * @param idr Whether the frame is an IDR.
   */
  void submit(std::int64_t frame_index, const host_stamps_t &stamps, std::int64_t t_first_packet_tx, std::size_t frame_bytes, bool idr);

}  // namespace frame_trace
