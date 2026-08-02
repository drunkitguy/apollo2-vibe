/**
 * @file src/focus_hints.h
 * @brief Declarations for host text-input focus hints.
 */
#pragma once

// standard includes
#include <cstdint>
#include <functional>

// lib includes
#include <boost/endian/arithmetic.hpp>

/**
 * @brief Tells the client when the host has focused a text field, so it can raise a
 *        keyboard without the user asking for one.
 *
 * The client cannot infer this from the video stream, so the host classifies the focused
 * element and sends the result on the existing control channel. It is a state, not a
 * command: every classification change is sent, including back to `none`.
 *
 * Off unless `config::video.focus_hints` is set. When it is off nothing is hooked, no
 * thread is started, UI Automation is never instantiated and the streaming threads pay
 * nothing at all — the feature has no per-frame cost in either state, because it does no
 * per-frame work.
 *
 * ### Thread ownership
 *
 * - `start()` and `stop()` are called from the session start/teardown thread.
 * - The detector owns a dedicated thread with its own message loop. Every Win32 hook
 *   callback and every UI Automation call happens on that thread and nowhere else. This
 *   matters: `IUIAutomation::GetFocusedElement()` was measured blocking for over eight
 *   seconds against an application whose UI thread was busy, so it must never be called
 *   from the capture, encode, control or broadcast threads.
 * - The callback passed to `start()` is invoked on that detector thread. It must not
 *   block; the implementation only enqueues onto the control thread's outbound path.
 */
namespace focus_hints {

  /**
   * @brief What kind of input the focused element expects.
   * @details Wire values, agreed with the client. See
   *          artifacts/wire-contract-frame-trace.md §7.
   */
  enum class kind_t : std::uint8_t {
    none = 0,  ///< Nothing focused that takes text
    text = 1,  ///< Ordinary text entry
    numeric = 2,  ///< Digits are the expected input. Only ever reported when certain.
    password = 3,  ///< Text entry that must not be echoed or autocorrected
  };

  // Protocol constants. These mirror the client and must not be changed on one side alone.

  /// `x-ml-general.featureFlags` bit: the client understands focus hints.
  constexpr std::uint32_t CLIENT_FF_TEXT_FOCUS = 0x08;

  /// `x-ss-general.featureFlags` bit: the host emits focus hints.
  constexpr std::uint32_t HOST_FF_TEXT_FOCUS = 0x08;

  /// Control stream message type, host to client.
  constexpr std::uint16_t FOCUS_HINT_PTYPE = 0x3020;

  /// Value of `focus_hint_t::version`.
  constexpr std::uint8_t FOCUS_HINT_VERSION = 1;

#pragma pack(push, 1)

  /**
   * @brief The message body. Little-endian, 8 bytes.
   * @details `sequence_number` is monotonic within a session, starting at 1, so the client
   *          can drop a hint that arrives out of order rather than acting on a stale one.
   */
  struct focus_hint_t {
    std::uint8_t version;
    std::uint8_t focus_type;
    boost::endian::little_uint16_t reserved;
    boost::endian::little_uint32_t sequence_number;
  };

  static_assert(
    sizeof(focus_hint_t) == 8,
    "Focus hint message must be 8 bytes"
  );

#pragma pack(pop)

  /// Invoked on the detector thread when the classification changes.
  using callback_t = std::function<void(kind_t)>;

  /**
   * @brief Start watching focus changes.
   * @details Does nothing when `config::video.focus_hints` is disabled, or when a monitor
   *          is already running — there is one global detector, and a second session
   *          shares it rather than starting a second UI Automation client.
   * @param cb Invoked on the detector thread on each change. Must not block.
   * @return `true` if this caller started the monitor and must call `stop()`.
   */
  bool start(callback_t cb);

  /**
   * @brief Stop watching and join the detector thread.
   * @details Safe to call when nothing was started. After it returns the callback will not
   *          be invoked again.
   */
  void stop();

  /**
   * @brief Human-readable name, for logs and the CSV.
   */
  const char *to_string(kind_t kind);

  namespace platf {
    /**
     * @brief Platform detector, implemented per platform.
     * @details Runs until `stop_monitor()` is called. Invokes `on_change` only when the
     *          classification actually differs from the last one reported.
     * @return `false` when the platform has no implementation.
     */
    bool start_monitor(const std::function<void(kind_t)> &on_change);
    void stop_monitor();
  }  // namespace platf

}  // namespace focus_hints
