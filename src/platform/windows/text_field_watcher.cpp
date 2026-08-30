/**
 * @file src/platform/windows/text_field_watcher.cpp
 * @brief In-process UI Automation focus watcher used to drive the client-side soft keyboard.
 *
 * Threading model
 * ---------------
 * One worker thread owns everything COM. It initialises COM as MTA, creates the UI
 * Automation client, registers a focus-changed handler and then ticks a small debounce
 * state machine. UI Automation delivers focus events on its own thread pool; those
 * callbacks only classify from the cache the subscription already delivered, record a
 * candidate under the state mutex and return immediately. All publishing, and every call
 * that could possibly reach into the focused application, happens on the worker tick, so
 * a hung application can never stall an event callback into stalling the stream.
 *
 * Every UI Automation property read here is a CACHED read (get_Cached* /
 * GetCachedPropertyValue). A live property accessor issues a synchronous cross-process
 * call, which is the classic way to wedge the UI Automation callback pool against an
 * unresponsive application. Cached reads are served entirely from the snapshot the cache
 * request already populated, so they cannot block on the focused application at all.
 *
 * The one live UI Automation operation in this file is the bounded ancestor walk in
 * refine_candidate(). It exists because UI Automation cannot cache ancestors
 * (TreeScope_Ancestors is not a legal cache scope), and "the focused Edit lives inside a
 * Spinner" is the only signal that identifies a WinForms NumericUpDown, a WinUI NumberBox
 * or a WPF-toolkit up/down out of process. That walk:
 *   - runs on the worker thread ONLY, never in the event callback;
 *   - is capped at three control-view hops;
 *   - is skipped entirely unless IUIAutomation2's connection/transaction timeouts were
 *     applied, because without them a call into a hung provider is unbounded;
 *   - can only ever upgrade a verdict, never downgrade it and never produce "no field".
 * That is the whole reason the focus cache request may use AutomationElementMode_Full: the
 * live reference it implies is held for at most one 50 ms tick, one at a time.
 */

#include "text_field_watcher.h"

#include "src/config.h"
#include "src/logging.h"

#include <atomic>
#include <chrono>
#include <cwchar>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

// clang-format off
#include <winsock2.h>
#include <windows.h>
#include <commctrl.h>
#include <objbase.h>
#include <oleauto.h>
#include <uiautomation.h>
// clang-format on

// mingw-w64 and the Windows SDK both gate the CUIAutomation8 coclass and the
// IUIAutomation2 interface behind the MIDL guards below. CUIAutomation8 is a Windows 8+
// coclass whose only benefit here is IUIAutomation2's connection/transaction timeouts,
// which bound how long a call into a hung provider can block. When the headers predate
// it we fall back to the original CUIAutomation coclass, lose that protection, and with
// it the ancestor walk (see refine_candidate()).
#if defined(__CUIAutomation8_FWD_DEFINED__) && defined(__IUIAutomation2_INTERFACE_DEFINED__)
  #define SUNSHINE_TEXT_FIELD_HAVE_UIA2 1
#else
  #define SUNSHINE_TEXT_FIELD_HAVE_UIA2 0
#endif

using namespace std::literals;

namespace platf::text_field {
  namespace {

    // Transition TO a field. Long enough to swallow the focus storm a dialog produces
    // while it builds its control tree, short enough to feel immediate. It is also what
    // gives the worker's refinement pass (one tick, 50 ms) time to upgrade a provisional
    // verdict before the provisional one is ever published.
    constexpr auto FIELD_DEBOUNCE = 120ms;

    // Transition TO "no field". DELIBERATELY ASYMMETRIC and much longer than
    // FIELD_DEBOUNCE: focus routinely bounces through a non-editable element for tens of
    // milliseconds when a menu opens or a control is recreated, and a short unfocus
    // debounce would make the client IME flicker shut and immediately reopen.
    constexpr auto NONE_DEBOUNCE = 400ms;

    // Hard rate cap on state changes, so pathological focus churn cannot exceed
    // ~5 packets/second/session.
    constexpr auto MIN_PUBLISH_INTERVAL = 200ms;

    constexpr auto TICK_INTERVAL = 50ms;

    // The Win32 safety poll runs every 5th tick.
    constexpr int SAFETY_POLL_TICKS = 5;

    constexpr DWORD UIA_CONNECTION_TIMEOUT_MS = 500;
    constexpr DWORD UIA_TRANSACTION_TIMEOUT_MS = 1000;

    // Budget for one UDM_GETBUDDY probe. A bare SendMessage to another process's hung
    // window blocks forever, so this is always SendMessageTimeoutW.
    constexpr UINT UPDOWN_PROBE_TIMEOUT_MS = 50;

    // ...and a cap on how many up-down siblings one parent may be probed for, so a dialog
    // full of hung spin controls costs at most 8 x 50 ms instead of an unbounded multiple.
    constexpr int MAX_UPDOWN_PROBES = 8;

    // Hard cap on control-view hops in the ancestor probe. Three covers every layout the
    // design targets (WinForms UpDownEdit -> NumericUpDown is 1, WinUI TextBox ->
    // NumberBox is 1-2, Avalonia/WPF-toolkit TextBox -> ButtonSpinner -> NumericUpDown is
    // 2) and stops the walk long before it could reach the desktop root, where a Spinner
    // somewhere in the tree would say nothing about this field.
    constexpr int MAX_ANCESTOR_HOPS = 3;

    /**
     * @brief Where the window handle backing a classification came from.
     *
     * Recorded because the Win32 refinement depends on the cached NativeWindowHandle
     * actually being readable. If it never is, the watcher still fires and still
     * classifies, so nothing looks broken; the only visible symptom would be that Win32
     * numeric fields are never reported. Logging the provenance makes that distinguishable
     * from a debug log without any code change.
     */
    enum class hwnd_source_e : std::uint8_t {
      none = 0,  ///< No window handle was resolved
      uia_cache = 1,  ///< From the element's cached NativeWindowHandle
      gui_thread_info = 2,  ///< From GetGUIThreadInfo(), after the same-process check
    };

    const char *hwnd_source_name(hwnd_source_e source) {
      switch (source) {
        case hwnd_source_e::uia_cache:
          return "uia-cache";
        case hwnd_source_e::gui_thread_info:
          return "gui-thread-info";
        default:
          return "none";
      }
    }

    /**
     * @brief One focus verdict, in flight.
     *
     * operator== compares ONLY the fields the state machine acts on: the wire-visible kind
     * and flags plus the window handle the safety poll tracks. `needs_refine`, `rule` and
     * `diag` are bookkeeping and diagnostics; including them would restart the debounce
     * (and burn a packet) every time an identical field was re-reported through a
     * different rule.
     */
    struct candidate_t {
      kind_e kind {kind_e::none};
      std::uint8_t flags {0};
      HWND hwnd {nullptr};
      hwnd_source_e hwnd_source {hwnd_source_e::none};

      // --- deliberately NOT part of operator== ---
      bool needs_refine {false};  ///< The worker may be able to upgrade this verdict
      bool is_win32_edit {false};  ///< hwnd passed the classic EDIT class gate
      const char *rule {"R0-null"};  ///< Which ordered rule produced the verdict
      std::string diag;  ///< Provider identity, for the debug log only

      bool operator==(const candidate_t &other) const {
        return kind == other.kind && flags == other.flags && hwnd == other.hwnd &&
               hwnd_source == other.hwnd_source;
      }
    };

