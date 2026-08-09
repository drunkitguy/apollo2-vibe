/**
 * @file tests/support/input_trace_runtime_stubs.cpp
 * @brief Deterministic seam for the input trace component target.
 *
 * `src/input_trace.cpp` reads `config::input`, and it is tested alongside
 * `src/frame_trace.cpp`, which reads `config::video` and `config::stream`,
 * because the two share a clock and must not claim colliding wire constants.
 * Both log through the process-wide severity loggers. Linking `src/config.cpp`
 * and `src/logging.cpp` to reach any of that would drag the whole application
 * inventory into a component target, which this test layout deliberately
 * forbids. All of them are plain globals with no initialization order
 * requirements, so the target supplies its own value-initialized definitions
 * instead, and the loggers have no sinks attached, so log records are formatted
 * and discarded exactly as they are in an unconfigured process.
 */

// standard includes
#include <boost/log/sources/severity_logger.hpp>

// local includes
#include <src/config.h>

namespace config {
  input_t input {};
  video_t video {};
  stream_t stream {};
}  // namespace config

boost::log::sources::severity_logger<int> verbose;
boost::log::sources::severity_logger<int> debug;
boost::log::sources::severity_logger<int> info;
boost::log::sources::severity_logger<int> warning;
boost::log::sources::severity_logger<int> error;
boost::log::sources::severity_logger<int> fatal;
#ifdef SUNSHINE_TESTS
boost::log::sources::severity_logger<int> tests;
#endif
