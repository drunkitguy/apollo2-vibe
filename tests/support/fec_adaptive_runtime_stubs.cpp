/**
 * @file tests/support/fec_adaptive_runtime_stubs.cpp
 * @brief Deterministic seam for the adaptive FEC component target.
 *
 * `src/fec_adaptive.cpp` reads `config::stream` and logs through the
 * process-wide severity loggers. Linking `src/config.cpp` and `src/logging.cpp`
 * to reach them would drag the whole application inventory into a component
 * target, which this test layout deliberately forbids. Both are plain globals
 * with no initialization order requirements, so the target supplies its own
 * value-initialized definitions instead: the tests set the fields the control
 * law reads, and the loggers have no sinks attached, so log records are
 * formatted and discarded exactly as they are in an unconfigured process.
 */

// standard includes
#include <boost/log/sources/severity_logger.hpp>

// local includes
#include <src/config.h>

namespace config {
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
