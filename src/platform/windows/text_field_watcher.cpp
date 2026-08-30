/**
 * @file src/platform/windows/text_field_watcher.cpp
 * @brief In-process UI Automation focus watcher used to drive the client-side soft keyboard.
 *
 * Threading model
 * ---------------
 * One worker thread owns everything COM. It initialises COM as MTA, creates the UI
 * Automation client, registers a focus-changed handler and then ticks a small debounce
 * state machine. UI Automation delivers focus events on its own thread pool; those
 * callbacks only record a candidate under the state mutex and return immediately. All
 * publishing happens on the worker tick, so a hung application can never stall an event
 * callback into stalling the stream.
 *
 * Every UI Automation property read here is a CACHED read. A live (non-cached) accessor
 * issues a synchronous cross-process call from inside an event callback, which is the
 * classic way to wedge the UI Automation callback pool against an unresponsive
 * application. Cached reads are served entirely from the snapshot the focus subscription
 * already delivered, so they cannot block on the focused application at all.
 */

#include "text_field_watcher.h"

#include "src/logging.h"

#include <atomic>
#include <chrono>
#include <cwchar>
#include <mutex>
#include <string>
#include <thread>

// clang-format off
#include <winsock2.h>
#include <windows.h>
#include <objbase.h>
#include <oleauto.h>
#include <uiautomation.h>
// clang-format on

// mingw-w64 and the Windows SDK both gate the CUIAutomation8 coclass and the
// IUIAutomation2 interface behind the MIDL guards below. CUIAutomation8 is a Windows 8+
// coclass whose only benefit here is IUIAutomation2's connection/transaction timeouts,
// which bound how long a call into a hung provider can block. When the headers predate
// it we fall back to the original CUIAutomation coclass and simply lose that protection.
#if defined(__CUIAutomation8_FWD_DEFINED__) && defined(__IUIAutomation2_INTERFACE_DEFINED__)
  #define SUNSHINE_TEXT_FIELD_HAVE_UIA2 1
#else
  #define SUNSHINE_TEXT_FIELD_HAVE_UIA2 0
#endif

using namespace std::literals;

namespace platf::text_field {
  namespace {

    // Transition TO a field. Long enough to swallow the focus storm a dialog produces
    // while it builds its control tree, short enough to feel immediate.
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

    /**
     * @brief Where the window handle backing a classification came from.
     *
     * Recorded because the whole Win32 refinement - the only reliable source of a numeric
     * verdict for desktop applications - depends on the cached NativeWindowHandle actually
     * being readable under AutomationElementMode_None. If it never is, the watcher still
     * fires and still classifies, so nothing looks broken; the only visible symptom would
     * be that numeric fields are never reported. Logging the provenance makes that
     * distinguishable from a debug log without any code change.
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

    struct candidate_t {
      kind_e kind {kind_e::none};
      std::uint8_t flags {0};
      HWND hwnd {nullptr};
      hwnd_source_e hwnd_source {hwnd_source_e::none};

      bool operator==(const candidate_t &other) const {
        return kind == other.kind && flags == other.flags && hwnd == other.hwnd &&
               hwnd_source == other.hwnd_source;
      }
    };

    std::mutex g_state_mutex;
    candidate_t g_candidate;
    std::chrono::steady_clock::time_point g_candidate_since;
    candidate_t g_published;
    std::chrono::steady_clock::time_point g_last_publish;
    // Tier A placeholder, mirroring state_t::input_scope in the header. Tier B has no way to
    // read a TSF input scope out of another process, so this is always 0 and is carried only
    // so the wire format and the public state struct do not have to change when a future
    // implementation can fill it in. classify_focus() is the function that would set it.
    std::uint32_t g_published_input_scope {0};
    std::uint64_t g_generation {0};

    std::mutex g_lifecycle_mutex;
    std::jthread g_worker;
    std::atomic_bool g_running {false};

    /**
     * @brief Record a newly observed focus candidate.
     *
     * Called from the UI Automation callback pool and from the worker's safety poll.
     * Never blocks and never publishes.
     */
    void set_candidate(const candidate_t &candidate) {
      std::lock_guard lg {g_state_mutex};
      if (g_candidate == candidate) {
        return;
      }
      g_candidate = candidate;
      g_candidate_since = std::chrono::steady_clock::now();
    }

