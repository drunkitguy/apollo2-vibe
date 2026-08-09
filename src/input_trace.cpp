/**
 * @file src/input_trace.cpp
 * @brief Host half of the input round-trip probe. See input_trace.h for the design and the
 *        thread ownership table, and artifacts/wire-contract-frame-trace.md §8 for the wire.
 */
// this include
#include "input_trace.h"

// standard includes
#include <algorithm>
#include <chrono>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>

// local includes
#include "config.h"
#include "logging.h"

namespace fs = std::filesystem;
using namespace std::literals;

namespace input_trace {

  namespace detail {
    std::atomic<bool> active {false};
  }  // namespace detail

  namespace {

    /**
     * @brief One recorded input event.
     * @details Plain and trivially copyable so the ring can be memset and written without
     *          touching the heap on the injection thread.
     */
    struct record_t {
      std::uint32_t sequence_number;
      std::uint32_t magic;  ///< NV_INPUT_HEADER::magic, so events can be split by type
      std::int64_t client_event_us;  ///< Client clock, not converted here
      std::int64_t client_send_us;  ///< Client clock, not converted here
      std::int64_t host_rx_us;
      std::int64_t host_dispatch_us;
      std::int64_t host_inject_call_us;
      std::int64_t host_inject_return_us;
      std::uint8_t coalesced_count;
    };

    struct {
      std::mutex mtx;
      std::vector<record_t> ring;
      std::size_t next = 0;  ///< Index of the next slot to write
      std::size_t written = 0;  ///< Total records submitted, may exceed ring.size()
      metadata_t meta;
      std::int64_t epoch_us = 0;  ///< Reference clock when the session began
      std::time_t epoch_wall = 0;  ///< Wall clock at the same instant, to correlate runs
    } recorder;

    /// Guards the single pending probe. Touched only on the control stream thread; the mutex
    /// exists so session teardown cannot race an in-flight input packet.
    std::mutex pending_mtx;
    pending_t pending;

    /// Guards the echo sink. Set on the control thread, invoked on the task pool thread.
    std::mutex sink_mtx;
    echo_fn sink;

    /**
     * @brief Deliver an echo through the registered sink, if there is one.
     * @details Copies the sink out from under the lock so the send cannot deadlock against a
     *          concurrent set_echo_sink().
     */
    void emit_echo(const input_probe_echo_t &echo) {
      echo_fn fn;
      {
        std::lock_guard lg {sink_mtx};
        fn = sink;
      }
      if (fn) {
        fn(echo);
      }
    }

    /**
     * @brief Echo a probe that never got an input packet attached.
     * @param sequence_number Probe sequence to echo back.
     * @param host_rx_us When the probe was read.
     */
    void emit_orphan_echo(std::uint32_t sequence_number, std::int64_t host_rx_us) {
      input_probe_echo_t echo {};
      echo.version = INPUT_PROBE_VERSION;
      echo.flags = ECHO_FLAG_NO_INPUT;
      echo.coalesced_count = COALESCED_UNKNOWN;
      echo.reserved = 0;
      echo.sequence_number = sequence_number;
      echo.host_rx_us = static_cast<std::uint64_t>(host_rx_us);
      echo.host_inject_us = 0;
      emit_echo(echo);
    }

    /**
     * @brief Build the output path for this run.
     * @details Same convention as the frame trace: the configured value names a file and the
     *          session's wall clock time is inserted before the extension, so consecutive runs
     *          do not overwrite each other.
     */
    fs::path build_output_path(std::time_t when) {
      fs::path configured {config::input.input_trace_path};
      if (configured.empty()) {
        configured = "input_trace.csv";
      }

      char stamp[32] {};
      std::tm tm {};
#ifdef _WIN32
      localtime_s(&tm, &when);
#else
      localtime_r(&when, &tm);
#endif
      std::strftime(stamp, sizeof(stamp), "%Y%m%d_%H%M%S", &tm);

      auto stem = configured.stem().string() + "_" + stamp;
      return configured.parent_path() / (stem + configured.extension().string());
    }

