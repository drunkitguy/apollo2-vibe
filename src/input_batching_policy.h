/**
 * @file src/input_batching_policy.h
 * @brief Declarations for the input message batching rules.
 *
 * These decide whether one queued client input message may be merged into an
 * earlier one before it is injected. They are pure functions over the wire
 * packet structs — no session state, no platform calls, no configuration — so
 * they live apart from `input.cpp` and can be exercised on their own.
 */
#pragma once

// lib includes
extern "C" {
#include <moonlight-common-c/src/Input.h>
#include <moonlight-common-c/src/Limelight.h>
}

namespace input {
  /**
   * @brief Outcome of attempting to merge one queued input message into another.
   */
  enum class batch_result_e {
    batched,  ///< This entry was batched with the source entry
    not_batchable,  ///< Not eligible to batch but continue attempts to batch
    terminate_batch,  ///< Stop trying to batch with this entry
  };

  batch_result_e batch(PNV_REL_MOUSE_MOVE_PACKET dest, PNV_REL_MOUSE_MOVE_PACKET src);
  batch_result_e batch(PNV_ABS_MOUSE_MOVE_PACKET dest, PNV_ABS_MOUSE_MOVE_PACKET src);
  batch_result_e batch(PNV_SCROLL_PACKET dest, PNV_SCROLL_PACKET src);
  batch_result_e batch(PSS_HSCROLL_PACKET dest, PSS_HSCROLL_PACKET src);
  batch_result_e batch(PNV_MULTI_CONTROLLER_PACKET dest, PNV_MULTI_CONTROLLER_PACKET src);
  batch_result_e batch(PSS_TOUCH_PACKET dest, PSS_TOUCH_PACKET src);
  batch_result_e batch(PSS_PEN_PACKET dest, PSS_PEN_PACKET src);
  batch_result_e batch(PSS_CONTROLLER_TOUCH_PACKET dest, PSS_CONTROLLER_TOUCH_PACKET src);
  batch_result_e batch(PSS_CONTROLLER_MOTION_PACKET dest, PSS_CONTROLLER_MOTION_PACKET src);

  /**
   * @brief Dispatch to the overload matching the packet type.
   * @param dest The original packet to batch into.
   * @param src A later packet to attempt to batch.
   * @return The status of the batching operation. Unbatchable types terminate the batch.
   */
  batch_result_e batch(PNV_INPUT_HEADER dest, PNV_INPUT_HEADER src);
}  // namespace input