    /**
     * @brief Read a cached UI Automation boolean property.
     *
     * Uses GetCachedPropertyValue rather than a live accessor so the read is served from
     * the cache the focus subscription already populated and cannot block.
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
     *
     * Cached for the same reason as cached_bool(): a live accessor would block on the
     * focused application from inside an event callback.
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
     * @brief Read the cached ARIA role, lowercased ASCII, or an empty string.
     */
    std::wstring cached_aria_role(IUIAutomationElement *element) {
      if (!element) {
        return {};
      }

      VARIANT value;
      VariantInit(&value);
      if (FAILED(element->GetCachedPropertyValue(UIA_AriaRolePropertyId, &value))) {
        VariantClear(&value);
        return {};
      }

      std::wstring role;
      if (value.vt == VT_BSTR && value.bstrVal) {
        role.assign(value.bstrVal, SysStringLen(value.bstrVal));
      }
      VariantClear(&value);

      for (auto &c : role) {
        if (c >= L'A' && c <= L'Z') {
          c = static_cast<wchar_t>(c - L'A' + L'a');
        }
      }
      return role;
    }

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
     * @brief Classify the focused element into the wire-format kind and flags.
     *
     * THIS FUNCTION IS THE ENTIRE TIER B / TIER A SEAM. A future implementation that can
     * read the real TSF input scope of the focused control replaces this function and
     * nothing else; the wire format already carries an input_scope field for it.
     *
     * Ordered rules, first match wins. See PLAN2 section 2.3.
     */
    candidate_t classify_focus(IUIAutomationElement *element) {
      candidate_t result;

      // 1. No element at all.
      if (!element) {
        return result;
      }

      // 2. Anything that cannot take typed input is not a text field. Default these to
      //    "yes" on a failed read so a provider that does not publish them is not
      //    silently discarded.
      if (!cached_bool(element, UIA_IsEnabledPropertyId, true) ||
          cached_bool(element, UIA_IsOffscreenPropertyId, false) ||
          !cached_bool(element, UIA_IsKeyboardFocusablePropertyId, true)) {
        return result;
      }

      // Resolve the window backing the focused element. Non-native providers (Chromium,
      // WPF, Qt) report no native window handle, so fall back to the foreground thread's
      // focus window. GetGUIThreadInfo(0, ...) reports the FOREGROUND thread and does not
      // require AttachThreadInput.
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
        // foreground window's focus HWND - and step 3 below would then read ES_* style bits
        // off an unrelated application's edit control and return a confident verdict for the
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

      // 3. Classic Win32 refinement. This is the strongest signal available in Tier B and
      //    the only source of a reliable "numeric" verdict for desktop applications.
      //
      //    THE CLASS-NAME GATE IS LOAD-BEARING, NOT COSMETIC: ES_PASSWORD/ES_NUMBER/
      //    ES_READONLY/ES_MULTILINE share bit positions with BS_*, LBS_* and SS_*.
      //    Reading GWL_STYLE off a button, list box or static control and interpreting it
      //    as ES_* produces confident garbage. Never read these bits without the gate.
      if (hwnd) {
        constexpr int class_name_capacity = 64;
        wchar_t class_name[class_name_capacity] {};
        bool is_edit = false;
        if (RealGetWindowClassW(hwnd, class_name, static_cast<UINT>(class_name_capacity))) {
          is_edit = class_is_edit(class_name);
        }
        if (!is_edit) {
          // RealGetWindowClassW reports the base class of a superclassed control;
          // GetClassNameW reports the registered name. Either identifying the control as
          // an edit is good enough.
          wchar_t registered_name[class_name_capacity] {};
          if (GetClassNameW(hwnd, registered_name, class_name_capacity)) {
            is_edit = class_is_edit(registered_name);
          }
        }

        if (is_edit) {
          const LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_STYLE);
          if (style & ES_PASSWORD) {
            result.kind = kind_e::password;
          } else if (style & ES_NUMBER) {
            result.kind = kind_e::numeric;
          } else {
            result.kind = kind_e::text;
          }
          if (style & ES_READONLY) {
            result.flags |= flag_read_only;
          }
          if (style & ES_MULTILINE) {
            result.flags |= flag_multiline;
          }
          // Deliberately not flagged as flag_source_uia: this verdict came from Win32.
          return result;
        }
      }

      // 4. UI Automation classification.
      if (cached_bool(element, UIA_IsPasswordPropertyId, false)) {
        result.kind = kind_e::password;
      } else {
        CONTROLTYPEID control_type = 0;
        if (FAILED(element->get_CachedControlType(&control_type))) {
          control_type = 0;
        }

        if (control_type == UIA_EditControlTypeId) {
          // A RangeValue pattern on an edit means a bounded numeric entry (spin box).
          result.kind = cached_bool(element, UIA_IsRangeValuePatternAvailablePropertyId, false)
                          ? kind_e::numeric
                          : kind_e::text;
        } else if (control_type == UIA_SpinnerControlTypeId) {
          result.kind = kind_e::numeric;
        } else if (control_type == UIA_DocumentControlTypeId) {
          result.kind = kind_e::text;
          result.flags |= flag_multiline;
        } else if (control_type == UIA_ComboBoxControlTypeId &&
                   cached_bool(element, UIA_IsValuePatternAvailablePropertyId, false)) {
          result.kind = kind_e::text;
        } else {
          // Chromium maps <input type="number"> onto the ARIA spinbutton role, which is
          // the one genuinely reliable cross-process numeric signal for web content.
          const auto role = cached_aria_role(element);
          if (role == L"spinbutton") {
            result.kind = kind_e::numeric;
          } else if (role == L"textbox" || role == L"searchbox") {
            result.kind = kind_e::text;
          }
        }
      }

