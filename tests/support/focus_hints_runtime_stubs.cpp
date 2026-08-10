/**
 * @file tests/support/focus_hints_runtime_stubs.cpp
 * @brief Deterministic seam for the focus hints component target.
 *
 * `src/focus_hints.cpp` reads `config::video` and logs through the process-wide
 * severity loggers. Linking `src/config.cpp` and `src/logging.cpp` to reach them
 * would drag the whole application inventory into a component target, which this
 * test layout deliberately forbids. Both are plain globals with no initialization
 * order requirements, so the target supplies its own value-initialized
 * definitions instead, and the loggers have no sinks attached, so log records are
 * formatted and discarded exactly as they are in an unconfigured process.
 *
 * The detector itself is a platform boundary: on Windows it installs a
 * `SetWinEventHook`, creates a UI Automation client and starts a thread, none of
 * which belongs in a unit test. The two entry points are stubbed to report "no
 * detector on this platform" — the same answer the real non-Windows build gives.
 * The tests only exercise the wire layout, `to_string()` and the disabled path,
 * which returns before either is reached.
 */

// standard includes
#include <boost/log/sources/severity_logger.hpp>

// local includes
#include <src/config.h>
#include <src/focus_hints.h>

namespace config {
  video_t video {};
}  // namespace config

#ifdef _WIN32
// Only Windows has a real detector, so only Windows leaves these undefined here.
// src/focus_hints.cpp already carries the same do-nothing pair for every other platform.
namespace focus_hints::platf {
  bool start_monitor(const std::function<void(kind_t)> &) {
    return false;
  }

  void stop_monitor() {
  }
}  // namespace focus_hints::platf
#endif

boost::log::sources::severity_logger<int> verbose;
boost::log::sources::severity_logger<int> debug;
boost::log::sources::severity_logger<int> info;
boost::log::sources::severity_logger<int> warning;
boost::log::sources::severity_logger<int> error;
boost::log::sources::severity_logger<int> fatal;
#ifdef SUNSHINE_TESTS
boost::log::sources::severity_logger<int> tests;
#endif
