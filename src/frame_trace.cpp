/**
 * @file src/frame_trace.cpp
 * @brief Definitions for the per-frame latency trace recorder.
 */
// this include
#include "frame_trace.h"

// standard includes
#include <algorithm>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <limits>
#include <mutex>
#include <vector>

// local includes
#include "config.h"
#include "logging.h"

namespace fs = std::filesystem;
using namespace std::literals;

namespace frame_trace {

  namespace detail {
    std::atomic<bool> active {false};
  }  // namespace detail

  namespace {

    /**
     * @brief One ring buffer slot. Trivially copyable and free of indirection so the
     *        buffer is a single allocation and `submit()` never allocates.
     */
    struct record_t {
      std::int64_t frame_index;
      std::int64_t capture_requested;
      std::int64_t capture_complete;
      std::int64_t encode_submit;
      std::int64_t encode_complete;
      std::int64_t tx_pipeline_entry;
      std::int64_t first_packet_tx;
      std::uint32_t frame_bytes;
      std::uint8_t idr;
    };

    /**
     * @brief The single global recorder.
     * @details `mtx` guards every member. The video broadcast thread holds it for the
     *          duration of one `submit()`; the session start/teardown thread holds it for
     *          the duration of `begin_session()` and `end_session()`. It is never held
     *          across a blocking call other than the one CSV write at session end.
     */
    struct recorder_t {
      std::mutex mtx;
      std::vector<record_t> ring;
      std::size_t next = 0;  ///< Index of the next slot to write
      std::size_t written = 0;  ///< Total records submitted, may exceed ring.size()
      metadata_t meta;
      std::int64_t epoch_us = 0;  ///< Value of the reference clock when the session began
      std::time_t epoch_wall = 0;  ///< Wall clock at the same instant, for correlating runs
    } recorder;

    /**
     * @brief Build the output path for this run.
     * @details `config::video.frame_trace_path` names a file; the wall clock time the
     *          session started is inserted before the extension so consecutive runs don't
     *          overwrite each other.
     */
    fs::path build_output_path(std::time_t when) {
      fs::path configured {config::video.frame_trace_path};
      if (configured.empty()) {
        configured = "frame_trace.csv";
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

    const char *video_format_name(int video_format) {
      switch (video_format) {
        case 0:
          return "H.264";
        case 1:
          return "HEVC";
        case 2:
          return "AV1";
        default:
          return "unknown";
      }
    }

  }  // namespace

  std::int64_t now_us() {
    return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
  }

  std::uint8_t negotiate_ext_version(int peer_max_version) {
    auto version = std::min<int>(peer_max_version, FRAME_TIMESTAMP_EXT_VERSION_MAX);
    return (std::uint8_t) std::max<int>(version, FRAME_TIMESTAMP_EXT_VERSION_MIN);
  }

  std::uint8_t validity_mask_for(const host_stamps_t &stamps) {
    std::uint8_t mask = 0;
    if (stamps.capture_requested) {
      mask |= STAMP_VALID_CAPTURE_REQUESTED;
    }
    if (stamps.capture_complete) {
      mask |= STAMP_VALID_CAPTURE_COMPLETE;
    }
    if (stamps.encode_submit) {
      mask |= STAMP_VALID_ENCODE_SUBMIT;
    }
    if (stamps.encode_complete) {
      mask |= STAMP_VALID_ENCODE_COMPLETE;
    }
    if (stamps.tx_pipeline_entry) {
      mask |= STAMP_VALID_TX_PIPELINE_ENTRY;
    }
    return mask;
  }

  bool begin_session(const metadata_t &meta) {
    if (!config::video.frame_trace) {
      return false;
    }

    std::lock_guard lg {recorder.mtx};

    if (detail::active.load(std::memory_order_relaxed)) {
      // One global ring buffer can't attribute records to a session, so the second
      // concurrent stream is left untraced rather than corrupting the first one's data.
      BOOST_LOG(warning) << "Frame trace: a trace is already running, not tracing this session"sv;
      return false;
    }

    auto capacity = std::max(config::video.frame_trace_capacity, 1);
    recorder.ring.assign(capacity, record_t {});
    recorder.next = 0;
    recorder.written = 0;
    recorder.meta = meta;
    recorder.epoch_us = now_us();
    recorder.epoch_wall = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());

    detail::active.store(true, std::memory_order_release);

    if (meta.trace_ext_negotiated) {
      BOOST_LOG(info) << "Frame trace: recording up to "sv << capacity
                      << " frames in memory, emitting frame timestamp extension v"sv << meta.trace_ext_version
                      << " (host supports up to v"sv << (int) FRAME_TIMESTAMP_EXT_VERSION_MAX << ')';
    } else {
      // The commonest way for a trace to look broken: the host CSV fills up, the client's
      // host columns stay empty, and nothing says why. Say why.
      BOOST_LOG(info) << "Frame trace: recording up to "sv << capacity
                      << " frames in memory, but the client did not negotiate the in-band extension "
                         "— it will have no host timestamps to join against"sv;
    }
    return true;
  }

