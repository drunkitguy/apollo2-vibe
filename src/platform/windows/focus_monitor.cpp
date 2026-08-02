/**
 * @file src/platform/windows/focus_monitor.cpp
 * @brief Definitions for detecting which kind of text field has focus on Windows.
 */
// standard includes
#include <atomic>
#include <chrono>
#include <thread>

// platform includes
#include <windows.h>
#include <initguid.h>
#include <uiautomation.h>

// local includes
#include "src/focus_hints.h"
#include "src/logging.h"

using namespace std::literals;

namespace focus_hints::platf {

  namespace {

    /**
     * @brief How long to wait after a focus event before classifying.
     * @details Focus storms are normal — tabbing through a dialog, or a window activating
     *          and then handing focus to its default control, produce several events in a
     *          few milliseconds. Classifying only the settled state keeps the expensive
     *          UI Automation call off the common path and keeps the control channel, which
     *          is shared with input, quiet.
     */
    constexpr auto DEBOUNCE = 120ms;

    /// Floor on the interval between two hints, whatever the focus does.
    constexpr auto MIN_SEND_INTERVAL = 100ms;

    /**
     * @brief Bound on a single UI Automation call.
     * @details `GetFocusedElement()` was measured taking 1.4-5.6 ms normally but blocking
     *          for 1130 ms and 8528 ms against applications whose UI thread was busy. The
     *          detector thread can afford to block; nothing else can, which is why this
     *          code never runs anywhere else. These timeouts are defence in depth.
     */
    constexpr DWORD UIA_CONNECTION_TIMEOUT_MS = 500;
    constexpr DWORD UIA_TRANSACTION_TIMEOUT_MS = 500;

    std::thread monitor_thread;
    std::atomic<bool> shutdown_requested {false};
    std::atomic<DWORD> monitor_thread_id {0};

    /// Set by the hook callback, consumed by the loop. Both on the detector thread.
    bool focus_event_pending = false;

    /**
     * @brief Classify a focused window from its Win32 style bits alone.
     * @details This is the only way to detect a numeric field. UI Automation has no
     *          property for it: a plain edit and an `ES_NUMBER` edit are byte-identical
     *          across every UIA property, verified by experiment against controls whose
     *          nature was known for certain. `InputScope` is a Text Services Framework
     *          concept and is not exposed to an out-of-process consumer.
     * @param hwnd Focused window, may be null.
     * @param[out] kind Set only when this function returns `true`.
     * @return `true` if the window is a classic edit control and was classified here,
     *         which also means UI Automation does not need to be consulted at all.
     */
    bool classify_from_window_style(HWND hwnd, kind_t &kind) {
      if (!hwnd || !IsWindow(hwnd)) {
        return false;
      }

      wchar_t class_name[64] = {};
      if (!GetClassNameW(hwnd, class_name, _countof(class_name))) {
        return false;
      }

      // The style bits below only mean what we want on the stock edit classes
      const bool is_edit = wcscmp(class_name, L"Edit") == 0 ||
                           wcsncmp(class_name, L"RichEdit", 8) == 0;
      if (!is_edit) {
        return false;
      }

      const auto style = GetWindowLongPtrW(hwnd, GWL_STYLE);
      if (style & ES_PASSWORD) {
        kind = kind_t::password;
      } else if (style & ES_NUMBER) {
        kind = kind_t::numeric;
      } else if (style & ES_READONLY) {
        // Focusable but not typeable, so raising a keyboard would be wrong
        kind = kind_t::none;
      } else {
        kind = kind_t::text;
      }
      return true;
    }

    /**
     * @brief Classify the focused element through UI Automation.
     * @details Covers everything without a classic edit HWND, which is most of a modern
     *          desktop. Cannot report `numeric` — see `classify_from_window_style()`.
     */
    kind_t classify_from_uia(IUIAutomation *uia) {
      if (!uia) {
        return kind_t::none;
      }

      IUIAutomationElement *element = nullptr;
      if (FAILED(uia->GetFocusedElement(&element)) || !element) {
        return kind_t::none;
      }

      auto kind = kind_t::none;

      CONTROLTYPEID control_type = 0;
      BOOL is_password = FALSE;
      element->get_CurrentControlType(&control_type);
      element->get_CurrentIsPassword(&is_password);

      if (is_password) {
        // Reliable across frameworks: this is what screen readers use to suppress echo
        kind = kind_t::password;
      } else {
        switch (control_type) {
          case UIA_EditControlTypeId:
          case UIA_DocumentControlTypeId:
            kind = kind_t::text;
            break;
          case UIA_SpinnerControlTypeId:
            // A spinner's value is a number by definition
            kind = kind_t::numeric;
            break;
          case UIA_ComboBoxControlTypeId:
            {
              // Editable combo boxes take text, fixed lists don't
              BOOL editable = FALSE;
              VARIANT value;
              VariantInit(&value);
              if (SUCCEEDED(element->GetCurrentPropertyValue(UIA_IsValuePatternAvailablePropertyId, &value)) &&
                  value.vt == VT_BOOL) {
                editable = (value.boolVal != VARIANT_FALSE);
              }
              VariantClear(&value);
              kind = editable ? kind_t::text : kind_t::none;
              break;
            }
          default:
            kind = kind_t::none;
            break;
        }
      }

      element->Release();
      return kind;
    }

