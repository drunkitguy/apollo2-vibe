/**
 * @file src/input_batching_policy.cpp
 * @brief Definitions for the input message batching rules.
 */
// this include
#include "input_batching_policy.h"

// standard includes
#include <cstring>

// local includes
#include "utility.h"

namespace input {


  /**
   * @brief Batch two relative mouse messages.
   * @param dest The original packet to batch into.
   * @param src A later packet to attempt to batch.
   * @return The status of the batching operation.
   */
  batch_result_e batch(PNV_REL_MOUSE_MOVE_PACKET dest, PNV_REL_MOUSE_MOVE_PACKET src) {
    short deltaX;
    short deltaY;

    // Batching is safe as long as the result doesn't overflow a 16-bit integer.
    // __builtin_add_overflow() returns true when the addition DID overflow, and it still
    // writes the wrapped result to the output, so the sum may only be used when it returns
    // false.
    if (__builtin_add_overflow(util::endian::big(dest->deltaX), util::endian::big(src->deltaX), &deltaX)) {
      return batch_result_e::terminate_batch;
    }
    if (__builtin_add_overflow(util::endian::big(dest->deltaY), util::endian::big(src->deltaY), &deltaY)) {
      return batch_result_e::terminate_batch;
    }

    // Take the sum of deltas
    dest->deltaX = util::endian::big(deltaX);
    dest->deltaY = util::endian::big(deltaY);
    return batch_result_e::batched;
  }

  /**
   * @brief Batch two absolute mouse messages.
   * @param dest The original packet to batch into.
   * @param src A later packet to attempt to batch.
   * @return The status of the batching operation.
   */
  batch_result_e batch(PNV_ABS_MOUSE_MOVE_PACKET dest, PNV_ABS_MOUSE_MOVE_PACKET src) {
    // Batching must only happen if the reference width and height don't change
    if (dest->width != src->width || dest->height != src->height) {
      return batch_result_e::terminate_batch;
    }

    // Take the latest absolute position
    *dest = *src;
    return batch_result_e::batched;
  }

  /**
   * @brief Batch two vertical scroll messages.
   * @param dest The original packet to batch into.
   * @param src A later packet to attempt to batch.
   * @return The status of the batching operation.
   */
  batch_result_e batch(PNV_SCROLL_PACKET dest, PNV_SCROLL_PACKET src) {
    short scrollAmt;

    // Batching is safe as long as the result doesn't overflow a 16-bit integer.
    // See the note in the relative mouse overload: true means it DID overflow.
    if (__builtin_add_overflow(util::endian::big(dest->scrollAmt1), util::endian::big(src->scrollAmt1), &scrollAmt)) {
      return batch_result_e::terminate_batch;
    }

    // Take the sum of delta
    dest->scrollAmt1 = util::endian::big(scrollAmt);
    dest->scrollAmt2 = util::endian::big(scrollAmt);
    return batch_result_e::batched;
  }

  /**
   * @brief Batch two horizontal scroll messages.
   * @param dest The original packet to batch into.
   * @param src A later packet to attempt to batch.
   * @return The status of the batching operation.
   */
  batch_result_e batch(PSS_HSCROLL_PACKET dest, PSS_HSCROLL_PACKET src) {
    short scrollAmt;

    // Batching is safe as long as the result doesn't overflow a 16-bit integer.
    // See the note in the relative mouse overload: true means it DID overflow.
    if (__builtin_add_overflow(util::endian::big(dest->scrollAmount), util::endian::big(src->scrollAmount), &scrollAmt)) {
      return batch_result_e::terminate_batch;
    }

    // Take the sum of delta
    dest->scrollAmount = util::endian::big(scrollAmt);
    return batch_result_e::batched;
  }

  /**
   * @brief Batch two controller state messages.
   * @param dest The original packet to batch into.
   * @param src A later packet to attempt to batch.
   * @return The status of the batching operation.
   */
  batch_result_e batch(PNV_MULTI_CONTROLLER_PACKET dest, PNV_MULTI_CONTROLLER_PACKET src) {
    // Do not allow batching if the active controllers change
    if (dest->activeGamepadMask != src->activeGamepadMask) {
      return batch_result_e::terminate_batch;
    }

    // We can only batch entries for the same controller, but allow batching attempts to continue
    // in case we have more packets for this controller later in the queue.
    if (dest->controllerNumber != src->controllerNumber) {
      return batch_result_e::not_batchable;
    }

    // Do not allow batching if the button state changes on this controller
    if (dest->buttonFlags != src->buttonFlags || dest->buttonFlags2 != src->buttonFlags2) {
      return batch_result_e::terminate_batch;
    }

    // Take the latest state
    *dest = *src;
    return batch_result_e::batched;
  }

  /**
   * @brief Batch two touch messages.
   * @param dest The original packet to batch into.
   * @param src A later packet to attempt to batch.
   * @return The status of the batching operation.
   */
  batch_result_e batch(PSS_TOUCH_PACKET dest, PSS_TOUCH_PACKET src) {
    // Only batch hover or move events
    if (dest->eventType != LI_TOUCH_EVENT_MOVE &&
        dest->eventType != LI_TOUCH_EVENT_HOVER) {
      return batch_result_e::terminate_batch;
    }

    // Don't batch beyond state changing events
    if (src->eventType != LI_TOUCH_EVENT_MOVE &&
        src->eventType != LI_TOUCH_EVENT_HOVER) {
      return batch_result_e::terminate_batch;
    }

    // Batched events must be the same pointer ID
    if (dest->pointerId != src->pointerId) {
      return batch_result_e::not_batchable;
    }

    // The pointer must be in the same state
    if (dest->eventType != src->eventType) {
      return batch_result_e::terminate_batch;
    }

    // Take the latest state
    *dest = *src;
    return batch_result_e::batched;
  }