      if (result.kind == kind_e::none) {
        // Return a fully empty candidate, window handle included. Keeping the handle here
        // would make every focus hop between two non-editable controls look like a new
        // candidate and restart the unfocus debounce, so the client keyboard would never
        // come down while the user tabbed around a dialog.
        return candidate_t {};
      }

      result.flags |= flag_source_uia;

      // 5. Read-only is reported but does not change the kind. The client treats
      //    read-only as "do not auto-raise" while the classification stays on the wire.
      if (cached_bool(element, UIA_ValueIsReadOnlyPropertyId, false)) {
        result.flags |= flag_read_only;
      }

      return result;
    }

    /**
     * @brief UI Automation focus-changed handler.
     *
     * Hand-rolled IUnknown over std::atomic<ULONG>. This tree is built with MinGW-w64,
     * so ATL (CComPtr) and WRL (Microsoft::WRL::RuntimeClass) are not available.
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
        // Automation callback pool.
        set_candidate(classify_focus(sender));
        return S_OK;
      }

    private:
      ~focus_handler_t() = default;

      std::atomic<ULONG> ref_count_ {1};
    };

    /**
     * @brief Build the cache request that populates every property classify_focus() reads.
     *
     * Named SDK constants only. Numeric UI Automation property IDs are forbidden here:
     * the values are not part of any stable contract this tree can verify.
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
      };
      for (const auto property : properties) {
        if (FAILED(cache->AddProperty(property))) {
          cache->Release();
          return nullptr;
        }
      }

      // AutomationElementMode_None keeps no live cross-process reference on the delivered
      // elements, so a provider that dies cannot leave us holding a stale proxy.
      cache->put_AutomationElementMode(AutomationElementMode_None);
      cache->put_TreeScope(TreeScope_Element);
      return cache;
    }

    /**
     * @brief Create the UI Automation client, preferring the timeout-capable variant.
     */
    IUIAutomation *create_automation() {
      IUIAutomation *automation = nullptr;

#if SUNSHINE_TEXT_FIELD_HAVE_UIA2
      // CUIAutomation8 also serves IUIAutomation, so ask for the base interface and
      // query up only to set the timeouts.
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
          automation2->put_ConnectionTimeout(UIA_CONNECTION_TIMEOUT_MS);
          automation2->put_TransactionTimeout(UIA_TRANSACTION_TIMEOUT_MS);
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
     * crashes, the workstation locks and the secure desktop takes over, or the UI
     * Automation core hiccups. Without this poll the client keyboard would stay up
     * forever. The poll can only ever invalidate a field, never detect one.
     *
     * The comparison is against the ROOT window rather than the exact focus window.
     * Exact-handle equality produces false unfocus events whenever a provider reports a
     * container handle while keyboard focus sits on a child (Win32 combo boxes, some
     * WinUI and Electron surfaces). Every failure mode this poll exists to catch changes
     * the root window or removes focus entirely, so root comparison loses nothing real.
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
        set_candidate(candidate_t {});
      }
    }

    /**
     * @brief Debounce and publish. Runs on the worker thread only.
     */
    void run_publish_tick() {
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

        // The window handle is part of the published state so the safety poll keeps
        // tracking the right window, but it is not on the wire, so moving between two
        // fields of the same kind must not burn a generation (and therefore a packet).
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
      // must never wait on a log sink.
      if (wire_changed) {
        BOOST_LOG(debug) << "Text field focus: kind="sv
                         << static_cast<unsigned>(published.kind)
                         << " flags="sv << static_cast<unsigned>(published.flags)
                         << " hwnd_source="sv << hwnd_source_name(published.hwnd_source)
                         << " generation="sv << generation;
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

      auto *handler = new focus_handler_t();
      const HRESULT registered = automation->AddFocusChangedEventHandler(cache, handler);
      if (FAILED(registered)) {
        BOOST_LOG(warning) << "Text field detection: could not subscribe to UI Automation focus events, focus events disabled"sv;
        handler->Release();
        cache->Release();
        automation->Release();
        return;
      }

      // Logged at info exactly once. A bug report that shows this line but no publish
      // lines below distinguishes "the subscription never fired" (see PLAN2 R1: UI
      // Automation delivery from a SYSTEM token to a medium-integrity application) from
      // "it fired but misclassified".
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
      handler->Release();
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

    {
      std::lock_guard state_lg {g_state_mutex};
      g_candidate = candidate_t {};
      g_published = candidate_t {};
      g_candidate_since = std::chrono::steady_clock::now();
      g_last_publish = std::chrono::steady_clock::time_point {};
      g_published_input_scope = 0;
      ++g_generation;
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