    std::mutex g_state_mutex;
    candidate_t g_candidate;
    std::chrono::steady_clock::time_point g_candidate_since;
    // Bumped every time g_candidate actually changes. The refinement pass carries the value
    // it started with and discards its result if the value moved (PLAN3 F5).
    std::uint32_t g_candidate_seq {0};
    // SINGLE-SLOT pending refinement holder. A focus storm can therefore never queue
    // unbounded work, and at most one live cross-process element reference exists at a
    // time. Written by the callback, drained by the worker tick, guarded by g_state_mutex.
    IUIAutomationElement *g_pending_element {nullptr};
    std::uint32_t g_pending_seq {0};
    candidate_t g_published;
    std::chrono::steady_clock::time_point g_last_publish;
    // Tier A placeholder, mirroring state_t::input_scope in the header. There is no way to
    // read a TSF input scope out of another process, so this is always 0 and is carried
    // only so the wire format and the public state struct do not have to change if that
    // ever becomes possible.
    std::uint32_t g_published_input_scope {0};
    std::uint64_t g_generation {0};

    std::mutex g_lifecycle_mutex;
    std::jthread g_worker;
    std::atomic_bool g_running {false};

    // Owned by the worker thread between worker_body()'s acquire and release. The event
    // callback must never touch these: every one of them is a live COM object whose use
    // can block.
    //
    // The IUIAutomation client itself is deliberately NOT mirrored here. Nothing outside
    // worker_body() needs it - the tree walker and the ancestor cache request are built
    // once from it and are all the refinement pass uses - and a second file-scope raw
    // pointer with no reader is only a way to leave a dangling one behind.
    IUIAutomationTreeWalker *g_control_walker {nullptr};
    IUIAutomationCacheRequest *g_ancestor_cache {nullptr};

    // True only when IUIAutomation2's connection/transaction timeouts were actually
    // applied. The ancestor walk is skipped when false: an unbounded blocking call into
    // someone else's UI thread is not an acceptable price for a keyboard layout.
    std::atomic_bool g_have_timeouts {false};

    // Snapshot of config::input.text_field_numeric_hints, taken in start(). The keyword
    // tier is off unless the operator turned it on.
    std::atomic_bool g_numeric_hints {false};

    /**
     * @brief Record a newly observed focus candidate.
     *
     * Called from the UI Automation callback pool and from the worker's safety poll. Never
     * blocks and never publishes.
     *
     * @param refine_element Optional. When the candidate wants refinement the caller passes
     *        an element reference IT HAS ALREADY AddRef'd and hands ownership over. This
     *        function either stores it or releases it, always OUTSIDE the mutex, and always
     *        releases whatever occupied the single slot before it.
     */
    void set_candidate(const candidate_t &candidate, IUIAutomationElement *refine_element = nullptr) {
      IUIAutomationElement *displaced = nullptr;
      bool stored = false;

      {
        std::lock_guard lg {g_state_mutex};
        const bool changed = !(g_candidate == candidate);
        if (changed) {
          g_candidate = candidate;
          g_candidate_since = std::chrono::steady_clock::now();
          ++g_candidate_seq;
        }

        // The slot is refreshed even when the candidate did not change, so the element that
        // gets refined is always the one the user focused LAST. Two different controls can
        // classify identically - two text inputs in the same browser window share a kind, a
        // flags byte and a window handle - and refining the older of them would attribute
        // the first field's labels and ancestors to the second. Refreshing does NOT restart
        // the debounce or bump the sequence number: it is the same verdict either way.
        if (changed || (candidate.needs_refine && refine_element)) {
          displaced = g_pending_element;
          g_pending_element = nullptr;
          if (candidate.needs_refine && refine_element) {
            g_pending_element = refine_element;
            g_pending_seq = g_candidate_seq;
            stored = true;
          }
        }
      }

      // Released outside the mutex. Release on an IUIAutomationElement is a refcount drop
      // on an object owned by UI Automation core INSIDE THIS PROCESS - it is the same
      // operation the callback pool itself performs on `sender` the moment we return - so
      // it is not a live cross-process property call. Keeping it off the mutex still
      // matters: the worker tick takes the same lock.
      if (displaced) {
        displaced->Release();
      }
      if (refine_element && !stored) {
        refine_element->Release();
      }
    }

    /**
     * @brief Read a cached UI Automation boolean property.
     */
    bool cached_bool(IUIAutomationElement *element, PROPERTYID property, bool fallback) {
      if (!element) {
        return fallback;
      }

      VARIANT value;
      VariantInit(&value);
      if (FAILED(element->GetCachedPropertyValue(property, &value))) {
        VariantClear(&value);
        return fallback;
      }

      bool result = fallback;
      if (value.vt == VT_BOOL) {
        result = value.boolVal != VARIANT_FALSE;
      }
      VariantClear(&value);
      return result;
    }

    /**
     * @brief Read a cached UI Automation integer property.
     */
    int cached_int(IUIAutomationElement *element, PROPERTYID property, int fallback) {
      if (!element) {
        return fallback;
      }

      VARIANT value;
      VariantInit(&value);
      if (FAILED(element->GetCachedPropertyValue(property, &value))) {
        VariantClear(&value);
        return fallback;
      }

      int result = fallback;
      if (value.vt == VT_I4) {
        result = value.lVal;
      }
      VariantClear(&value);
      return result;
    }

    /**
     * @brief Read a cached UI Automation string property, or an empty string.
     */
    std::wstring cached_string(IUIAutomationElement *element, PROPERTYID property) {
      if (!element) {
        return {};
      }

      VARIANT value;
      VariantInit(&value);
      if (FAILED(element->GetCachedPropertyValue(property, &value))) {
        VariantClear(&value);
        return {};
      }

      std::wstring text;
      if (value.vt == VT_BSTR && value.bstrVal) {
        text.assign(value.bstrVal, SysStringLen(value.bstrVal));
      }
      VariantClear(&value);
      return text;
    }

    void lower_ascii(std::wstring &text) {
      for (auto &c : text) {
        if (c >= L'A' && c <= L'Z') {
          c = static_cast<wchar_t>(c - L'A' + L'a');
        }
      }
    }

    /**
     * @brief Read the cached ARIA role, lowercased ASCII, or an empty string.
     */
    std::wstring cached_aria_role(IUIAutomationElement *element) {
      auto role = cached_string(element, UIA_AriaRolePropertyId);
      lower_ascii(role);
      return role;
    }

    /**
     * @brief Fold a wide string down to ASCII for the debug log.
     *
     * Diagnostics only. Never used for classification: a localized string must never reach
     * a decision (see UIA_LocalizedControlTypePropertyId, cached for this log line alone).
     */
    std::string narrow_ascii(const std::wstring &text) {
      std::string out;
      out.reserve(text.size());
      for (const auto c : text) {
        out.push_back(c >= 0x20 && c < 0x7f ? static_cast<char>(c) : '?');
      }
      return out;
    }

    /**
     * @brief Minimal owning handle for one UI Automation element reference.
     *
     * Exists so that a std::bad_alloc out of a string or vector operation cannot leak a
     * live cross-process element reference. MinGW-w64 has neither ATL's CComPtr nor WRL's
     * ComPtr, so this is hand-rolled; it is deliberately move-only and does nothing else.
     */
    class element_ref_t {
    public:
      element_ref_t() = default;

      explicit element_ref_t(IUIAutomationElement *element):
          ptr_ {element} {}

      element_ref_t(const element_ref_t &) = delete;
      element_ref_t &operator=(const element_ref_t &) = delete;

      ~element_ref_t() {
        reset(nullptr);
      }

      /// Takes ownership of @p element and releases whatever was held before it.
      void reset(IUIAutomationElement *element) {
        IUIAutomationElement *previous = ptr_;
        ptr_ = element;
        if (previous) {
          previous->Release();
        }
      }

      IUIAutomationElement *get() const {
        return ptr_;
      }

    private:
      IUIAutomationElement *ptr_ {nullptr};
    };