  void submit(std::int64_t frame_index, const host_stamps_t &stamps, std::int64_t t_first_packet_tx, std::size_t frame_bytes, bool idr) {
    if (!enabled()) {
      return;
    }

    std::lock_guard lg {recorder.mtx};

    // end_session() may have won the race for the lock
    if (recorder.ring.empty()) {
      return;
    }

    recorder.ring[recorder.next] = record_t {
      frame_index,
      stamps.capture_requested,
      stamps.capture_complete,
      stamps.encode_submit,
      stamps.encode_complete,
      stamps.tx_pipeline_entry,
      t_first_packet_tx,
      (std::uint32_t) std::min<std::size_t>(frame_bytes, std::numeric_limits<std::uint32_t>::max()),
      (std::uint8_t) idr,
    };

    recorder.next = (recorder.next + 1) % recorder.ring.size();
    recorder.written++;
  }

  void end_session() {
    // Stop the producer before taking the lock so it drops out on the fast path
    if (!detail::active.exchange(false, std::memory_order_acq_rel)) {
      return;
    }

    std::lock_guard lg {recorder.mtx};

    if (recorder.ring.empty()) {
      return;
    }

    const auto total = recorder.written;
    const auto kept = std::min(total, recorder.ring.size());
    // Oldest kept record: the ring has wrapped iff more were written than it holds
    const auto first = (total > recorder.ring.size()) ? recorder.next : 0;

    auto path = build_output_path(recorder.epoch_wall);

    std::ofstream out {path.string(), std::ios::out | std::ios::trunc};
    if (!out) {
      BOOST_LOG(error) << "Frame trace: couldn't open ["sv << path.string() << "] for writing"sv;
      recorder.ring.clear();
      recorder.ring.shrink_to_fit();
      return;
    }

    char wall[32] {};
    std::tm tm {};
#ifdef _WIN32
    localtime_s(&tm, &recorder.epoch_wall);
#else
    localtime_r(&recorder.epoch_wall, &tm);
#endif
    std::strftime(wall, sizeof(wall), "%Y-%m-%dT%H:%M:%S", &tm);

    const auto &meta = recorder.meta;

    // The client only ever receives `t_tx_pipeline_entry`, because the frame header is
    // covered by FEC parity computed before the first packet leaves. Measure how far that
    // sits from the real transmit timestamp so a consumer joining the two CSVs knows the
    // size of the bias instead of having to assume one. Computed from this run's own data.
    std::vector<std::int64_t> tx_bias;
    tx_bias.reserve(kept);
    for (std::size_t i = 0; i < kept; i++) {
      const auto &r = recorder.ring[(first + i) % recorder.ring.size()];
      if (r.tx_pipeline_entry != 0 && r.first_packet_tx != 0) {
        tx_bias.push_back(r.first_packet_tx - r.tx_pipeline_entry);
      }
    }
    std::sort(tx_bias.begin(), tx_bias.end());
    const auto bias_at = [&tx_bias](double q) -> std::int64_t {
      if (tx_bias.empty()) {
        return -1;
      }
      auto idx = (std::size_t) (q * (double) (tx_bias.size() - 1));
      return tx_bias[idx];
    };

    // Metadata block. A run without it is not comparable against another run, so it is
    // written unconditionally and the whole file is discarded if it is missing.
    out << "# apollo_version=" << PROJECT_VERSION << '\n'
        << "# session_start_wall=" << wall << '\n'
        << "# session_start_us=" << recorder.epoch_us << '\n'
        << "# clock=steady_clock_us\n"
        << "# resolution=" << meta.width << 'x' << meta.height << '\n'
        << "# framerate=" << meta.framerate << '\n'
        << "# bitrate_kbps=" << meta.bitrate_kbps << '\n'
        << "# codec=" << video_format_name(meta.video_format) << '\n'
        << "# chroma=" << (meta.chroma_sampling_type == 1 ? "4:4:4" : "4:2:0") << '\n'
        << "# bit_depth=" << (meta.dynamic_range ? 10 : 8) << '\n'
        << "# slices_per_frame=" << meta.slices_per_frame << '\n'
        << "# num_ref_frames=" << meta.num_ref_frames << '\n'
        << "# encoder_csc_mode=" << meta.encoder_csc_mode << '\n'
        << "# intra_refresh=" << meta.intra_refresh << '\n'
        << "# encoder=" << (meta.encoder.empty() ? "unknown" : meta.encoder) << '\n'
        << "# capture=" << (meta.capture.empty() ? "auto" : meta.capture) << '\n'
        << "# client=" << meta.client << '\n'
        // Without these a joined host/client dataset can't be interpreted: version 1 has no
        // validity mask, and "not negotiated" means the client has no host columns at all.
        << "# trace_ext_negotiated=" << (meta.trace_ext_negotiated ? 1 : 0) << '\n'
        << "# trace_ext_version=" << meta.trace_ext_version << '\n'
        << "# nvenc_preset=" << config::video.nv.quality_preset << '\n'
        << "# nvenc_vbv_increase=" << config::video.nv.vbv_percentage_increase << '\n'
        << "# fec_percentage=" << config::stream.fec_percentage << '\n'
        << "# frames_submitted=" << total << '\n'
        << "# frames_kept=" << kept << '\n'
        // How much later the first packet actually left than the in-band
        // tx_pipeline_entry_us the client was given. -1 means no frame had both stamps.
        << "# tx_pipeline_entry_bias_us_p50=" << bias_at(0.50) << '\n'
        << "# tx_pipeline_entry_bias_us_p99=" << bias_at(0.99) << '\n'
        << "# tx_pipeline_entry_bias_us_max=" << (tx_bias.empty() ? -1 : tx_bias.back()) << '\n';

    // t_tx_pipeline_entry is the value the client was given in band; t_first_packet_tx is
    // the real one. Both are emitted so the bias is measurable per frame, not just in
    // aggregate. A zero in any timestamp column means "not measured", never "time zero".
    out << "frame_index,frame_id,idr,frame_bytes,t_capture_requested,t_capture_complete,"
           "t_encode_submit,t_encode_complete,t_tx_pipeline_entry,t_first_packet_tx\n";

    for (std::size_t i = 0; i < kept; i++) {
      const auto &r = recorder.ring[(first + i) % recorder.ring.size()];
      out << r.frame_index << ','
          // frame_id as the client sees it, so the join survives the 32-bit wire wraparound
          << (std::uint32_t) r.frame_index << ','
          << (int) r.idr << ','
          << r.frame_bytes << ','
          << r.capture_requested << ','
          << r.capture_complete << ','
          << r.encode_submit << ','
          << r.encode_complete << ','
          << r.tx_pipeline_entry << ','
          << r.first_packet_tx << '\n';
    }

    out.close();

    if (total > kept) {
      BOOST_LOG(warning) << "Frame trace: ring buffer wrapped, dropped the oldest "sv << (total - kept) << " frames"sv;
    }
    BOOST_LOG(info) << "Frame trace: wrote "sv << kept << " records to ["sv << path.string() << ']';

    recorder.ring.clear();
    recorder.ring.shrink_to_fit();
  }

}  // namespace frame_trace