    void CALLBACK focus_event_proc(HWINEVENTHOOK, DWORD event, HWND, LONG object_id, LONG, DWORD, DWORD) {
      if (event == EVENT_OBJECT_FOCUS && object_id == OBJID_CLIENT) {
        // Just record that something happened. The classification is deliberately not done
        // here: this callback runs on the detector thread's message loop and the UI
        // Automation call it would trigger can block for seconds.
        focus_event_pending = true;
      }
    }

    /**
     * @brief The detector thread.
     * @details Owns everything: the COM apartment, the UI Automation client, the WinEvent
     *          hook and the message loop that delivers it. Nothing here is touched by any
     *          other thread except through `shutdown_requested`.
     */
    void monitor_loop(std::function<void(kind_t)> on_change) {
      monitor_thread_id = GetCurrentThreadId();

      if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) {
        BOOST_LOG(error) << "Focus hints: CoInitializeEx() failed"sv;
        return;
      }

      IUIAutomation *uia = nullptr;

      // Prefer the interface that can bound a blocking call. Fall back to the original if
      // it isn't available, since the architecture doesn't depend on the timeouts.
      IUIAutomation2 *uia2 = nullptr;
      if (SUCCEEDED(CoCreateInstance(CLSID_CUIAutomation8, nullptr, CLSCTX_INPROC_SERVER, IID_IUIAutomation2, (void **) &uia2)) && uia2) {
        uia2->put_ConnectionTimeout(UIA_CONNECTION_TIMEOUT_MS);
        uia2->put_TransactionTimeout(UIA_TRANSACTION_TIMEOUT_MS);
        uia = uia2;
      } else if (FAILED(CoCreateInstance(CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER, IID_IUIAutomation, (void **) &uia)) || !uia) {
        BOOST_LOG(error) << "Focus hints: couldn't create a UI Automation client"sv;
        CoUninitialize();
        return;
      }

      // WINEVENT_OUTOFCONTEXT keeps our code out of other processes; SKIPOWNPROCESS stops
      // Apollo's own web UI window from generating hints.
      auto hook = SetWinEventHook(
        EVENT_OBJECT_FOCUS,
        EVENT_OBJECT_FOCUS,
        nullptr,
        focus_event_proc,
        0,
        0,
        WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS
      );

      if (!hook) {
        BOOST_LOG(error) << "Focus hints: SetWinEventHook() failed"sv;
        uia->Release();
        CoUninitialize();
        return;
      }

      auto reported = kind_t::none;
      auto last_sent = std::chrono::steady_clock::now() - MIN_SEND_INTERVAL;
      std::optional<std::chrono::steady_clock::time_point> settle_at;

      while (!shutdown_requested.load(std::memory_order_relaxed)) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
          TranslateMessage(&msg);
          DispatchMessageW(&msg);
        }

        if (focus_event_pending) {
          focus_event_pending = false;
          settle_at = std::chrono::steady_clock::now() + DEBOUNCE;
        }

        auto now = std::chrono::steady_clock::now();
        if (settle_at && now >= *settle_at && now - last_sent >= MIN_SEND_INTERVAL) {
          settle_at.reset();

          // The cheap path first. When it answers, UI Automation is never consulted, which
          // also means the numeric case costs two user32 calls and no COM at all.
          auto kind = kind_t::none;
          GUITHREADINFO gui_info = {sizeof(GUITHREADINFO)};
          if (GetGUIThreadInfo(0, &gui_info) && classify_from_window_style(gui_info.hwndFocus, kind)) {
            // classified from style bits
          } else {
            kind = classify_from_uia(uia);
          }

          if (kind != reported) {
            reported = kind;
            last_sent = std::chrono::steady_clock::now();
            BOOST_LOG(debug) << "Focus hints: "sv << to_string(kind);
            on_change(kind);
          }
        }

        // The hook delivers through the message queue, so wait on it rather than spinning.
        // A timeout keeps the debounce and the shutdown check ticking.
        MsgWaitForMultipleObjectsEx(0, nullptr, 20, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
      }

      UnhookWinEvent(hook);

      // Tell the client the field is gone, so it can drop a keyboard it raised for us
      if (reported != kind_t::none) {
        on_change(kind_t::none);
      }

      uia->Release();
      CoUninitialize();
    }

  }  // namespace

  bool start_monitor(const std::function<void(kind_t)> &on_change) {
    if (monitor_thread.joinable()) {
      return false;
    }

    shutdown_requested = false;
    monitor_thread = std::thread {monitor_loop, on_change};
    return true;
  }

  void stop_monitor() {
    if (!monitor_thread.joinable()) {
      return;
    }

    shutdown_requested = true;

    // The loop may be parked in MsgWaitForMultipleObjectsEx; post a null message so it
    // wakes immediately instead of waiting out its timeout.
    if (auto tid = monitor_thread_id.load()) {
      PostThreadMessageW(tid, WM_NULL, 0, 0);
    }

    monitor_thread.join();
    monitor_thread_id = 0;
  }

}  // namespace focus_hints::platf