    bool equals_ignore_case(const wchar_t *lhs, int lhs_len, const wchar_t *rhs, int rhs_len) {
      return CompareStringOrdinal(lhs, lhs_len, rhs, rhs_len, TRUE) == CSTR_EQUAL;
    }

    /**
     * @brief Whether a window class name identifies a classic Win32 edit control.
     */
    bool class_is_edit(const wchar_t *class_name) {
      if (!class_name || !class_name[0]) {
        return false;
      }
      if (equals_ignore_case(class_name, -1, L"Edit", -1)) {
        return true;
      }
      // RichEdit20W, RICHEDIT50W, RICHEDIT60W, ...
      constexpr int rich_edit_len = 8;  // length of L"RichEdit"
      if (std::wcslen(class_name) < static_cast<std::size_t>(rich_edit_len)) {
        return false;
      }
      return equals_ignore_case(class_name, rich_edit_len, L"RichEdit", rich_edit_len);
    }

    /**
     * @brief Whether a UI Automation ClassName names a numeric up/down control.
     *
     * EXACT match on the last dot-separated segment, case-insensitively. Never a substring:
     * "PhoneNumberBox" must not match "NumberBox", and a substring test would put a numpad
     * on a telephone-formatted text box.
     */
    bool class_name_segment_is_numeric(const std::wstring &class_name) {
      if (class_name.empty()) {
        return false;
      }

      const auto dot = class_name.find_last_of(L'.');
      const std::size_t offset = dot == std::wstring::npos ? 0 : dot + 1;
      if (offset >= class_name.size()) {
        return false;
      }
      const wchar_t *segment = class_name.c_str() + offset;
      const int segment_len = static_cast<int>(class_name.size() - offset);

      // WinUI 3 / WinUI 2 NumberBox, WPF-toolkit (Extended WPF Toolkit) up/downs and
      // Avalonia's NumericUpDown. The Avalonia entries are INFERRED from its control names,
      // not verified against a running build.
      static const wchar_t *const numeric_classes[] = {
        L"NumberBox",
        L"NumericUpDown",
        L"IntegerUpDown",
        L"DecimalUpDown",
        L"DoubleUpDown",
        L"ByteUpDown",
        L"ShortUpDown",
        L"LongUpDown",
        L"SingleUpDown",
        L"TimeSpanUpDown",
      };
      for (const auto *name : numeric_classes) {
        if (equals_ignore_case(segment, segment_len, name, -1)) {
          return true;
        }
      }
      return false;
    }

    /**
     * @brief Split a label into whole lowercase tokens for the keyword tier.
     *
     * Splits on every non-alphanumeric character, on camelCase boundaries and on
     * letter/digit transitions, so "txtPortNumber", "Port_Number" and "CVV2" all yield the
     * words a human would read. Whole-token matching is the point: substring matching turns
     * "portfolio" into a numpad.
     */
    void tokenize_for_hints(const std::wstring &text, std::vector<std::wstring> &out) {
      const auto is_upper = [](wchar_t c) {
        return c >= L'A' && c <= L'Z';
      };
      const auto is_lower = [](wchar_t c) {
        return c >= L'a' && c <= L'z';
      };
      const auto is_digit = [](wchar_t c) {
        return c >= L'0' && c <= L'9';
      };
      const auto is_alnum = [&](wchar_t c) {
        return is_upper(c) || is_lower(c) || is_digit(c);
      };

      std::wstring token;
      const auto flush = [&]() {
        if (!token.empty() && token.size() <= 32) {
          out.push_back(token);
        }
        token.clear();
      };

      for (std::size_t i = 0; i < text.size(); ++i) {
        const wchar_t c = text[i];
        if (!is_alnum(c)) {
          // Any non-ASCII-alphanumeric character is a separator. Non-English labels
          // therefore mostly produce nothing, which is the documented behaviour of this
          // tier, not a bug to fix with a translation table.
          flush();
          continue;
        }

        if (!token.empty()) {
          const wchar_t prev = text[i - 1];
          const bool camel_start = is_upper(c) && !is_upper(prev);
          const bool acronym_end = is_upper(prev) && is_upper(c) && i + 1 < text.size() && is_lower(text[i + 1]);
          const bool digit_boundary = is_digit(c) != is_digit(prev);
          if (camel_start || acronym_end || digit_boundary) {
            flush();
          }
        }

        token.push_back(is_upper(c) ? static_cast<wchar_t>(c - L'A' + L'a') : c);
      }
      flush();
    }

    bool token_in(const std::wstring &token, const wchar_t *const *words, std::size_t count) {
      for (std::size_t i = 0; i < count; ++i) {
        if (token == words[i]) {
          return true;
        }
      }
      return false;
    }

    // Word lists from PLAN3 section 5, verbatim. Two entries look like mistakes and are
    // not: "code" is in VETO because a promo/country/auth code is text at least as often as
    // it is a numeric OTP (the "otp" and "pin" entries carry that case), and "cvvcode" is
    // in VETO for the same reason its tokens already are.
    const wchar_t *const HINT_NUMERIC[] = {
      L"port", L"pin", L"cvv", L"cvc", L"otp", L"zip", L"zipcode", L"postcode", L"postal",
      L"quantity", L"qty", L"amount", L"price", L"cost", L"total", L"phone", L"telephone", L"tel",
      L"mobile", L"fax", L"age", L"year", L"month", L"day", L"hour", L"hours", L"minute", L"minutes",
      L"second", L"seconds", L"percent", L"percentage", L"width", L"height", L"fps", L"bitrate",
      L"volume", L"timeout", L"delay", L"offset", L"threshold"
    };

    const wchar_t *const HINT_VETO[] = {
      L"name", L"firstname", L"lastname", L"surname", L"email", L"mail", L"address", L"street",
      L"city", L"country", L"search", L"query", L"filter", L"password", L"passphrase", L"url",
      L"uri", L"link", L"user", L"username", L"login", L"account", L"comment", L"message", L"note",
      L"notes", L"description", L"title", L"subject", L"text", L"code", L"key", L"token", L"path",
      L"file", L"filename", L"folder", L"tag", L"tags", L"label", L"cvvcode"
    };

    // A masked field is only ever annotated as numeric on the four labels that are numeric
    // by definition (PLAN3 section 5.1). The wider list must not apply here: "amount" or
    // "year" on a password box is far more likely to be a misread than a numeric PIN.
    const wchar_t *const HINT_NUMERIC_PASSWORD[] = {
      L"pin", L"cvv", L"cvc", L"otp"
    };

