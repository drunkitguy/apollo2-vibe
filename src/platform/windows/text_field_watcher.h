/**
 * @file src/platform/windows/text_field_watcher.h
 * @brief Watches Windows keyboard focus and classifies the focused control as a text field.
 *
 * The watcher runs entirely in-process on a dedicated worker thread. Sunshine.exe is
 * launched into the interactive console session on winsta0\default (see
 * tools/sunshinesvc.cpp), so a UI Automation client created here observes the same
 * desktop the user is looking at and no helper process is required.
 *
 * The published state is consumed by the control stream, which forwards it to connected
 * Moonlight clients as the Apollo "Set Text Field Focus" packet (0x3003).
 */
#pragma once

#include <cstdint>

namespace platf::text_field {

  /**
   * @brief Kind of control that currently holds keyboard focus.
   *
   * These values are part of the 0x3003 wire format and must stay in sync with the
   * ML_TEXT_FIELD_* defines in moonlight-common-c's Limelight.h.
   */
  enum class kind_e : std::uint8_t {
    none = 0,  ///< No text-entry control is focused
    text = 1,  ///< A general text field is focused
    numeric = 2,  ///< A numeric-only field is focused
    password = 3,  ///< A password field is focused
  };

  /**
   * @brief Additional information about the focused control.
   *
   * These values are part of the 0x3003 wire format and must stay in sync with the
   * ML_TEXT_FIELD_FLAG_* defines in moonlight-common-c's Limelight.h.
   */
  inline constexpr std::uint8_t flag_read_only = 0x01;  ///< The field rejects input
  inline constexpr std::uint8_t flag_multiline = 0x02;  ///< The field accepts multiple lines
  /// The FINAL verdict came from a UI Automation rule (R4-R6), not from Win32 window
  /// styles. A classic EDIT window whose style bits were only provisional and which was
  /// then confirmed by a UI Automation rule carries this flag: the Win32 read contributed
  /// the read-only and multiline bits, but it did not decide the kind.
  inline constexpr std::uint8_t flag_source_uia = 0x04;
  inline constexpr std::uint8_t flag_low_confidence = 0x08;  ///< The verdict came from the label keyword tier
  inline constexpr std::uint8_t flag_numeric = 0x10;  ///< Numeric evidence was found; load-bearing when kind is password

  /**
   * @brief Snapshot of the focus state the watcher has settled on.
   *
   * `generation` increments every time the wire-visible fields change. Callers use it to
   * decide whether a peer still needs to be told about the current state.
   */
  struct state_t {
    kind_e kind {kind_e::none};
    std::uint8_t flags {0};
    std::uint32_t input_scope {0};  ///< Reserved for a future TSF-based implementation; always 0 today
    std::uint64_t generation {0};
  };

  /**
   * @brief Start the focus watcher.
   *
   * Idempotent: calling this while the watcher is already running is a no-op that returns
   * true. Returns false when the worker thread could not be started. A worker that starts
   * but cannot reach UI Automation logs a warning once and leaves running() false, so
   * callers stop emitting focus packets rather than reporting a permanent "no field".
   */
  bool start();

  /**
   * @brief Stop the focus watcher and reset the published state to "no field".
   *
   * Safe to call when the watcher was never started.
   */
  void stop();

  /**
   * @brief Whether the watcher is running and successfully subscribed to focus events.
   */
  bool running();

  /**
   * @brief The most recently published focus state.
   */
  state_t current();

#if defined(SUNSHINE_FOCUS_REPORTER)
  /**
   * @brief Set the keyword tier on or off, for builds with no Sunshine configuration.
   *
   * Only compiled into tools/focus_reporter, which builds text_field_watcher.cpp into a
   * standalone executable and therefore has no `config::input` to read. Takes effect on
   * the next start(); the running worker only ever sees the snapshot start() took.
   *
   * The Sunshine build does not have this and does not want it: there the setting is
   * `text_field_numeric_hints`, read from the configuration exactly as every other
   * setting is.
   */
  void set_numeric_hints(bool enabled);
#endif

}  // namespace platf::text_field
