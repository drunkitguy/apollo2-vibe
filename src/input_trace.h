/**
 * @file src/input_trace.h
 * @brief Opt-in per-event host input latency trace, host half of the input round-trip probe.
 *
 * Companion to `frame_trace`, and deliberately parasitic on it: the reference clock and the
 * clock offset estimation are `frame_trace`'s, so host and client input timestamps land in the
 * same domain as the video ones with no second synchronisation mechanism.
 *
 * The wire contract lives in `artifacts/wire-contract-frame-trace.md` §8 and is shared with the
 * client agent. Nothing here may change without updating that section.
 *
 * ## What this measures, and where it stops
 *
 * A probe (`0x3030`) arrives on the control channel immediately before the input packet it
 * marks. The host stamps four instants:
 *
 *   1. `host_rx`          the probe was read off the control channel
 *   2. `host_dispatch`    the marked input left the input queue on the pool thread
 *   3. `host_inject_call` immediately before the OS injection call
 *   4. `host_inject_ret`  immediately after it returned
 *
 * Only (1) and (3) go on the wire, as `hostRecvTimeUs` and `hostInjectTimeUs`. All four go in
 * the CSV.
 *
 * **The measurement ends at the injection call, not at the game.** After
 * `vigem_target_x360_update()` returns, the ViGEm bus driver still has to carry the report
 * through HIDClass and the game still has to poll XInput. That stage is real and is not
 * observable from user space. It is recorded in the CSV metadata as explicitly unmeasured and
 * is never folded into another number.
 *
 * ## Coalescing, and why `coalesced_count` exists
 *
 * `passthrough_next_message()` merges batchable input already sitting in the queue into the
 * entry it is about to inject (src/input.cpp), and gamepad state is batchable. So one injection
 * can represent several client input packets, and `host_inject_call` is then the injection of
 * the *batch* containing the marked packet. `coalesced_count` reports how many entries were
 * merged so a consumer can tell a clean per-packet sample (1) from a batch figure (>1). Without
 * it the field would read as per-packet and quietly mean something narrower under load.
 *
 * ## Thread ownership
 *
 * | State | Written by | Read by | Discipline |
 * |---|---|---|---|
 * | `detail::active` | control thread, session start/end | all | `std::atomic`, relaxed |
 * | pending probe | control stream thread only | control stream thread only | `pending_mtx` |
 * | echo sink | control stream thread | task pool thread | `sink_mtx` |
 * | recorder ring | task pool thread | control thread at flush | `recorder.mtx` |
 *
 * The pending probe is only ever touched on the control stream thread — both the `0x3030`
 * handler and `input::passthrough()` run there — so the ordering "probe, then the input packet
 * it marks" is exactly wire order. The mutex guards session teardown, not a data race between
 * peers.
 */
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>

#include <boost/endian/arithmetic.hpp>

#include "frame_trace.h"

namespace input_trace {

  /// Client advertises this in `x-ml-general.featureFlags` when it can send input probes.
  constexpr std::uint32_t CLIENT_FF_INPUT_PROBE = 0x10;

  /// Host advertises this in `x-ss-general.featureFlags` when the input trace is enabled.
  constexpr std::uint32_t HOST_FF_INPUT_PROBE = 0x10;

  /// Client to host. Marks the input packet that follows it on the same channel.
  constexpr std::uint16_t INPUT_PROBE_PTYPE = 0x3030;

  /// Host to client. Echoes the probe with the host's receive and injection instants.
  constexpr std::uint16_t INPUT_PROBE_ECHO_PTYPE = 0x3031;

  /// Only version the wire contract defines. Anything else is dropped by both sides.
  constexpr std::uint8_t INPUT_PROBE_VERSION = 1;

  /// Echo flag bit 0: no input packet was associated with this probe, `host_inject_us` is 0.
  constexpr std::uint8_t ECHO_FLAG_NO_INPUT = 0x01;

  /// `coalesced_count` value meaning the host could not determine the merge count.
  constexpr std::uint8_t COALESCED_UNKNOWN = 0;

#pragma pack(push, 1)

  /**
   * @brief `0x3030` body. Client to host, 24 bytes, little-endian.
   * @details Ratified from the client's proposal without change.
   */
  struct input_probe_t {
    std::uint8_t version;  ///< INPUT_PROBE_VERSION
    std::uint8_t flags;  ///< bit0: the client's batching limiter delayed this event
    boost::endian::little_uint16_t reserved;  ///< 0, ignored
    boost::endian::little_uint32_t sequence_number;  ///< Monotonic per session, starts at 1
    boost::endian::little_uint64_t client_event_us;  ///< Kernel event time, client clock
    boost::endian::little_uint64_t client_send_us;  ///< Immediately before enqueue, client clock
  };

  static_assert(
    sizeof(input_probe_t) == 24,
    "input_probe_t must stay 24 bytes; the client parses a fixed length (contract §8)"
  );