    /**
     * @brief Classify the focused element into the wire-format kind and flags.
     *
     * Ordered rules R0-R8 from PLAN3 section 8.2. A rule is TERMINAL (return now) or
     * PROVISIONAL (record the verdict, mark it for refinement and let a stronger rule below
     * overwrite it).
     *
     * THIS RUNS IN THE UI AUTOMATION CALLBACK. Every read is cached; nothing here calls
     * into the focused application.
     */
    candidate_t classify_focus(IUIAutomationElement *element) {
      candidate_t result;

      // R0. No element at all.
      if (!element) {
        result.rule = "R0-null";
        return result;
      }

      // R1. Anything that cannot take typed input is not a text field. Default these to
      //     "yes" on a failed read so a provider that does not publish them is not silently
      //     discarded.
      if (!cached_bool(element, UIA_IsEnabledPropertyId, true) ||
          cached_bool(element, UIA_IsOffscreenPropertyId, false) ||
          !cached_bool(element, UIA_IsKeyboardFocusablePropertyId, true)) {
        result.rule = "R1-not-focusable";
        return result;
      }

      CONTROLTYPEID control_type = 0;
      if (FAILED(element->get_CachedControlType(&control_type))) {
        control_type = 0;
      }
      const auto class_name = cached_string(element, UIA_ClassNamePropertyId);
      const auto aria_role = cached_aria_role(element);

      // Diagnostics only - none of this decides anything. LocalizedControlType is included
      // BECAUSE it is localized: it is the field that makes a non-English bug report
      // readable, and the field that must never be keyword-matched.
      {
        auto diag = "fw="s + narrow_ascii(cached_string(element, UIA_FrameworkIdPropertyId));
        diag += " ct="s + std::to_string(static_cast<int>(control_type));
        diag += " lct="s + narrow_ascii(cached_string(element, UIA_LocalizedControlTypePropertyId));
        diag += " cls="s + narrow_ascii(class_name);
        diag += " aria="s + narrow_ascii(aria_role);
        result.diag = std::move(diag);
      }

      // R2. Resolve the window backing the focused element. Non-native providers (Chromium,
      //     WPF, Qt) report no native window handle, so fall back to the foreground thread's
      //     focus window. GetGUIThreadInfo(0, ...) reports the FOREGROUND thread and does
      //     not require AttachThreadInput.
      HWND hwnd = nullptr;
      UIA_HWND native_handle = nullptr;
      if (SUCCEEDED(element->get_CachedNativeWindowHandle(&native_handle))) {
        hwnd = static_cast<HWND>(native_handle);
      }
      if (hwnd) {
        // No cross-check needed: this handle is the element's by construction.
        result.hwnd_source = hwnd_source_e::uia_cache;
      } else {
        // THE FALLBACK MUST BE PROVEN TO BELONG TO THE SAME PROCESS AS THE ELEMENT.
        // UI Automation focus events are NOT restricted to the foreground window, so a
        // focused non-native element in a background window would otherwise be handed the
        // foreground window's focus HWND - and R3 below would then read ES_* style bits off
        // an unrelated application's edit control and return a confident verdict for the
        // wrong window. The class-name gate does not help here: it would be intact and
        // applied to the wrong window.
        GUITHREADINFO gti {};
        gti.cbSize = sizeof(gti);
        if (GetGUIThreadInfo(0, &gti) && gti.hwndFocus) {
          DWORD window_pid = 0;
          GetWindowThreadProcessId(gti.hwndFocus, &window_pid);
          const int element_pid = cached_int(element, UIA_ProcessIdPropertyId, 0);
          if (element_pid != 0 && window_pid == static_cast<DWORD>(element_pid)) {
            hwnd = gti.hwndFocus;
            result.hwnd_source = hwnd_source_e::gui_thread_info;
          }
        }
      }
      result.hwnd = hwnd;

      // R3. CLASSIC WIN32 GATE.
      //
      //     THE CLASS-NAME GATE IS LOAD-BEARING, NOT COSMETIC: ES_PASSWORD/ES_NUMBER/
      //     ES_READONLY/ES_MULTILINE share bit positions with BS_*, LBS_* and SS_*. Reading
      //     GWL_STYLE off a button, list box or static control and interpreting it as ES_*
      //     produces confident garbage. Never read these bits without the gate.
      //
      //     THE GATE IS PROVISIONAL, NOT TERMINAL. It returns a verdict only for the three
      //     styles that ARE a verdict (password, number, multiline). A plain EDIT falls
      //     through into the UI Automation rules, because WinForms' NumericUpDown inner
      //     UpDownEdit, a classic up-down buddy edit and WinUI's NumberBox InputBox are all
      //     plain EDIT windows - returning "text" here is exactly what used to lose every
      //     one of them.
      bool provisional = false;
      if (hwnd) {
        constexpr int class_name_capacity = 64;
        wchar_t win32_class[class_name_capacity] {};
        bool is_edit = false;
        if (RealGetWindowClassW(hwnd, win32_class, static_cast<UINT>(class_name_capacity))) {
          is_edit = class_is_edit(win32_class);
        }
        if (!is_edit) {
          // RealGetWindowClassW reports the base class of a superclassed control;
          // GetClassNameW reports the registered name. Either identifying the control as an
          // edit is good enough.
          wchar_t registered_name[class_name_capacity] {};
          if (GetClassNameW(hwnd, registered_name, class_name_capacity)) {
            is_edit = class_is_edit(registered_name);
          }
        }

        if (is_edit) {
          result.is_win32_edit = true;
          const LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_STYLE);
          if (style & ES_READONLY) {
            result.flags |= flag_read_only;
          }
          if (style & ES_MULTILINE) {
            result.flags |= flag_multiline;
          }

          // R3a. A masked edit is a password whatever else it is. ES_NUMBER on top of it is
          //      a numeric PIN, which the client can raise a numeric password layout for.
          if (style & ES_PASSWORD) {
            result.kind = kind_e::password;
            result.rule = "R3a-es-password";
            if (style & ES_NUMBER) {
              result.flags |= flag_numeric;
              result.rule = "R3a-es-password-number";
            }
            return result;
          }
          // R3b. ES_NUMBER is the strongest numeric signal that exists on Windows.
          if (style & ES_NUMBER) {
            result.kind = kind_e::numeric;
            result.flags |= flag_numeric;
            result.rule = "R3b-es-number";
            return result;
          }
          // R3c. A multiline edit is prose. No spinner has one.
          if (style & ES_MULTILINE) {
            result.kind = kind_e::text;
            result.rule = "R3c-es-multiline";
            return result;
          }
          // R3d. THE FIX. Provisional text; keep going.
          result.kind = kind_e::text;
          result.rule = "R3d-win32-edit-provisional";
          result.needs_refine = true;
          provisional = true;
        }
      }

      // R4-R6. UI Automation rules. Any of them outranks R3d.
      const bool matched_uia = [&]() -> bool {
        // R4. A masked field, but ONLY when the element is itself text-editable. Some
        //     providers set IsPassword on a container, and honouring that would raise a
        //     keyboard on a group box.
        if (cached_bool(element, UIA_IsPasswordPropertyId, false) &&
            (control_type == UIA_EditControlTypeId || control_type == UIA_DocumentControlTypeId)) {
          result.kind = kind_e::password;
          result.rule = "R4-uia-password";
          // Refinement may still find numeric evidence and annotate it (the PIN/CVV case).
          result.needs_refine = true;
          return true;
        }

        // R5. STRUCTURAL NUMERIC, high confidence, terminal.
        if (control_type == UIA_SpinnerControlTypeId) {
          result.kind = kind_e::numeric;
          result.rule = "R5-spinner";
        } else if (control_type == UIA_EditControlTypeId &&
                   cached_bool(element, UIA_IsRangeValuePatternAvailablePropertyId, false)) {
          // A RangeValue pattern on an edit means a bounded numeric entry. Chromium only
          // exposes RangeValue on spin buttons, sliders, meters and progress bars, never on
          // a plain text input.
          result.kind = kind_e::numeric;
          result.rule = "R5-edit-rangevalue";
        } else if (aria_role == L"spinbutton") {
          // Chromium maps <input type="number"> onto the ARIA spinbutton role.
          result.kind = kind_e::numeric;
          result.rule = "R5-aria-spinbutton";
        } else if (class_name_segment_is_numeric(class_name)) {
          result.kind = kind_e::numeric;
          result.rule = "R5-class-name";
        }
        if (result.kind == kind_e::numeric) {
          result.flags |= flag_numeric;
          result.needs_refine = false;
          return true;
        }

        // R6. STRUCTURAL TEXT.
        if (control_type == UIA_DocumentControlTypeId) {
          result.kind = kind_e::text;
          result.flags |= flag_multiline;
          result.rule = "R6-document";
          result.needs_refine = false;
          return true;
        }
        if (control_type == UIA_ComboBoxControlTypeId &&
            cached_bool(element, UIA_IsValuePatternAvailablePropertyId, false)) {
          // An editable combo box. Chromium turns <input type="number" list="..."> into one
          // of these, which is the one place a typed numeric input loses its Spinner role.
          result.kind = kind_e::text;
          result.rule = "R6-editable-combobox";
          result.needs_refine = false;
          return true;
        }
        if (aria_role == L"textbox" || aria_role == L"searchbox") {
          result.kind = kind_e::text;
          result.rule = "R6-aria-textbox";
          result.needs_refine = true;
          return true;
        }
        if (control_type == UIA_EditControlTypeId) {
          result.kind = kind_e::text;
          result.rule = "R6-edit";
          result.needs_refine = true;
          return true;
        }
        return false;
      }();