  /**
   * @brief Batch two pen messages.
   * @param dest The original packet to batch into.
   * @param src A later packet to attempt to batch.
   * @return The status of the batching operation.
   */
  batch_result_e batch(PSS_PEN_PACKET dest, PSS_PEN_PACKET src) {
    // Only batch hover or move events
    if (dest->eventType != LI_TOUCH_EVENT_MOVE &&
        dest->eventType != LI_TOUCH_EVENT_HOVER) {
      return batch_result_e::terminate_batch;
    }

    // Batched events must be the same type
    if (dest->eventType != src->eventType) {
      return batch_result_e::terminate_batch;
    }

    // Do not allow batching if the button state changes
    if (dest->penButtons != src->penButtons) {
      return batch_result_e::terminate_batch;
    }

    // Do not batch beyond tool changes
    if (dest->toolType != src->toolType) {
      return batch_result_e::terminate_batch;
    }

    // Take the latest state
    *dest = *src;
    return batch_result_e::batched;
  }

  /**
   * @brief Batch two controller touch messages.
   * @param dest The original packet to batch into.
   * @param src A later packet to attempt to batch.
   * @return The status of the batching operation.
   */
  batch_result_e batch(PSS_CONTROLLER_TOUCH_PACKET dest, PSS_CONTROLLER_TOUCH_PACKET src) {
    // Only batch hover or move events
    if (dest->eventType != LI_TOUCH_EVENT_MOVE &&
        dest->eventType != LI_TOUCH_EVENT_HOVER) {
      return batch_result_e::terminate_batch;
    }

    // We can only batch entries for the same controller, but allow batching attempts to continue
    // in case we have more packets for this controller later in the queue.
    if (dest->controllerNumber != src->controllerNumber) {
      return batch_result_e::not_batchable;
    }

    // Don't batch beyond state changing events
    if (src->eventType != LI_TOUCH_EVENT_MOVE &&
        src->eventType != LI_TOUCH_EVENT_HOVER) {
      return batch_result_e::terminate_batch;
    }

    // Batched events must be the same pointer ID
    if (dest->pointerId != src->pointerId) {
      return batch_result_e::not_batchable;
    }

    // The pointer must be in the same state
    if (dest->eventType != src->eventType) {
      return batch_result_e::terminate_batch;
    }

    // Take the latest state
    *dest = *src;
    return batch_result_e::batched;
  }

  /**
   * @brief Batch two controller motion messages.
   * @param dest The original packet to batch into.
   * @param src A later packet to attempt to batch.
   * @return The status of the batching operation.
   */
  batch_result_e batch(PSS_CONTROLLER_MOTION_PACKET dest, PSS_CONTROLLER_MOTION_PACKET src) {
    // We can only batch entries for the same controller, but allow batching attempts to continue
    // in case we have more packets for this controller later in the queue.
    if (dest->controllerNumber != src->controllerNumber) {
      return batch_result_e::not_batchable;
    }

    // Batched events must be the same sensor
    if (dest->motionType != src->motionType) {
      return batch_result_e::not_batchable;
    }

    // Take the latest state
    *dest = *src;
    return batch_result_e::batched;
  }

  /**
   * @brief Batch two input messages.
   * @param dest The original packet to batch into.
   * @param src A later packet to attempt to batch.
   * @return The status of the batching operation.
   */
  batch_result_e batch(PNV_INPUT_HEADER dest, PNV_INPUT_HEADER src) {
    // We can only batch if the packet types are the same
    if (dest->magic != src->magic) {
      return batch_result_e::terminate_batch;
    }

    // We can only batch certain message types
    switch (util::endian::little(dest->magic)) {
      case MOUSE_MOVE_REL_MAGIC_GEN5:
        return batch((PNV_REL_MOUSE_MOVE_PACKET) dest, (PNV_REL_MOUSE_MOVE_PACKET) src);
      case MOUSE_MOVE_ABS_MAGIC:
        return batch((PNV_ABS_MOUSE_MOVE_PACKET) dest, (PNV_ABS_MOUSE_MOVE_PACKET) src);
      case SCROLL_MAGIC_GEN5:
        return batch((PNV_SCROLL_PACKET) dest, (PNV_SCROLL_PACKET) src);
      case SS_HSCROLL_MAGIC:
        return batch((PSS_HSCROLL_PACKET) dest, (PSS_HSCROLL_PACKET) src);
      case MULTI_CONTROLLER_MAGIC_GEN5:
        return batch((PNV_MULTI_CONTROLLER_PACKET) dest, (PNV_MULTI_CONTROLLER_PACKET) src);
      case SS_TOUCH_MAGIC:
        return batch((PSS_TOUCH_PACKET) dest, (PSS_TOUCH_PACKET) src);
      case SS_PEN_MAGIC:
        return batch((PSS_PEN_PACKET) dest, (PSS_PEN_PACKET) src);
      case SS_CONTROLLER_TOUCH_MAGIC:
        return batch((PSS_CONTROLLER_TOUCH_PACKET) dest, (PSS_CONTROLLER_TOUCH_PACKET) src);
      case SS_CONTROLLER_MOTION_MAGIC:
        return batch((PSS_CONTROLLER_MOTION_PACKET) dest, (PSS_CONTROLLER_MOTION_PACKET) src);
      default:
        // Not a batchable message type
        return batch_result_e::terminate_batch;
    }
  }

}  // namespace input