  /**
   * @brief `0x3031` body. Host to client, 24 bytes, little-endian.
   * @details `coalesced_count` is the host's one addition to the client's proposal. It takes a
   *          byte from the proposed `uint16_t reserved`, so `sequence_number` still sits at
   *          offset 4 and both 64-bit fields at 8 and 16. Nothing realigned.
   */
  struct input_probe_echo_t {
    std::uint8_t version;  ///< INPUT_PROBE_VERSION
    std::uint8_t flags;  ///< ECHO_FLAG_* bits
    std::uint8_t coalesced_count;  ///< 1 = clean sample, >1 = batch, 0 = unknown
    std::uint8_t reserved;  ///< 0, ignored
    boost::endian::little_uint32_t sequence_number;  ///< Echoed unmodified
    boost::endian::little_uint64_t host_rx_us;  ///< Probe read off the control channel
    boost::endian::little_uint64_t host_inject_us;  ///< Immediately before the OS injection call
  };

  static_assert(
    sizeof(input_probe_echo_t) == 24,
    "input_probe_echo_t must stay 24 bytes; the client parses a fixed length (contract §8)"
  );

#pragma pack(pop)

  /**
   * @brief A probe waiting to be attached to the next input packet.
   * @details Copied, never referenced across threads. `valid` false means "no probe pending",
   *          which is the common case since the client sends at most 20 per second.
   */
  struct pending_t {
    bool valid = false;
    std::uint32_t sequence_number = 0;
    std::int64_t client_event_us = 0;
    std::int64_t client_send_us = 0;
    std::int64_t host_rx_us = 0;
  };

  /// Called on the task pool thread with a fully populated echo body, to be sent by the owner.
  using echo_fn = std::function<void(const input_probe_echo_t &)>;

  namespace detail {
    /// Read through `enabled()`. See the thread ownership table above.
    extern std::atomic<bool> active;
  }  // namespace detail

  /**
   * @brief Whether an input trace session is currently recording.
   * @return `true` if the trace hooks will do anything.
   */
  inline bool enabled() {
    return detail::active.load(std::memory_order_relaxed);
  }

  /**
   * @brief Current value of the host reference clock.
   * @details Deliberately delegates to `frame_trace::now_us()` so input and video stamps share
   *          one clock and the client's existing §6 offset converts both.
   * @return Steady clock microseconds.
   */
  inline std::int64_t now_us() {
    return frame_trace::now_us();
  }

  /**
   * @brief Metadata block for the CSV header.
   */
  struct metadata_t {
    std::string client_name;
    std::string gamepad_type;  ///< "x360", "ds4" or "none", as configured
    int capacity = 0;
  };

  /**
   * @brief Start recording, allocating the ring buffer up front.
   * @details Does nothing when `config::input.input_trace` is disabled.
   * @param meta Session parameters for the CSV metadata block.
   * @return `true` if this caller now owns the trace and must call `end_session()`.
   */
  bool begin_session(const metadata_t &meta);

  /**
   * @brief Stop recording and write the ring buffer out as CSV.
   * @details Safe to call when no session was started. Emits a no-input echo for any probe
   *          still pending so the client is never left waiting on a sequence number.
   */
  void end_session();

  /**
   * @brief Register the sink that delivers echoes back to the client.
   * @details Called on the control stream thread at session setup. The sink is invoked on the
   *          task pool thread, so it must be safe to call from there — in practice it raises on
   *          a `safe::mail` queue that the control thread drains.
   * @param fn Sink, or `nullptr` to detach.
   */
  void set_echo_sink(echo_fn fn);

  /**
   * @brief Record a probe as pending, to be attached to the next input packet.
   * @details Control stream thread only. If a probe is already pending it is superseded, and
   *          the superseded one is echoed immediately with `ECHO_FLAG_NO_INPUT` so that every
   *          probe the client sends produces exactly one echo.
   * @param probe Parsed probe body.
   * @param host_rx_us Instant the probe was read, taken before any other work.
   */
  void mark_next_input(const input_probe_t &probe, std::int64_t host_rx_us);

  /**
   * @brief Detach the pending probe, if any, to attach it to an input packet being queued.
   * @details Control stream thread only, called from `input::passthrough()`.
   * @return The pending probe, or a `pending_t` with `valid == false`.
   */
  pending_t take_pending();

  /**
   * @brief Record one completed input event and echo it to the client.
   * @details Task pool thread only. Cheap no-op when tracing is off or `probe.valid` is false.
   * @param probe The probe attached to this input, from `take_pending()`.
   * @param magic The input packet's `NV_INPUT_HEADER::magic`, for attributing by event type.
   * @param dispatch_us Instant the entry left the input queue.
   * @param inject_call_us Instant immediately before the OS injection call.
   * @param inject_return_us Instant immediately after the OS injection call returned.
   * @param coalesced_count Entries merged into this injection, 1 when nothing was merged.
   */
  void complete(
    const pending_t &probe,
    std::uint32_t magic,
    std::int64_t dispatch_us,
    std::int64_t inject_call_us,
    std::int64_t inject_return_us,
    std::uint8_t coalesced_count
  );

}  // namespace input_trace