      if (matched_uia) {
        result.flags |= flag_source_uia;
      } else if (!provisional) {
        // R7. Nothing matched. Return a fully empty candidate, WINDOW HANDLE INCLUDED.
        //     Keeping the handle here would make every focus hop between two non-editable
        //     controls look like a new candidate and restart the unfocus debounce, so the
        //     client keyboard would never come down while the user tabbed around a dialog.
        candidate_t empty;
        empty.rule = "R7-no-match";
        empty.diag = std::move(result.diag);
        return empty;
      }
      // else: the provisional Win32 verdict from R3d stands, and deliberately carries no
      // flag_source_uia - it did not come from UI Automation.

      // R8. Read-only is reported but does not change the kind. The client treats read-only
      //     as "do not auto-raise" while the classification stays on the wire.
      if (cached_bool(element, UIA_ValueIsReadOnlyPropertyId, false)) {
        result.flags |= flag_read_only;
      }

      return result;
    }

    struct updown_probe_t {
      HWND target {nullptr};
      bool found {false};
      int probes {0};
    };

    /**
     * @brief EnumChildWindows callback for find_updown_buddy().
     */
    BOOL CALLBACK updown_probe_proc(HWND child, LPARAM lparam) {
      auto *probe = reinterpret_cast<updown_probe_t *>(lparam);

      constexpr int class_name_capacity = 64;
      wchar_t class_name[class_name_capacity] {};
      if (!RealGetWindowClassW(child, class_name, static_cast<UINT>(class_name_capacity))) {
        return TRUE;
      }
      if (!equals_ignore_case(class_name, -1, UPDOWN_CLASSW, -1)) {
        return TRUE;
      }
      if (++probe->probes > MAX_UPDOWN_PROBES) {
        return FALSE;
      }

      // SendMessageTimeoutW, NEVER SendMessage: this crosses into another process's UI
      // thread, and a bare send to a hung window never returns.
      DWORD_PTR buddy = 0;
      if (SendMessageTimeoutW(
            child,
            UDM_GETBUDDY,
            0,
            0,
            SMTO_ABORTIFHUNG | SMTO_BLOCK,
            UPDOWN_PROBE_TIMEOUT_MS,
            &buddy
          )) {
        if (reinterpret_cast<HWND>(buddy) == probe->target) {
          probe->found = true;
          return FALSE;  // stop enumerating
        }
      }
      return TRUE;
    }

    /**
     * @brief PLAN3 F1. Whether `edit` is the buddy of a classic up-down control.
     *
     * The classic Win32 "spin button next to a number box" is two sibling windows: an EDIT
     * and an msctls_updown32 whose UDM_GETBUDDY answer is that EDIT. Nothing about the edit
     * itself says numeric, so this sibling probe is the only way to see it.
     *
     * WORKER THREAD ONLY.
     */
    bool find_updown_buddy(HWND edit) {
      if (!edit) {
        return false;
      }
      const HWND parent = GetParent(edit);
      if (!parent) {
        return false;
      }

      updown_probe_t probe;
      probe.target = edit;
      EnumChildWindows(parent, updown_probe_proc, reinterpret_cast<LPARAM>(&probe));
      return probe.found;
    }

    /**
     * @brief PLAN3 F2. Whether the focused element sits inside a spinner.
     *
     * THE ONLY LIVE UI AUTOMATION CALL IN THIS FILE, and the reason the focus cache request
     * uses AutomationElementMode_Full. UI Automation cannot cache ancestors
     * (TreeScope_Ancestors is not a legal cache scope and FindFirst rejects it), so a live
     * element and a tree walker are the only route to "my parent is a Spinner" - the signal
     * that identifies WinForms NumericUpDown, WinUI NumberBox and the WPF-toolkit /
     * Avalonia up/downs, all of which put keyboard focus on a plain inner text box.
     *
     * Bounds, all mandatory:
     *   - worker thread only, never the event callback;
     *   - at most MAX_ANCESTOR_HOPS hops, so the walk can never reach the desktop root;
     *   - an early-out as soon as the walk leaves the control and enters window/pane/document
     *     scaffolding;
     *   - every hop is a BuildCache call whose properties are then read with get_Cached*;
     *   - the caller only reaches here when IUIAutomation2's timeouts are in force, so each
     *     hop is bounded by them (500 ms connect / 1000 ms transaction);
     *   - failure at any point returns false, which leaves the provisional verdict alone.
     *
     * The CONTROL view, not the raw view: the control view skips the presentational grids a
     * XAML template puts between a NumberBox and its InputBox, which is what keeps the real
     * container inside three hops.
     */
    bool ancestor_is_numeric(IUIAutomationElement *element) {
      if (!element || !g_control_walker || !g_ancestor_cache) {
        return false;
      }

      // Owns the ancestor of the moment; `element` itself stays the caller's.
      element_ref_t node;
      IUIAutomationElement *current = element;
      bool numeric = false;

      for (int hop = 0; hop < MAX_ANCESTOR_HOPS; ++hop) {
        IUIAutomationElement *parent = nullptr;
        const HRESULT hr = g_control_walker->GetParentElementBuildCache(current, g_ancestor_cache, &parent);
        if (FAILED(hr) || !parent) {
          break;
        }
        node.reset(parent);  // releases the previous hop
        current = parent;

        CONTROLTYPEID control_type = 0;
        if (FAILED(current->get_CachedControlType(&control_type))) {
          control_type = 0;
        }
        if (control_type == UIA_SpinnerControlTypeId ||
            cached_aria_role(current) == L"spinbutton" ||
            class_name_segment_is_numeric(cached_string(current, UIA_ClassNamePropertyId))) {
          numeric = true;
          break;
        }

        // Cheap early-out: once the walk is into window/pane/document scaffolding it has
        // left this control's container and a Spinner found beyond it would say nothing
        // about this field. Allowed one hop of slack because a XAML template can put a
        // Pane-like element directly above the input box.
        if (hop >= 1 &&
            (control_type == UIA_WindowControlTypeId || control_type == UIA_PaneControlTypeId ||
             control_type == UIA_DocumentControlTypeId)) {
          break;
        }
      }

      return numeric;
    }