    /**
     * @brief Format a wall clock instant for the CSV metadata block.
     */
    std::string wall_clock_string(std::time_t when) {
      char buf[32] {};
      std::tm tm {};
#ifdef _WIN32
      localtime_s(&tm, &when);
#else
      localtime_r(&when, &tm);
#endif
      std::strftime(buf, sizeof(buf), "%FT%T", &tm);
      return buf;
    }

  }  // namespace

  bool begin_session(const metadata_t &meta) {
    if (!config::input.input_trace) {
      return false;
    }

    std::lock_guard lg {recorder.mtx};
    if (detail::active.load(std::memory_order_relaxed)) {
      BOOST_LOG(warning) << "Input trace: a session is already recording, refusing to start a second"sv;
      return false;
    }

    auto capacity = config::input.input_trace_capacity;
    recorder.ring.assign(static_cast<std::size_t>(capacity), record_t {});
    recorder.next = 0;
    recorder.written = 0;
    recorder.meta = meta;
    recorder.meta.capacity = capacity;
    recorder.epoch_us = now_us();
    recorder.epoch_wall = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());

    {
      std::lock_guard pg {pending_mtx};
      pending = {};
    }

    detail::active.store(true, std::memory_order_relaxed);
    BOOST_LOG(info) << "Input trace: recording, capacity "sv << capacity << " events"sv;
    return true;
  }

  void set_echo_sink(echo_fn fn) {
    std::lock_guard lg {sink_mtx};
    sink = std::move(fn);
  }

  void mark_next_input(const input_probe_t &probe, std::int64_t host_rx_us) {
    if (!enabled()) {
      return;
    }

    pending_t superseded;
    {
      std::lock_guard lg {pending_mtx};
      superseded = pending;

      pending.valid = true;
      pending.sequence_number = probe.sequence_number;
      pending.client_event_us = static_cast<std::int64_t>(probe.client_event_us);
      pending.client_send_us = static_cast<std::int64_t>(probe.client_send_us);
      pending.host_rx_us = host_rx_us;
    }

    // Every probe the client sends gets exactly one echo. A probe that never had an input
    // packet follow it would otherwise leave the client waiting on a sequence number forever.
    if (superseded.valid) {
      emit_orphan_echo(superseded.sequence_number, superseded.host_rx_us);
    }
  }

  pending_t take_pending() {
    if (!enabled()) {
      return {};
    }

    std::lock_guard lg {pending_mtx};
    auto taken = pending;
    pending = {};
    return taken;
  }

  void complete(
    const pending_t &probe,
    std::uint32_t magic,
    std::int64_t dispatch_us,
    std::int64_t inject_call_us,
    std::int64_t inject_return_us,
    std::uint8_t coalesced_count
  ) {
    if (!probe.valid || !enabled()) {
      return;
    }

    {
      std::lock_guard lg {recorder.mtx};
      if (!recorder.ring.empty()) {
        auto &slot = recorder.ring[recorder.next];
        slot.sequence_number = probe.sequence_number;
        slot.magic = magic;
        slot.client_event_us = probe.client_event_us;
        slot.client_send_us = probe.client_send_us;
        slot.host_rx_us = probe.host_rx_us;
        slot.host_dispatch_us = dispatch_us;
        slot.host_inject_call_us = inject_call_us;
        slot.host_inject_return_us = inject_return_us;
        slot.coalesced_count = coalesced_count;

        recorder.next = (recorder.next + 1) % recorder.ring.size();
        ++recorder.written;
      }
    }

    input_probe_echo_t echo {};
    echo.version = INPUT_PROBE_VERSION;
    echo.flags = 0;
    echo.coalesced_count = coalesced_count;
    echo.reserved = 0;
    echo.sequence_number = probe.sequence_number;
    echo.host_rx_us = static_cast<std::uint64_t>(probe.host_rx_us);
    echo.host_inject_us = static_cast<std::uint64_t>(inject_call_us);
    emit_echo(echo);
  }

  void end_session() {
    // Flip the flag first so the injection thread stops recording while the file is written.
    if (!detail::active.exchange(false, std::memory_order_relaxed)) {
      return;
    }

    pending_t leftover;
    {
      std::lock_guard lg {pending_mtx};
      leftover = pending;
      pending = {};
    }
    if (leftover.valid) {
      emit_orphan_echo(leftover.sequence_number, leftover.host_rx_us);
    }

    {
      std::lock_guard lg {sink_mtx};
      sink = nullptr;
    }

    std::lock_guard lg {recorder.mtx};

    auto path = build_output_path(recorder.epoch_wall);
    std::ofstream out {path, std::ios::binary | std::ios::trunc};
    if (!out) {
      BOOST_LOG(error) << "Input trace: couldn't open "sv << path.string();
      recorder.ring.clear();
      return;
    }

    auto total = recorder.written;
    auto kept = std::min(total, recorder.ring.size());

    out << "# apollo_input_trace v1\n";
    out << "# apollo_version=" << PROJECT_VERSION << "\n";
    out << "# session_start_wall=" << wall_clock_string(recorder.epoch_wall) << "\n";
    out << "# session_start_us=" << recorder.epoch_us << "\n";
    out << "# clock=steady_clock_us\n";
    out << "# client=" << recorder.meta.client_name << "\n";
    out << "# gamepad_type=" << recorder.meta.gamepad_type << "\n";
    out << "# events_submitted=" << total << "\n";
    out << "# events_kept=" << kept << "\n";
    out << "# capacity=" << recorder.meta.capacity << "\n";
    // Stated rather than implied. A reader who only sees the columns would otherwise assume
    // the round trip ends where the game observes the input, and it does not.
    out << "# UNMEASURED: the stage after host_inject_call. On Windows the gamepad path calls\n";
    out << "#   vigem_target_x360_update()/vigem_target_ds4_update_ex(), and after that returns the\n";
    out << "#   ViGEm bus driver still has to deliver the report through HIDClass and the game still\n";
    out << "#   has to poll XInput. That is real latency, it is NOT in any column here, and it is not\n";
    out << "#   folded into host_inject_return either -- that is only the API call cost.\n";
    // A record here means the event reached the injection dispatch, not that the OS accepted it.
    out << "# CAVEAT: a record means the event reached type dispatch. Per-device toggles (mouse,\n";
    out << "#   keyboard, controller) are checked further in, so an event dropped by one of those\n";
    out << "#   still produces a row with injection timestamps. Those rows time the dispatch, not a\n";
    out << "#   real injection. Only relevant if you disabled a device and then measured it.\n";
    out << "# host_inject_call is taken immediately before the host converts the packet into OS\n";
    out << "#   calls. It therefore includes per-packet decode and coordinate/keycode mapping, and\n";
    out << "#   excludes all queueing. host_inject_return - host_inject_call bounds that whole cost.\n";
    out << "# coalesced_count: input packets merged into the injection that carried this probe.\n";
    out << "#   1 = clean per-event sample. >1 = the host batched, and the merged packets' queueing\n";
    out << "#   time is not visible in host_inject_call. 0 = not determined.\n";
    out << "# client_* columns are in the CLIENT clock and are NOT converted here; join them with\n";
    out << "#   the client CSV using the clock offset from the control channel sync.\n";
    out << "sequence_number,magic,coalesced_count,client_event_us,client_send_us,"
           "host_rx_us,host_dispatch_us,host_inject_call_us,host_inject_return_us\n";

    auto emit = [&out](const record_t &r) {
      out << r.sequence_number << ','
          << "0x" << std::hex << r.magic << std::dec << ','
          << static_cast<int>(r.coalesced_count) << ','
          << r.client_event_us << ','
          << r.client_send_us << ','
          << r.host_rx_us << ','
          << r.host_dispatch_us << ','
          << r.host_inject_call_us << ','
          << r.host_inject_return_us << '\n';
    };

    if (total <= recorder.ring.size()) {
      for (std::size_t i = 0; i < total; ++i) {
        emit(recorder.ring[i]);
      }
    } else {
      // Wrapped: the oldest surviving record sits at `next`.
      for (std::size_t i = 0; i < recorder.ring.size(); ++i) {
        emit(recorder.ring[(recorder.next + i) % recorder.ring.size()]);
      }
    }

    out.flush();
    BOOST_LOG(info) << "Input trace: wrote "sv << kept << " events to "sv << path.string()
                    << (total > kept ? " (ring wrapped, oldest dropped)"sv : ""sv);

    recorder.ring.clear();
    recorder.ring.shrink_to_fit();
  }

}  // namespace input_trace