    /**
     * @brief PLAN3 F3. The keyword tier. DEFAULT OFF, English-only, best effort.
     *
     * Scored asymmetrically on purpose. On Android a TYPE_CLASS_NUMBER layout has no letters
     * key at all, so a false numeric verdict is not "slightly annoying", it is "the user
     * cannot type"; a false text verdict costs three taps. Hence: veto first, whole tokens
     * only, and never on a control that could plausibly be prose.
     */
    bool hint_is_numeric(IUIAutomationElement *element, const candidate_t &snapshot) {
      if (!element) {
        return false;
      }
      // Never override a structural verdict, and never guess at prose or a dead field.
      if (snapshot.kind != kind_e::text && snapshot.kind != kind_e::password) {
        return false;
      }
      if (snapshot.flags & (flag_multiline | flag_read_only)) {
        return false;
      }

      CONTROLTYPEID control_type = 0;
      if (FAILED(element->get_CachedControlType(&control_type))) {
        control_type = 0;
      }
      if (control_type != UIA_EditControlTypeId) {
        return false;
      }

      std::vector<std::wstring> tokens;
      tokenize_for_hints(cached_string(element, UIA_AutomationIdPropertyId), tokens);
      tokenize_for_hints(cached_string(element, UIA_NamePropertyId), tokens);
      // In Chromium this is the HTML placeholder, which is the single best label a web page
      // gives us.
      tokenize_for_hints(cached_string(element, UIA_HelpTextPropertyId), tokens);

      for (const auto &token : tokens) {
        if (token_in(token, HINT_VETO, std::size(HINT_VETO))) {
          return false;
        }
      }

      const bool masked = snapshot.kind == kind_e::password;
      const wchar_t *const *words = masked ? HINT_NUMERIC_PASSWORD : HINT_NUMERIC;
      const std::size_t count = masked ? std::size(HINT_NUMERIC_PASSWORD) : std::size(HINT_NUMERIC);
      for (const auto &token : tokens) {
        if (token_in(token, words, count)) {
          return true;
        }
      }
      return false;
    }

    /**
     * @brief PLAN3 section 8.3. Try to upgrade the pending candidate. WORKER THREAD ONLY.
     *
     * Drains the single pending-refinement slot, runs F1-F3 outside the state mutex, and
     * applies the result only if no newer focus event has arrived (F5).
     *
     * DEGRADES SAFELY BY CONSTRUCTION: every probe returns "no evidence" on failure,
     * timeout or a dead element, and no evidence means the provisional verdict is published
     * exactly as it was classified. This function can upgrade text -> numeric and annotate
     * password with flag_numeric. It can never downgrade a verdict and can never produce
     * kind_e::none.
     */
    void refine_candidate() {
      element_ref_t element;
      std::uint32_t seq = 0;
      candidate_t snapshot;

      {
        std::lock_guard lg {g_state_mutex};
        if (!g_pending_element) {
          return;
        }
        element.reset(g_pending_element);  // ownership moves to this frame
        g_pending_element = nullptr;
        seq = g_pending_seq;
        snapshot = g_candidate;
      }

      bool numeric = false;
      bool low_confidence = false;
      const char *rule = nullptr;

      // F1. Classic up-down buddy. Only for a real, single-line EDIT window.
      if (snapshot.is_win32_edit && snapshot.hwnd && !(snapshot.flags & flag_multiline) &&
          find_updown_buddy(snapshot.hwnd)) {
        numeric = true;
        rule = "F1-updown-buddy";
      }

      // F2. Ancestor spinner. SKIPPED ENTIRELY without IUIAutomation2's timeouts: an
      //     unbounded blocking call into a hung provider is not an acceptable trade for a
      //     keyboard layout, so that configuration simply keeps the provisional verdict.
      if (!numeric && g_have_timeouts.load(std::memory_order_acquire) && ancestor_is_numeric(element.get())) {
        numeric = true;
        rule = "F2-ancestor";
      }

      // F3. Keyword tier, off unless the operator enabled it.
      if (!numeric && g_numeric_hints.load(std::memory_order_acquire) && hint_is_numeric(element.get(), snapshot)) {
        numeric = true;
        low_confidence = true;
        rule = "F3-hint";
      }

      if (!numeric) {
        return;
      }

      {
        std::lock_guard lg {g_state_mutex};
        // F5. Supersession: a newer focus event has already replaced what we refined.
        if (g_candidate_seq != seq) {
          return;
        }
        // Belt and braces behind the sequence check: refinement never resurrects a field.
        if (g_candidate.kind != kind_e::text && g_candidate.kind != kind_e::password) {
          return;
        }

        // F4. Password keeps its kind and gains the numeric annotation, so the client can
        //     raise a numeric password layout for a PIN or a CVV instead of a QWERTY one.
        //     Text is upgraded outright.
        g_candidate.flags |= flag_numeric;
        if (low_confidence) {
          g_candidate.flags |= flag_low_confidence;
        }
        if (g_candidate.kind == kind_e::text) {
          g_candidate.kind = kind_e::numeric;
        }
        g_candidate.needs_refine = false;
        g_candidate.rule = rule;

        // g_candidate_since is deliberately NOT reset: this is the same field the user just
        // focused, so the debounce must keep running from the focus event. Resetting it
        // would add the refinement latency to every keyboard raise.
      }
    }

    /**
     * @brief UI Automation focus-changed handler.
     *
     * Hand-rolled IUnknown over std::atomic<ULONG>. This tree is built with MinGW-w64, so
     * ATL (CComPtr) and WRL (Microsoft::WRL::RuntimeClass) are not available.
     */
    class focus_handler_t final: public IUIAutomationFocusChangedEventHandler {
    public:
      focus_handler_t() = default;

      ULONG STDMETHODCALLTYPE AddRef() override {
        return ref_count_.fetch_add(1, std::memory_order_relaxed) + 1;
      }

      ULONG STDMETHODCALLTYPE Release() override {
        const ULONG remaining = ref_count_.fetch_sub(1, std::memory_order_acq_rel) - 1;
        if (remaining == 0) {
          delete this;
        }
        return remaining;
      }

      HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **out) override {
        if (!out) {
          return E_POINTER;
        }
        if (IsEqualIID(riid, __uuidof(IUnknown)) ||
            IsEqualIID(riid, __uuidof(IUIAutomationFocusChangedEventHandler))) {
          *out = static_cast<IUIAutomationFocusChangedEventHandler *>(this);
          AddRef();
          return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
      }

      HRESULT STDMETHODCALLTYPE HandleFocusChangedEvent(IUIAutomationElement *sender) override {
        // Must not block, allocate heavily or log above debug: this runs on the UI
        // Automation callback pool. Everything below is a cached read.
        //
        // NOTHING HERE MAY WALK THE TREE. The refinement pass that does is the worker's
        // job; all this does is hand the worker the element to refine.
        //
        // Wrapped because classify_focus() builds std::string/std::wstring: an exception
        // thrown back across the COM boundary into UI Automation's own callback pool is
        // undefined behaviour, and losing one focus event is not.
        try {
          const candidate_t candidate = classify_focus(sender);
          if (candidate.needs_refine && sender) {
            sender->AddRef();  // ownership passes to set_candidate()
            set_candidate(candidate, sender);
          } else {
            set_candidate(candidate);
          }
        } catch (...) {
          // Deliberately silent: this runs on someone else's thread pool and the next focus
          // event, or the safety poll, will correct the state.
        }
        return S_OK;
      }

    private:
      ~focus_handler_t() = default;

      std::atomic<ULONG> ref_count_ {1};
    };

    /**
     * @brief Build the cache request that populates every property classify_focus() reads.
     *
     * Named SDK constants only. Numeric UI Automation property IDs are forbidden here: the
     * values are not part of any stable contract this tree can verify.
     */
    IUIAutomationCacheRequest *make_cache_request(IUIAutomation *automation) {
      IUIAutomationCacheRequest *cache = nullptr;
      if (FAILED(automation->CreateCacheRequest(&cache)) || !cache) {
        return nullptr;
      }

      static const PROPERTYID properties[] = {
        UIA_ControlTypePropertyId,
        UIA_IsPasswordPropertyId,
        UIA_NativeWindowHandlePropertyId,
        UIA_ProcessIdPropertyId,
        UIA_IsEnabledPropertyId,
        UIA_IsOffscreenPropertyId,
        UIA_IsKeyboardFocusablePropertyId,
        UIA_ValueIsReadOnlyPropertyId,
        UIA_IsValuePatternAvailablePropertyId,
        UIA_IsRangeValuePatternAvailablePropertyId,
        UIA_AriaRolePropertyId,
        // WinUI NumberBox and the WPF-toolkit / Avalonia up/downs are identified by their
        // automation peer class name.
        UIA_ClassNamePropertyId,
        // Diagnostics and per-stack gating: "Chrome", "Qt", "WPF", "WinForm", "Win32", "XAML".
        UIA_FrameworkIdPropertyId,
        // Keyword tier (F3) only.
        UIA_AutomationIdPropertyId,
        UIA_NamePropertyId,
        UIA_HelpTextPropertyId,
        // DEBUG LOG ONLY. This string is LOCALIZED; keyword-matching it would be a bug on
        // every non-English Windows.
        UIA_LocalizedControlTypePropertyId,
      };
      for (const auto property : properties) {
        if (FAILED(cache->AddProperty(property))) {
          cache->Release();
          return nullptr;
        }
      }

      // AutomationElementMode_Full, where this used to be _None. _None hands back a pure
      // snapshot with no reference to the live UI, which is safer but makes
      // GetParentElementBuildCache impossible - and the ancestor is where the numeric truth
      // lives for every framework that puts focus on an inner text box. The safety that
      // _None bought is reconstructed around the walk itself: see refine_candidate() and
      // ancestor_is_numeric(). Property reads stay cached either way.
      cache->put_AutomationElementMode(AutomationElementMode_Full);
      cache->put_TreeScope(TreeScope_Element);
      return cache;
    }

    /**
     * @brief Build the cache request used for each hop of the ancestor walk.
     *
     * AutomationElementMode_None here, and only here: an ancestor is read once and dropped,
     * so it never needs a live reference of its own.
     */
    IUIAutomationCacheRequest *make_ancestor_cache_request(IUIAutomation *automation) {
      IUIAutomationCacheRequest *cache = nullptr;
      if (FAILED(automation->CreateCacheRequest(&cache)) || !cache) {
        return nullptr;
      }

      static const PROPERTYID properties[] = {
        UIA_ControlTypePropertyId,
        UIA_ClassNamePropertyId,
        UIA_AriaRolePropertyId,
        // Cached because the design calls for it, and deliberately NOT used as a trigger on
        // its own: Slider, ScrollBar, Meter and ProgressBar all expose RangeValue too, so an
        // ancestor that merely has the pattern is not evidence that this field is numeric.
        UIA_IsRangeValuePatternAvailablePropertyId,
      };
      for (const auto property : properties) {
        if (FAILED(cache->AddProperty(property))) {
          cache->Release();
          return nullptr;
        }
      }

      cache->put_AutomationElementMode(AutomationElementMode_None);
      cache->put_TreeScope(TreeScope_Element);
      return cache;
    }

    /**
     * @brief Create the UI Automation client, preferring the timeout-capable variant.
     *
     * Sets g_have_timeouts, which gates the ancestor walk.
     */
    IUIAutomation *create_automation() {
      IUIAutomation *automation = nullptr;
      g_have_timeouts.store(false, std::memory_order_release);

#if SUNSHINE_TEXT_FIELD_HAVE_UIA2
      // CUIAutomation8 also serves IUIAutomation, so ask for the base interface and query
      // up only to set the timeouts.
      if (FAILED(CoCreateInstance(
            __uuidof(CUIAutomation8),
            nullptr,
            CLSCTX_INPROC_SERVER,
            __uuidof(IUIAutomation),
            reinterpret_cast<void **>(&automation)
          ))) {
        automation = nullptr;
      }

      if (automation) {
        IUIAutomation2 *automation2 = nullptr;
        if (SUCCEEDED(automation->QueryInterface(__uuidof(IUIAutomation2), reinterpret_cast<void **>(&automation2))) &&
            automation2) {
          const HRESULT connect_timeout = automation2->put_ConnectionTimeout(UIA_CONNECTION_TIMEOUT_MS);
          const HRESULT transaction_timeout = automation2->put_TransactionTimeout(UIA_TRANSACTION_TIMEOUT_MS);
          if (SUCCEEDED(connect_timeout) && SUCCEEDED(transaction_timeout)) {
            g_have_timeouts.store(true, std::memory_order_release);
          }
          automation2->Release();
        }
        return automation;
      }
#endif

      if (FAILED(CoCreateInstance(
            __uuidof(CUIAutomation),
            nullptr,
            CLSCTX_INPROC_SERVER,
            __uuidof(IUIAutomation),
            reinterpret_cast<void **>(&automation)
          ))) {
        return nullptr;
      }
      return automation;
    }

    /**
     * @brief Win32 backstop that can invalidate a stale focus belief.
     *
     * UI Automation can stop delivering events without any error: the provider process
     * crashes, the workstation locks and the secure desktop takes over, or the UI Automation
     * core hiccups. Without this poll the client keyboard would stay up forever. The poll
     * can only ever invalidate a field, never detect one.
     *
     * The comparison is against the ROOT window rather than the exact focus window.
     * Exact-handle equality produces false unfocus events whenever a provider reports a
     * container handle while keyboard focus sits on a child (Win32 combo boxes, some WinUI
     * and Electron surfaces). Every failure mode this poll exists to catch changes the root
     * window or removes focus entirely, so root comparison loses nothing real.
     *
     * gti.hwndCaret is deliberately NOT consulted: Chromium, WPF, WinUI and Qt draw their
     * own carets and never call CreateCaret, so a null caret must never veto a
     * classification.
     */
    void run_safety_poll() {
      candidate_t snapshot;
      {
        std::lock_guard lg {g_state_mutex};
        snapshot = g_candidate;
      }

      if (snapshot.kind == kind_e::none) {
        return;
      }

      bool focus_lost = false;

      GUITHREADINFO gti {};
      gti.cbSize = sizeof(gti);
      if (!GetGUIThreadInfo(0, &gti)) {
        focus_lost = true;
      } else if (!gti.hwndFocus) {
        focus_lost = true;
      } else if (!GetForegroundWindow()) {
        focus_lost = true;
      } else if (snapshot.hwnd) {
        const HWND focus_root = GetAncestor(gti.hwndFocus, GA_ROOT);
        const HWND believed_root = GetAncestor(snapshot.hwnd, GA_ROOT);
        if (focus_root != believed_root) {
          focus_lost = true;
        }
      }

      if (focus_lost) {
        candidate_t cleared;
        cleared.rule = "safety-poll";
        set_candidate(cleared);
      }
    }

    /**
     * @brief Debounce and publish. Runs on the worker thread only.
     */
    void run_publish_tick() {
      // BEFORE the debounce check, so a provisional verdict is normally upgraded before it
      // is ever published. At most one refinement per tick, from a single slot.
      refine_candidate();

      const auto now = std::chrono::steady_clock::now();

      bool wire_changed = false;
      candidate_t published;
      std::uint64_t generation = 0;

      {
        std::lock_guard lg {g_state_mutex};
        if (g_candidate == g_published) {
          return;
        }

        const auto debounce = g_candidate.kind == kind_e::none ? NONE_DEBOUNCE : FIELD_DEBOUNCE;
        if (now - g_candidate_since < debounce) {
          return;
        }
        if (now - g_last_publish < MIN_PUBLISH_INTERVAL) {
          return;
        }

        // The window handle is part of the published state so the safety poll keeps tracking
        // the right window, but it is not on the wire, so moving between two fields of the
        // same kind must not burn a generation (and therefore a packet).
        wire_changed =
          g_candidate.kind != g_published.kind || g_candidate.flags != g_published.flags;

        g_published = g_candidate;
        g_last_publish = now;

        if (wire_changed) {
          ++g_generation;
        }
        published = g_published;
        generation = g_generation;
      }

      // Logged outside the lock: the UI Automation callback pool takes the same mutex and
      // must never wait on a log sink. The rule id is the point of this line - without it a
      // "wrong keyboard" report from the field is unactionable, and field reports are the
      // only way this feature's coverage can be measured at all.
      if (wire_changed) {
        BOOST_LOG(debug) << "Text field focus: kind="sv
                         << static_cast<unsigned>(published.kind)
                         << " flags="sv << static_cast<unsigned>(published.flags)
                         << " rule="sv << (published.rule ? published.rule : "?")
                         << " hwnd_source="sv << hwnd_source_name(published.hwnd_source)
                         << " " << published.diag
                         << " generation="sv << generation;
      }
    }

    /**
     * @brief Release anything left in the pending-refinement slot.
     *
     * MUST run on the worker thread: it created the MTA the elements live in.
     */
    void drain_pending_element() {
      IUIAutomationElement *pending = nullptr;
      {
        std::lock_guard lg {g_state_mutex};
        pending = g_pending_element;
        g_pending_element = nullptr;
      }
      if (pending) {
        pending->Release();
      }
    }

    /**
     * @brief The worker's actual work, between CoInitializeEx() and CoUninitialize().
     *
     * Split out so worker_main() can wrap it in a catch-all without also having to own the
     * COM apartment lifetime.
     */
    void worker_body(std::stop_token stop_token) {
      IUIAutomation *automation = create_automation();
      if (!automation) {
        BOOST_LOG(warning) << "Text field detection: UI Automation unavailable, focus events disabled"sv;
        return;
      }

      IUIAutomationCacheRequest *cache = make_cache_request(automation);
      if (!cache) {
        BOOST_LOG(warning) << "Text field detection: could not build the UI Automation cache request, focus events disabled"sv;
        automation->Release();
        return;
      }

      // The ancestor walk is a best-effort refinement: if either of these cannot be built
      // the watcher runs exactly as it did before them, one verdict weaker.
      IUIAutomationCacheRequest *ancestor_cache = make_ancestor_cache_request(automation);
      IUIAutomationTreeWalker *control_walker = nullptr;
      if (FAILED(automation->get_ControlViewWalker(&control_walker))) {
        control_walker = nullptr;
      }
      if (!ancestor_cache || !control_walker || !g_have_timeouts.load(std::memory_order_acquire)) {
        BOOST_LOG(info) << "Text field detection: ancestor refinement disabled (no UI Automation timeouts or tree walker); "
                           "numeric fields whose control puts focus on an inner text box will report as text"sv;
      }

      // Published for the worker's own use only. The event callback must never touch them.
      g_control_walker = control_walker;
      g_ancestor_cache = ancestor_cache;

      auto *handler = new focus_handler_t();
      const HRESULT registered = automation->AddFocusChangedEventHandler(cache, handler);
      if (FAILED(registered)) {
        BOOST_LOG(warning) << "Text field detection: could not subscribe to UI Automation focus events, focus events disabled"sv;
        g_control_walker = nullptr;
        g_ancestor_cache = nullptr;
        handler->Release();
        if (control_walker) {
          control_walker->Release();
        }
        if (ancestor_cache) {
          ancestor_cache->Release();
        }
        cache->Release();
        automation->Release();
        return;
      }

      // Logged at info exactly once. A bug report that shows this line but no publish lines
      // below distinguishes "the subscription never fired" from "it fired but misclassified".
      BOOST_LOG(info) << "Text field detection: subscribed to UI Automation focus events"sv;
      g_running.store(true, std::memory_order_release);

      int tick = 0;
      while (!stop_token.stop_requested()) {
        run_publish_tick();
        if (++tick >= SAFETY_POLL_TICKS) {
          tick = 0;
          run_safety_poll();
        }
        std::this_thread::sleep_for(TICK_INTERVAL);
      }

      g_running.store(false, std::memory_order_release);

      // RemoveAllEventHandlers() before releasing anything. That plus CoUninitialize() on
      // the creating thread (worker_main, below) is not optional: UI Automation teardown
      // from the wrong thread is a known deadlock.
      automation->RemoveAllEventHandlers();

      // After RemoveAllEventHandlers(), so no callback can refill the slot behind us, and on
      // this thread, which owns the apartment those elements were marshalled into.
      g_control_walker = nullptr;
      g_ancestor_cache = nullptr;
      drain_pending_element();

      handler->Release();
      if (control_walker) {
        control_walker->Release();
      }
      if (ancestor_cache) {
        ancestor_cache->Release();
      }
      cache->Release();
      automation->Release();
    }

    void worker_main(std::stop_token stop_token) {
      SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);

      const HRESULT co_init = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
      if (FAILED(co_init)) {
        BOOST_LOG(warning) << "Text field detection: COM initialization failed, focus events disabled"sv;
        return;
      }

      // An exception escaping a std::jthread body calls std::terminate() and takes the whole
      // host process down with it. Nothing in worker_body() is expected to throw - the UI
      // Automation surface is HRESULT-based - but operator new and the logging sink both
      // allocate. Losing focus detection is survivable; losing the stream is not.
      try {
        worker_body(stop_token);
      } catch (...) {
        // This path leaks the UI Automation interfaces and skips RemoveAllEventHandlers,
        // because there is nothing sane left to unwind through. It happens at most once per
        // process and the watcher stays down afterwards, which is a far better outcome than
        // terminating the host mid-stream.
        g_running.store(false, std::memory_order_release);
        g_control_walker = nullptr;
        g_ancestor_cache = nullptr;
        try {
          BOOST_LOG(error) << "Text field detection: the focus watcher thread stopped after an "
                              "unexpected exception, focus events disabled"sv;
        } catch (...) {
          // A logger that throws while reporting a throw is not worth a second attempt.
        }
      }

      // Same thread that created the UI Automation client, as its teardown requires.
      CoUninitialize();
    }

  }  // namespace

  bool start() {
    std::lock_guard lg {g_lifecycle_mutex};
    if (g_worker.joinable()) {
      return true;
    }

    // Read once, here, on the caller's thread: the worker and the callback pool only ever
    // see the atomic.
    g_numeric_hints.store(config::input.text_field_numeric_hints, std::memory_order_release);

    {
      std::lock_guard state_lg {g_state_mutex};
      g_candidate = candidate_t {};
      g_published = candidate_t {};
      g_candidate_since = std::chrono::steady_clock::now();
      g_last_publish = std::chrono::steady_clock::time_point {};
      g_candidate_seq = 0;
      g_pending_seq = 0;
      g_published_input_scope = 0;
      ++g_generation;
      // g_pending_element is deliberately not touched: only the worker thread may release a
      // UI Automation element, and worker_body() drains the slot before it exits.
    }

    try {
      g_worker = std::jthread {[](std::stop_token stop_token) {
        worker_main(stop_token);
      }};
    } catch (...) {
      BOOST_LOG(warning) << "Text field detection: could not start the focus watcher thread"sv;
      return false;
    }
    return true;
  }

  void stop() {
    std::lock_guard lg {g_lifecycle_mutex};
    if (!g_worker.joinable()) {
      return;
    }

    g_worker.request_stop();
    g_worker.join();
    g_worker = {};

    // Reset after the join so the worker cannot race a final publish over the top.
    std::lock_guard state_lg {g_state_mutex};
    g_candidate = candidate_t {};
    g_published = candidate_t {};
    g_published_input_scope = 0;
    ++g_generation;
  }

  bool running() {
    return g_running.load(std::memory_order_acquire);
  }

  state_t current() {
    std::lock_guard lg {g_state_mutex};
    state_t state;
    state.kind = g_published.kind;
    state.flags = g_published.flags;
    state.input_scope = g_published_input_scope;
    state.generation = g_generation;
    return state;
  }

}  // namespace platf::text_field
