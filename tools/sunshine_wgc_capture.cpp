/**
 * @file sunshine_wgc_capture.cpp
 * @brief Windows Graphics Capture helper process for Sunshine.
 *
 * This standalone executable provides Windows Graphics Capture functionality
 * for the main Sunshine streaming process. It runs as a separate process to
 * isolate WGC operations and handle secure desktop scenarios. The helper
 * communicates with the main process via a control pipe, shared D3D11 texture,
 * shared frame metadata, and a frame-ready event.
 */

#define WIN32_LEAN_AND_MEAN

// standard includes
#include <atomic>
#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <format>
#include <functional>
#include <list>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

// local includes
#include "src/logging.h"
#include "src/platform/windows/ipc/misc_utils.h"
#include "src/platform/windows/ipc/pipes.h"
#include "src/platform/windows/wgc_capture_policy.h"
#include "src/platform/windows/window_capture_policy.h"
#include "src/stat_trackers.h"
#include "src/utility.h"  // For RAII utilities

// platform includes
#include <d3d11.h>
#include <dwmapi.h>
#include <dxgi1_2.h>
#include <inspectable.h>  // For IInspectable
#include <KnownFolders.h>
#include <ShellScalingApi.h>  // For DPI awareness
#include <ShlObj.h>
#include <Windows.Graphics.Capture.Interop.h>
#include <Windows.h>
#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Metadata.h>  // For ApiInformation
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>

// Manual declaration for CreateDirect3D11DeviceFromDXGIDevice if missing
extern "C" {
  HRESULT __stdcall CreateDirect3D11DeviceFromDXGIDevice(::IDXGIDevice *dxgiDevice, ::IInspectable **graphicsDevice);
}

/**
 * Windows structures sometimes have compile-time GUIDs. GCC supports this, but in a roundabout way.
 * If WINRT_IMPL_HAS_DECLSPEC_UUID is true, then the compiler supports adding this attribute to a struct. For example, Visual Studio.
 * If not, then MinGW GCC has a workaround to assign a GUID to a structure.
 */
struct
#if WINRT_IMPL_HAS_DECLSPEC_UUID
  __declspec(uuid("A9B3D012-3DF2-4EE3-B8D1-8695F457D3C1"))
#endif
  IDirect3DDxgiInterfaceAccess: ::IUnknown {
  virtual HRESULT __stdcall GetInterface(REFIID id, IUnknown **object) = 0;
};

#if !WINRT_IMPL_HAS_DECLSPEC_UUID
static constexpr GUID GUID__IDirect3DDxgiInterfaceAccess = {
  0xA9B3D012,
  0x3DF2,
  0x4EE3,
  {0xB8, 0xD1, 0x86, 0x95, 0xF4, 0x57, 0xD3, 0xC1}
};

template<>
constexpr auto __mingw_uuidof<IDirect3DDxgiInterfaceAccess>() -> GUID const & {
  return GUID__IDirect3DDxgiInterfaceAccess;
}

static constexpr GUID GUID__IDirect3DSurface = {
  0x0BF4A146,
  0x13C1,
  0x4694,
  {0xBE, 0xE3, 0x7A, 0xBF, 0x15, 0xEA, 0xF5, 0x86}
};

template<>
constexpr auto __mingw_uuidof<winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DSurface>() -> GUID const & {
  return GUID__IDirect3DSurface;
}
#endif

using namespace winrt;
using namespace winrt::Windows::Foundation;
using namespace winrt::Windows::Graphics;
using namespace winrt::Windows::Graphics::Capture;
using namespace winrt::Windows::Graphics::DirectX;
using namespace winrt::Windows::Graphics::DirectX::Direct3D11;
using namespace platf::dxgi;

// GPU scheduling priority definitions for optimal capture performance under high GPU load
enum class D3DKMT_SchedulingPriorityClass : LONG {
  IDLE = 0,
  BELOW_NORMAL = 1,
  NORMAL = 2,
  ABOVE_NORMAL = 3,
  HIGH = 4,
  REALTIME = 5
};

using PD3DKMTSetProcessSchedulingPriorityClass = LONG(__stdcall *)(HANDLE, LONG);

/**
 * @brief D3D11 device creation flags for the WGC helper process.
 *
 * NOTE: This constant is NOT shared with the main process. It is defined here separately
 * because including display.h would pull in too many dependencies for this standalone helper.
 * If you change this flag, you MUST update it in both this file and src/platform/windows/display.h.
 */
constexpr UINT D3D11_CREATE_DEVICE_FLAGS = 0;

/**
 * @brief Initial log level for the helper process.
 */
const int INITIAL_LOG_LEVEL = 2;

/**
 * @brief Global configuration data received from the main process.
 */
constexpr uint32_t DEFAULT_WGC_IPC_FLAGS =
  platf::dxgi::WGC_IPC_FLAG_DRAIN_TO_LATEST |
  platf::dxgi::WGC_IPC_FLAG_ALLOW_BUFFER_DECREASE;
static platf::dxgi::config_data_t g_config = {0, 0, 0, L"", {0, 0}, 10000, 60, 1, 2, DEFAULT_WGC_IPC_FLAGS, 120, 0};
static std::mutex g_config_mutex;
static std::condition_variable g_config_cv;
static std::atomic<int32_t> g_activity_admission_fps {120};
static std::atomic<uint32_t> g_activity_admission_generation {0};

/**
 * @brief Debug-level min/max/avg timer for the capture helper.
 *
 * logging::min_max_avg_periodic_logger reads config::sunshine, which the helper does not
 * link, so the helper keeps its own. Create it after the config from the main process
 * arrived: it collects nothing unless that config asks for debug logging. Not thread-safe;
 * callers serialize collect().
 */
class helper_debug_timer_t {
public:
  explicit helper_debug_timer_t(const char *name):
      _name(name),
      _enabled(g_config.log_level <= 1) {  // 1 is the debug severity (see src/logging.cpp)
  }

  void collect(const std::chrono::steady_clock::duration elapsed) {
    if (!_enabled) {
      return;
    }
    const double elapsed_ms = std::chrono::duration<double, std::milli>(elapsed).count();
    _tracker.collect_and_callback_on_interval(elapsed_ms, [this](const double min_ms, const double max_ms, const double avg_ms) {
      BOOST_LOG(debug) << _name << " (min/max/avg): " << std::format("{:.2f}ms/{:.2f}ms/{:.2f}ms", min_ms, max_ms, avg_ms);
    }, std::chrono::seconds(20));
  }

private:
  const char *_name;
  bool _enabled;
  stat_trackers::min_max_avg_tracker<double> _tracker;
};

/**
 * @brief Latest app window set from the main process (WGC_IPC_FLAG_WINDOW_SET sessions only).
 * Constructed on first use and never destroyed, so static initialization and exit order
 * cannot matter.
 */
struct window_set_state_t {
  std::mutex mutex;
  uint64_t target_hwnd = 0;  ///< Target the set belongs to.
  std::vector<uint32_t> pids;  ///< Sorted ascending.
  uint64_t generation = 0;  ///< Bumped with every update, under `mutex`.
};

window_set_state_t &window_set_state() {
  static auto *state = new window_set_state_t {};
  return *state;
}

/**
 * @brief Bumped whenever a new window set arrives.
 */
static std::atomic<uint64_t> g_window_set_generation {0};

/**
 * @brief Refresh `pids` with the processes for `target_hwnd` only when the set changed since
 * `generation`; reuses the vector's storage, so the per-frame path does not allocate.
 */
void refresh_window_set_pids(const uint64_t target_hwnd, std::vector<uint32_t> &pids, uint64_t &generation) {
  auto &state = window_set_state();
  std::lock_guard lock(state.mutex);
  if (state.generation == generation) {
    return;
  }
  if (state.target_hwnd == target_hwnd) {
    pids.assign(state.pids.begin(), state.pids.end());
  } else {
    pids.clear();
  }
  generation = state.generation;
}

/**
 * @brief Auto-reset event that wakes the helper's main loop for the layer manager (a new
 * window set, a closed layer item). Created on first use and never closed.
 */
HANDLE layer_wake_event() {
  static const HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  return event;
}

/**
 * @brief Flag indicating whether configuration data has been received from main process.
 */
static bool g_config_received = false;

/**
 * @brief Global communication pipe for sending session closed notifications.
 */
static std::weak_ptr<AsyncNamedPipe> g_communication_pipe_weak;

/**
 * @brief Global Windows event hook for desktop switch detection.
 */
static safe_winevent_hook g_desktop_switch_hook = nullptr;

/**
 * @brief Flag indicating if a secure desktop has been detected.
 */
static bool g_secure_desktop_detected = false;

/**
 * @brief Set when the GraphicsCaptureItem reports it was closed (display mode change,
 * DWM restart, monitor removal). FrameArrived never fires again after this, so the
 * helper must exit and let the main process reinitialize capture.
 */
static std::atomic<bool> g_capture_item_closed {false};

/**
 * @brief Pending verification deadline for desktop switches that were not immediately secure.
 */
static std::optional<std::chrono::steady_clock::time_point> g_pending_secure_desktop_check_deadline;

/**
 * @brief Grace window for confirming secure desktop after a desktop switch event.
 *
 * EVENT_SYSTEM_DESKTOPSWITCH can fire before OpenInputDesktop reflects the final target desktop.
 * Keep a short follow-up probe window so we still notify the main process when the switch settles
 * onto secure desktop, without forcing a reinit for ordinary non-secure desktop churn.
 */
constexpr auto SECURE_DESKTOP_CONFIRMATION_WINDOW = std::chrono::seconds(1);

void notify_main_process_about_secure_desktop() {
  if (g_secure_desktop_detected) {
    return;
  }

  g_secure_desktop_detected = true;
  g_pending_secure_desktop_check_deadline.reset();
  BOOST_LOG(info) << "Secure desktop detected - sending notification to main process";

  if (auto pipe = g_communication_pipe_weak.lock(); pipe && pipe->is_connected()) {
    uint8_t msg = SECURE_DESKTOP_MSG;
    pipe->send(std::span<const uint8_t>(&msg, 1));
    BOOST_LOG(info) << "Queued desktop switch reinit notification to main process";
  } else {
    BOOST_LOG(warning) << "Desktop switch detected, but the main process pipe is not connected";
  }
}

void poll_pending_secure_desktop_transition() {
  if (!g_pending_secure_desktop_check_deadline) {
    return;
  }

  if (platf::dxgi::is_secure_desktop_active()) {
    BOOST_LOG(info) << "Secure desktop became active shortly after desktop switch";
    notify_main_process_about_secure_desktop();
    return;
  }

  if (std::chrono::steady_clock::now() >= *g_pending_secure_desktop_check_deadline) {
    BOOST_LOG(debug) << "Desktop switch settled without entering secure desktop; ignoring reinit";
    g_pending_secure_desktop_check_deadline.reset();
  }
}

/**
 * @brief System initialization class to handle DPI, threading, and MMCSS setup.
 *
 * This class manages critical system-level initialization for optimal capture performance:
 * - Sets DPI awareness to handle high-DPI displays correctly
 * - Elevates thread priority for better capture timing
 * - Configures MMCSS (Multimedia Class Scheduler Service) for audio/video workloads
 * - Initializes WinRT apartment for Windows Graphics Capture APIs
 */
class SystemInitializer {
private:
  safe_mmcss_handle _mmcss_handle = nullptr;  ///< Handle for MMCSS thread characteristics
  bool _dpi_awareness_set = false;  ///< Flag indicating if DPI awareness was successfully set
  bool _thread_priority_set = false;  ///< Flag indicating if thread priority was elevated
  bool _mmcss_characteristics_set = false;  ///< Flag indicating if MMCSS characteristics were set
  bool _gpu_priority_set = false;  ///< Flag indicating if GPU scheduling priority was set

public:
  /**
   * @brief Initializes DPI awareness for the process.
   *
   * Attempts to set per-monitor DPI awareness to handle high-DPI displays correctly.
   * First tries the newer SetProcessDpiAwarenessContext API, then falls back to
   * SetProcessDpiAwareness if the newer API is not available.
   *
   * @return true if DPI awareness was successfully set, false otherwise.
   */
  bool initialize_dpi_awareness() {
    // Try newer API first
    if (HMODULE user32_module = GetModuleHandleA("user32.dll")) {
      using set_process_dpi_awareness_context_fn = BOOL(WINAPI *)(DPI_AWARENESS_CONTEXT);
      auto set_process_dpi_awareness_context =
        reinterpret_cast<set_process_dpi_awareness_context_fn>(
          GetProcAddress(user32_module, "SetProcessDpiAwarenessContext")
        );

      if (set_process_dpi_awareness_context &&
          set_process_dpi_awareness_context(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)) {
        _dpi_awareness_set = true;
        return true;
      }
    }

    // Fallback to older API (Win 8.1+)
    if (SUCCEEDED(SetProcessDpiAwareness(PROCESS_PER_MONITOR_DPI_AWARE))) {
      _dpi_awareness_set = true;
      return true;
    }

    BOOST_LOG(warning) << "Failed to set DPI awareness, display scaling issues may occur";
    return false;
  }

  /**
   * @brief Elevates the current thread priority for better capture performance.
   *
   * Sets the thread priority to THREAD_PRIORITY_HIGHEST to reduce latency
   * and improve capture frame timing consistency.
   *
   * @return true if thread priority was successfully elevated, false otherwise.
   */
  bool initialize_thread_priority() {
    if (!SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST)) {
      BOOST_LOG(error) << "Failed to set thread priority: " << GetLastError();
      return false;
    }
    _thread_priority_set = true;
    return true;
  }

  /**
   * @brief Configures MMCSS (Multimedia Class Scheduler Service) characteristics.
   *
   * Registers the thread with MMCSS for multimedia workload scheduling.
   * First attempts "Pro Audio" task profile, then falls back to "Games" profile.
   * This helps ensure consistent timing for capture operations.
   * Additionally sets MMCSS thread relative priority to maximum for best performance.
   *
   * @return true if MMCSS characteristics were successfully set, false otherwise.
   */
  bool initialize_mmcss_characteristics() {
    DWORD task_idx = 0;
    HANDLE raw_handle = AvSetMmThreadCharacteristicsW(L"Pro Audio", &task_idx);
    if (!raw_handle) {
      raw_handle = AvSetMmThreadCharacteristicsW(L"Games", &task_idx);
      if (!raw_handle) {
        BOOST_LOG(error) << "Failed to set MMCSS characteristics: " << GetLastError();
        return false;
      }
    }

    // Set MMCSS thread relative priority to maximum (AVRT_PRIORITY_HIGH = 2)
    if (!AvSetMmThreadPriority(raw_handle, AVRT_PRIORITY_HIGH)) {
      BOOST_LOG(warning) << "Failed to set MMCSS thread priority: " << GetLastError();
      // Don't fail completely as the basic MMCSS registration still works
    }

    _mmcss_handle.reset(raw_handle);
    _mmcss_characteristics_set = true;
    return true;
  }

  /**
   * @brief Enables SeIncreaseBasePriorityPrivilege on the process token when the token holds it.
   *
   * A token can hold the privilege in the disabled state. Enabling it lets the REALTIME
   * GPU scheduling class succeed. Failure is expected for normal user tokens and only logged.
   *
   * @return true if the privilege is enabled, false otherwise.
   */
  static bool enable_increase_base_priority_privilege() {
    winrt::handle token;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, token.put())) {
      BOOST_LOG(debug) << "OpenProcessToken failed while enabling SeIncreaseBasePriorityPrivilege: " << GetLastError();
      return false;
    }

    LUID luid {};
    if (!LookupPrivilegeValueW(nullptr, L"SeIncreaseBasePriorityPrivilege", &luid)) {
      BOOST_LOG(debug) << "LookupPrivilegeValueW failed for SeIncreaseBasePriorityPrivilege: " << GetLastError();
      return false;
    }

    TOKEN_PRIVILEGES privileges {};
    privileges.PrivilegeCount = 1;
    privileges.Privileges[0].Luid = luid;
    privileges.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    if (!AdjustTokenPrivileges(token.get(), FALSE, &privileges, sizeof(privileges), nullptr, nullptr)) {
      BOOST_LOG(debug) << "AdjustTokenPrivileges failed for SeIncreaseBasePriorityPrivilege: " << GetLastError();
      return false;
    }
    // AdjustTokenPrivileges succeeds without enabling anything when the token lacks the privilege.
    if (GetLastError() == ERROR_NOT_ALL_ASSIGNED) {
      BOOST_LOG(debug) << "The process token does not hold SeIncreaseBasePriorityPrivilege";
      return false;
    }
    return true;
  }

  /**
   * @brief Sets GPU scheduling priority for optimal capture performance under high GPU load.
   *
   * Tries the REALTIME class first, after enabling SeIncreaseBasePriorityPrivilege when the
   * token holds it, then falls back to HIGH. This step is best effort: the helper keeps
   * running at the default GPU priority when both classes are refused.
   *
   * @return true if GPU priority was successfully set, false otherwise.
   */
  bool initialize_gpu_scheduling_priority() {
    HMODULE gdi32 = GetModuleHandleA("gdi32.dll");
    if (!gdi32) {
      BOOST_LOG(warning) << "Failed to get gdi32.dll handle for GPU priority adjustment";
      return false;
    }

    auto d3dkmt_set_process_priority = reinterpret_cast<PD3DKMTSetProcessSchedulingPriorityClass>(GetProcAddress(gdi32, "D3DKMTSetProcessSchedulingPriorityClass"));
    if (!d3dkmt_set_process_priority) {
      BOOST_LOG(warning) << "D3DKMTSetProcessSchedulingPriorityClass not available, GPU priority not set";
      return false;
    }

    const bool privilege_enabled = enable_increase_base_priority_privilege();

    HRESULT hr = d3dkmt_set_process_priority(GetCurrentProcess(), static_cast<LONG>(D3DKMT_SchedulingPriorityClass::REALTIME));
    if (SUCCEEDED(hr)) {
      BOOST_LOG(info) << "GPU scheduling priority set to REALTIME for optimal capture performance";
      _gpu_priority_set = true;
      return true;
    }
    BOOST_LOG(warning) << "Failed to set GPU scheduling priority to REALTIME: " << hr
                       << (privilege_enabled ? "" : " (SeIncreaseBasePriorityPrivilege is not available to this process)")
                       << "; trying HIGH";

    hr = d3dkmt_set_process_priority(GetCurrentProcess(), static_cast<LONG>(D3DKMT_SchedulingPriorityClass::HIGH));
    if (SUCCEEDED(hr)) {
      BOOST_LOG(info) << "GPU scheduling priority set to HIGH";
      _gpu_priority_set = true;
      return true;
    }
    BOOST_LOG(warning) << "Failed to set GPU scheduling priority to HIGH: " << hr
                       << "; capture continues at the default GPU priority";
    return false;
  }

  /**
   * @brief Initializes WinRT apartment for Windows Graphics Capture APIs.
   *
   * Sets up the WinRT apartment as multi-threaded to support Windows Graphics
   * Capture operations from background threads.
   *
   * @return true always (WinRT initialization typically succeeds).
   */
  bool initialize_winrt_apartment() const {
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    return true;
  }

  /**
   * @brief Initializes all system components for optimal capture performance.
   *
   * Calls all initialization methods in sequence:
   * - DPI awareness configuration
   * - Thread priority elevation
   * - GPU scheduling priority setup
   * - MMCSS characteristics setup
   * - WinRT apartment initialization
   *
   * Which steps are required is decided by
   * platf::dxgi::wgc_policy::helper_init_step_required(). DPI awareness and the GPU
   * scheduling priority are best effort.
   * @return true if all required initialization steps succeeded, false if any required step failed.
   */
  bool initialize_all() {
    using platf::dxgi::wgc_policy::helper_init_step_e;
    bool success = true;
    const auto record = [&success](const helper_init_step_e step, const bool step_succeeded) {
      if (!step_succeeded && platf::dxgi::wgc_policy::helper_init_step_required(step)) {
        success = false;
      }
    };
    record(helper_init_step_e::dpi_awareness, initialize_dpi_awareness());
    record(helper_init_step_e::thread_priority, initialize_thread_priority());
    record(helper_init_step_e::gpu_scheduling_priority, initialize_gpu_scheduling_priority());
    record(helper_init_step_e::mmcss_characteristics, initialize_mmcss_characteristics());
    record(helper_init_step_e::winrt_apartment, initialize_winrt_apartment());
    return success;
  }

  /**
   * @brief Checks if DPI awareness was successfully set.
   * @return true if DPI awareness is configured, false otherwise.
   */
  bool is_dpi_awareness_set() const {
    return _dpi_awareness_set;
  }

  /**
   * @brief Checks if thread priority was successfully elevated.
   * @return true if thread priority is elevated, false otherwise.
   */
  bool is_thread_priority_set() const {
    return _thread_priority_set;
  }

  /**
   * @brief Checks if MMCSS characteristics were successfully set.
   * @return true if MMCSS characteristics are configured, false otherwise.
   */
  bool is_mmcss_characteristics_set() const {
    return _mmcss_characteristics_set;
  }

  /**
   * @brief Checks if GPU scheduling priority was successfully set.
   * @return true if GPU scheduling priority is configured, false otherwise.
   */
  bool is_gpu_priority_set() const {
    return _gpu_priority_set;
  }

  /**
   * @brief Destructor for SystemInitializer.
   *
   * RAII automatically releases MMCSS resources.
   */
  ~SystemInitializer() noexcept = default;
};

/**
 * @brief D3D11 device management class to handle device creation and WinRT interop.
 *
 * This class manages D3D11 device and context creation, as well as the WinRT
 * interop device required for Windows Graphics Capture. It handles the complex
 * process of bridging between traditional D3D11 APIs and WinRT capture APIs.
 */
class D3D11DeviceManager {
private:
  winrt::com_ptr<ID3D11Device> _device;  ///< D3D11 device for graphics operations
  winrt::com_ptr<ID3D11DeviceContext> _context;  ///< D3D11 device context for rendering
  D3D_FEATURE_LEVEL _feature_level;  ///< D3D feature level supported by the device
  winrt::com_ptr<IDXGIDevice> _dxgi_device;  ///< DXGI device interface for WinRT interop
  winrt::com_ptr<::IDirect3DDevice> _interop_device;  ///< Intermediate interop device
  IDirect3DDevice _winrt_device = nullptr;  ///< WinRT Direct3D device for capture integration

public:
  /**
   * @brief Creates a D3D11 device and context for graphics operations.
   *
   * Creates a hardware-accelerated D3D11 device using the specified adapter.
   * The device is used for texture operations and WinRT interop.
   * Also sets GPU thread priority to 7 for optimal capture performance.
   * Uses the same D3D11_CREATE_DEVICE_FLAGS as the main Sunshine process.
   *
   * @param adapter_luid LUID of the adapter to use, or all zeros for default adapter.
   * @return true if device creation succeeded, false otherwise.
   */
  bool create_device(const LUID &adapter_luid) {
    // Feature levels to try, matching the main process
    D3D_FEATURE_LEVEL featureLevels[] {
      D3D_FEATURE_LEVEL_11_1,
      D3D_FEATURE_LEVEL_11_0,
      D3D_FEATURE_LEVEL_10_1,
      D3D_FEATURE_LEVEL_10_0,
      D3D_FEATURE_LEVEL_9_3,
      D3D_FEATURE_LEVEL_9_2,
      D3D_FEATURE_LEVEL_9_1
    };

    // Find the adapter matching the LUID
    winrt::com_ptr<IDXGIAdapter1> adapter;
    if (adapter_luid.HighPart != 0 || adapter_luid.LowPart != 0) {
      // Non-zero LUID provided, find the matching adapter
      winrt::com_ptr<IDXGIFactory1> factory;
      HRESULT hr = CreateDXGIFactory1(__uuidof(IDXGIFactory1), factory.put_void());
      if (FAILED(hr)) {
        BOOST_LOG(error) << "Failed to create DXGI factory for adapter lookup";
        return false;
      }

      IDXGIAdapter1 *raw_adapter = nullptr;
      for (UINT i = 0; factory->EnumAdapters1(i, &raw_adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
        winrt::com_ptr<IDXGIAdapter1> test_adapter;
        test_adapter.attach(raw_adapter);
        DXGI_ADAPTER_DESC1 desc;
        if (SUCCEEDED(test_adapter->GetDesc1(&desc))) {
          if (desc.AdapterLuid.HighPart == adapter_luid.HighPart &&
              desc.AdapterLuid.LowPart == adapter_luid.LowPart) {
            adapter = test_adapter;
            BOOST_LOG(info) << "Found matching adapter: " << wide_to_utf8(desc.Description);
            break;
          }
        }
      }

      if (!adapter) {
        BOOST_LOG(warning) << "Could not find adapter with LUID "
                           << std::hex << adapter_luid.HighPart << ":" << adapter_luid.LowPart
                           << std::dec << ", using default adapter";
      }
    } else {
      BOOST_LOG(info) << "Using default adapter (no LUID specified)";
    }

    // Create the D3D11 device using the same flags as the main process
    HRESULT hr = D3D11CreateDevice(
      adapter.get(),  // nullptr if no specific adapter found
      adapter ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE,
      nullptr,
      D3D11_CREATE_DEVICE_FLAGS,
      featureLevels,
      ARRAYSIZE(featureLevels),
      D3D11_SDK_VERSION,
      _device.put(),
      &_feature_level,
      _context.put()
    );

    if (FAILED(hr)) {
      BOOST_LOG(error) << "Failed to create D3D11 device: " << std::hex << hr << std::dec;
      return false;
    }

    // Set GPU thread priority to 7 for optimal capture performance under high GPU load
    _dxgi_device = _device.try_as<IDXGIDevice>();
    if (_dxgi_device) {
      hr = _dxgi_device->SetGPUThreadPriority(7);
      if (FAILED(hr)) {
        BOOST_LOG(warning) << "Failed to set GPU thread priority to 7: " << hr
                           << " (may require administrator privileges for optimal performance)";
      } else {
        BOOST_LOG(info) << "GPU thread priority set to 7 for optimal capture performance";
      }
    } else {
      BOOST_LOG(warning) << "Failed to query DXGI device for GPU thread priority setting";
    }

    return true;
  }

  /**
   * @brief Creates WinRT interop device from the D3D11 device.
   *
   * Bridges the D3D11 device to WinRT APIs required for Windows Graphics Capture.
   * This involves:
   * - Querying DXGI device interface from D3D11 device
   * - Creating Direct3D interop device using WinRT factory
   * - Converting to WinRT IDirect3DDevice interface
   *
   * @return true if WinRT interop device creation succeeded, false otherwise.
   */
  bool create_winrt_interop() {
    if (!_device) {
      return false;
    }

    _dxgi_device = _device.try_as<IDXGIDevice>();
    if (!_dxgi_device) {
      BOOST_LOG(error) << "Failed to get DXGI device";
      return false;
    }

    HRESULT hr = CreateDirect3D11DeviceFromDXGIDevice(_dxgi_device.get(), reinterpret_cast<::IInspectable **>(winrt::put_abi(_interop_device)));
    if (FAILED(hr)) {
      BOOST_LOG(error) << "Failed to create interop device";
      return false;
    }

    _winrt_device = _interop_device.try_as<IDirect3DDevice>();
    if (!_winrt_device) {
      BOOST_LOG(error) << "Failed to query IDirect3DDevice from interop device";
      return false;
    }
    return true;
  }

  /**
   * @brief Initializes the D3D11 device and WinRT interop device.
   *
   * Calls create_device() with the specified adapter LUID, then create_winrt_interop() to create the WinRT interop device.
   *
   * @param adapter_luid LUID of the adapter to use, or all zeros for default adapter.
   * @return true if both device and interop device are successfully created, false otherwise.
   */
  bool initialize_all(const LUID &adapter_luid) {
    return create_device(adapter_luid) && create_winrt_interop();
  }

  /**
   * @brief Gets the underlying D3D11 device com_ptr.
   *
   * @return Pointer to the managed ID3D11Device, or empty if not initialized.
   */
  const winrt::com_ptr<ID3D11Device> &get_device() const {
    return _device;
  }

  /**
   * @brief Gets the underlying D3D11 device context com_ptr.
   *
   * @return Pointer to the managed ID3D11DeviceContext, or empty if not initialized.
   */
  const winrt::com_ptr<ID3D11DeviceContext> &get_context() const {
    return _context;
  }

  /**
   * @brief Gets the WinRT IDirect3DDevice for Windows Graphics Capture interop.
   *
   * @return The WinRT IDirect3DDevice, or nullptr if not initialized.
   */
  IDirect3DDevice get_winrt_device() const {
    return _winrt_device;
  }

  /**
   * @brief Destructor for D3D11DeviceManager.
   *
   * RAII automatically releases device and context resources.
   */
  ~D3D11DeviceManager() noexcept = default;
};

/**
 * @brief Monitor and display management class to handle monitor enumeration and selection.
 *
 * This class manages monitor detection, selection, and configuration for capture operations.
 * It handles scenarios with multiple monitors and provides resolution configuration based
 * on monitor capabilities and user preferences.
 */
class DisplayManager {
private:
  HMONITOR _selected_monitor = nullptr;  ///< Handle to the selected monitor for capture
  MONITORINFO _monitor_info = {sizeof(MONITORINFO)};  ///< Information about the selected monitor
  UINT _width = 0;  ///< Final capture width in pixels
  UINT _height = 0;  ///< Final capture height in pixels

public:
  /**
   * @brief Selects a monitor for capture based on the provided configuration.
   *
   * If a display name is specified in the config, attempts to find and select the monitor matching that name.
   * If not found, or if no display name is provided, falls back to selecting the primary monitor.
   *
   * @param config The configuration data containing the desired display name (if any).
   * @return true if a monitor was successfully selected; false if monitor selection failed.
   */
  bool select_monitor(const platf::dxgi::config_data_t &config) {
    if (config.display_name[0] != L'\0') {
      auto find_monitor_by_name = [&](const wchar_t *target_name) -> HMONITOR {
        struct EnumData {
          const wchar_t *target_name;
          HMONITOR found_monitor;
        };

        EnumData enum_data = {target_name, nullptr};

        auto enum_proc = +[](HMONITOR h_mon, HDC /*hdc*/, RECT * /*rc*/, LPARAM l_param) {
          auto *data = static_cast<EnumData *>(reinterpret_cast<void *>(l_param));
          if (MONITORINFOEXW m_info = {sizeof(MONITORINFOEXW)}; GetMonitorInfoW(h_mon, &m_info) && wcsncmp(m_info.szDevice, data->target_name, 32) == 0) {
            data->found_monitor = h_mon;
            return FALSE;  // Stop enumeration
          }
          return TRUE;
        };

        EnumDisplayMonitors(nullptr, nullptr, enum_proc, static_cast<LPARAM>(reinterpret_cast<std::uintptr_t>(&enum_data)));
        return enum_data.found_monitor;
      };

      _selected_monitor = find_monitor_by_name(config.display_name);
      if (!_selected_monitor) {
        // During virtual display topology transitions, monitor enumeration can lag briefly.
        // Wait a short amount of time before falling back so we avoid capture bouncing.
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(1500);
        while (!_selected_monitor && std::chrono::steady_clock::now() < deadline) {
          std::this_thread::sleep_for(std::chrono::milliseconds(100));
          _selected_monitor = find_monitor_by_name(config.display_name);
        }
      }
      if (!_selected_monitor) {
        BOOST_LOG(warning) << "Could not find monitor with name '" << winrt::to_string(config.display_name) << "', falling back to primary.";
      }
    }

    if (!_selected_monitor) {
      _selected_monitor = MonitorFromWindow(GetDesktopWindow(), MONITOR_DEFAULTTOPRIMARY);
      if (!_selected_monitor) {
        BOOST_LOG(error) << "Failed to get primary monitor";
        return false;
      }
    }
    return true;
  }

  /**
   * @brief Retrieves information about the currently selected monitor.
   *
   * This function queries the Windows API to obtain monitor information for the selected monitor,
   * including its dimensions. The width and height are stored as fallback values for later use.
   *
   * @return true if monitor information was successfully retrieved; false otherwise.
   */
  bool get_monitor_info() {
    if (!_selected_monitor) {
      return false;
    }

    if (!GetMonitorInfo(_selected_monitor, &_monitor_info)) {
      BOOST_LOG(error) << "Failed to get monitor info";
      return false;
    }

    return true;
  }

  /**
   * @brief Creates a Windows Graphics Capture item for the selected monitor.
   *
   * Uses the IGraphicsCaptureItemInterop interface to create a GraphicsCaptureItem
   * corresponding to the currently selected monitor. This item is required for
   * initiating Windows Graphics Capture sessions.
   *
   * @param[out] item Reference to a GraphicsCaptureItem that will be set on success.
   * @return true if the GraphicsCaptureItem was successfully created; false otherwise.
   */
  bool create_graphics_capture_item(GraphicsCaptureItem &item) {
    if (!_selected_monitor) {
      return false;
    }

    auto activation_factory = winrt::get_activation_factory<GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
    HRESULT hr = activation_factory->CreateForMonitor(_selected_monitor, winrt::guid_of<GraphicsCaptureItem>(), winrt::put_abi(item));
    if (FAILED(hr)) {
      BOOST_LOG(error) << "Failed to create GraphicsCaptureItem for monitor: " << hr;
      return false;
    }
    return true;
  }

  /**
   * @brief Creates a Windows Graphics Capture item for a single top-level window.
   *
   * @param hwnd The window to capture.
   * @param[out] item Reference to a GraphicsCaptureItem that will be set on success.
   * @return true if the GraphicsCaptureItem was successfully created; false otherwise.
   */
  bool create_window_capture_item(HWND hwnd, GraphicsCaptureItem &item) {
    if (!hwnd || !IsWindow(hwnd)) {
      BOOST_LOG(warning) << "Window-only target is no longer a window";
      return false;
    }

    try {
      auto activation_factory = winrt::get_activation_factory<GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
      HRESULT hr = activation_factory->CreateForWindow(hwnd, winrt::guid_of<GraphicsCaptureItem>(), winrt::put_abi(item));
      if (FAILED(hr)) {
        BOOST_LOG(error) << "Failed to create GraphicsCaptureItem for window: " << hr;
        item = nullptr;
        return false;
      }
    } catch (const winrt::hresult_error &ex) {
      BOOST_LOG(error) << "Failed to create GraphicsCaptureItem for window: " << ex.code();
      item = nullptr;
      return false;
    }
    return static_cast<bool>(item);
  }

  /**
   * @brief Gets the selected monitor rectangle in desktop coordinates (physical pixels).
   * @return The monitor rectangle.
   */
  RECT monitor_rect() const {
    return _monitor_info.rcMonitor;
  }

  /**
   * @brief Calculates the final capture resolution based on config, monitor info, and WGC item size.
   *
   * Chooses resolution in this order:
   *
   * - Uses config width/height if valid.
   *
   * - Otherwise uses monitor logical size.
   *
   * - If WGC item size differs significantly (DPI scaling), uses WGC physical size to avoid cropping/zoom.
   *
   * @param config The configuration data with requested width/height.
   * @param config_received True if config data was received.
   * @param item The GraphicsCaptureItem for the selected monitor.
   */
  void configure_capture_resolution(const GraphicsCaptureItem &item) {
    // Get actual WGC item size to ensure we capture full desktop (fixes zoomed display issue)
    auto item_size = item.Size();
    _height = item_size.Height;
    _width = item_size.Width;
  }

  /**
   * @brief Gets the handle to the selected monitor.
   * @return HMONITOR handle of the selected monitor, or nullptr if none selected.
   */
  HMONITOR get_selected_monitor() const {
    return _selected_monitor;
  }

  /**
   * @brief Gets the configured capture width.
   * @return Capture width in pixels.
   */
  UINT get_width() const {
    return _width;
  }

  /**
   * @brief Gets the configured capture height.
   * @return Capture height in pixels.
   */
  UINT get_height() const {
    return _height;
  }
};

/**
 * @brief Shared resource management class to handle texture, memory mapping, and events.
 *
 * This class manages shared D3D11 resources for inter-process communication with the main
 * Sunshine process. It creates a shared texture with a keyed mutex plus a shared metadata
 * view and frame-ready event for realtime cross-process delivery.
 */
class SharedResourceManager {
private:
  winrt::com_ptr<ID3D11Texture2D> _shared_texture;  ///< Shared D3D11 texture for frame data
  winrt::com_ptr<IDXGIKeyedMutex> _keyed_mutex;  ///< Keyed mutex for synchronization
  winrt::handle _shared_handle;  ///< Shared handle for cross-process sharing
  winrt::handle _frame_ready_event;  ///< Auto-reset event signaled after each published frame
  winrt::handle _frame_metadata_mapping;  ///< Shared memory containing latest frame metadata
  platf::dxgi::frame_metadata_t *_frame_metadata = nullptr;  ///< Mapped frame metadata view
  UINT _width = 0;  ///< Texture width in pixels
  UINT _height = 0;  ///< Texture height in pixels

public:
  /**
   * @brief Default constructor for SharedResourceManager.
   */
  SharedResourceManager() = default;

  /**
   * @brief Deleted copy constructor to prevent resource duplication.
   */
  SharedResourceManager(const SharedResourceManager &) = delete;

  /**
   * @brief Deleted copy assignment operator to prevent resource duplication.
   */
  SharedResourceManager &operator=(const SharedResourceManager &) = delete;

  /**
   * @brief Deleted move constructor to prevent resource transfer issues.
   */
  SharedResourceManager(SharedResourceManager &&) = delete;

  /**
   * @brief Deleted move assignment operator to prevent resource transfer issues.
   */
  SharedResourceManager &operator=(SharedResourceManager &&) = delete;

  /**
   * @brief Creates a shared D3D11 texture with keyed mutex for inter-process sharing.
   *
   * @param device Pointer to the D3D11 device used for texture creation.
   * @param texture_width Width of the texture in pixels.
   * @param texture_height Height of the texture in pixels.
   * @param format DXGI format for the texture.
   * @return true if the texture was successfully created; false otherwise.
   */
  bool create_shared_texture(const winrt::com_ptr<ID3D11Device> &device, UINT texture_width, UINT texture_height, DXGI_FORMAT format) {
    _width = texture_width;
    _height = texture_height;

    D3D11_TEXTURE2D_DESC tex_desc = {};
    tex_desc.Width = _width;
    tex_desc.Height = _height;
    tex_desc.MipLevels = 1;
    tex_desc.ArraySize = 1;
    tex_desc.Format = format;
    tex_desc.SampleDesc.Count = 1;
    tex_desc.Usage = D3D11_USAGE_DEFAULT;
    tex_desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    // Use NT shared handles exclusively
    tex_desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;

    HRESULT hr = device->CreateTexture2D(&tex_desc, nullptr, _shared_texture.put());
    if (FAILED(hr)) {
      BOOST_LOG(error) << "Failed to create NT shared texture: " << hr;
      return false;
    }
    return true;
  }

  /**
   * @brief Acquires a keyed mutex interface from the shared texture for synchronization.
   * @return true if the keyed mutex was successfully acquired; false otherwise.
   */
  bool create_keyed_mutex() {
    if (!_shared_texture) {
      return false;
    }

    _keyed_mutex = _shared_texture.try_as<IDXGIKeyedMutex>();
    if (!_keyed_mutex) {
      BOOST_LOG(error) << "Failed to get keyed mutex";
      return false;
    }
    return true;
  }

  /**
   * @brief Creates a shared handle for the texture resource.
   *
   * @return true if the handle was successfully created; false otherwise.
   */
  bool create_shared_handle() {
    if (!_shared_texture) {
      BOOST_LOG(error) << "Cannot create shared handle - no shared texture available";
      return false;
    }

    winrt::com_ptr<IDXGIResource1> dxgi_resource1 = _shared_texture.try_as<IDXGIResource1>();
    if (!dxgi_resource1) {
      BOOST_LOG(error) << "Failed to query DXGI resource1 interface";
      return false;
    }

    // Create the shared handle
    HRESULT hr = dxgi_resource1->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr, _shared_handle.put());
    if (FAILED(hr)) {
      BOOST_LOG(error) << "Failed to create shared handle: " << hr;
      return false;
    }
    return true;
  }

  bool create_frame_signal() {
    _frame_ready_event = winrt::handle(CreateEventW(nullptr, FALSE, FALSE, nullptr));
    if (!_frame_ready_event) {
      BOOST_LOG(error) << "Failed to create frame-ready event: " << GetLastError();
      return false;
    }

    _frame_metadata_mapping = winrt::handle(CreateFileMappingW(
      INVALID_HANDLE_VALUE,
      nullptr,
      PAGE_READWRITE,
      0,
      static_cast<DWORD>(sizeof(platf::dxgi::frame_metadata_t)),
      nullptr
    ));
    if (!_frame_metadata_mapping) {
      BOOST_LOG(error) << "Failed to create frame metadata mapping: " << GetLastError();
      return false;
    }

    _frame_metadata = static_cast<platf::dxgi::frame_metadata_t *>(MapViewOfFile(
      _frame_metadata_mapping.get(),
      FILE_MAP_READ | FILE_MAP_WRITE,
      0,
      0,
      sizeof(platf::dxgi::frame_metadata_t)
    ));
    if (!_frame_metadata) {
      BOOST_LOG(error) << "Failed to map frame metadata view: " << GetLastError();
      return false;
    }

    _frame_metadata->sequence = 0;
    _frame_metadata->frame_id = 0;
    _frame_metadata->frame_qpc = 0;
    return true;
  }

  /**
   * @brief Initializes all shared resource components: texture, keyed mutex, and handle.
   *
   * @param device Pointer to the D3D11 device used for resource creation.
   * @param texture_width Width of the texture in pixels.
   * @param texture_height Height of the texture in pixels.
   * @param format DXGI format for the texture.
   * @return true if all resources were successfully initialized; false otherwise.
   */
  bool initialize_all(const winrt::com_ptr<ID3D11Device> &device, UINT texture_width, UINT texture_height, DXGI_FORMAT format) {
    return create_shared_texture(device, texture_width, texture_height, format) &&
           create_keyed_mutex() &&
           create_shared_handle() &&
           create_frame_signal();
  }

  /**
   * @brief Gets the shared handle data for inter-process sharing.
   * @return shared_handle_data_t struct containing the shared handle and texture dimensions.
   */
  platf::dxgi::shared_handle_data_t get_shared_handle_data() const {
    platf::dxgi::shared_handle_data_t data = {};
    data.texture_handle = const_cast<HANDLE>(_shared_handle.get());
    data.frame_event_handle = const_cast<HANDLE>(_frame_ready_event.get());
    data.frame_metadata_handle = const_cast<HANDLE>(_frame_metadata_mapping.get());
    data.width = _width;
    data.height = _height;
    return data;
  }

  void publish_frame_metadata(uint64_t frame_qpc) {
    if (!_frame_metadata) {
      return;
    }

    InterlockedIncrement64(&_frame_metadata->sequence);
    InterlockedExchange64(&_frame_metadata->frame_qpc, static_cast<LONG64>(frame_qpc));
    InterlockedIncrement64(&_frame_metadata->frame_id);
    InterlockedIncrement64(&_frame_metadata->sequence);
  }

  void signal_frame_ready() {
    if (!_frame_ready_event) {
      return;
    }

    SetEvent(_frame_ready_event.get());
  }

  /**
   * @brief Gets the underlying shared D3D11 texture pointer.
   * @return Pointer to the managed ID3D11Texture2D, or nullptr if not initialized.
   */
  const winrt::com_ptr<ID3D11Texture2D> &get_shared_texture() const {
    return _shared_texture;
  }

  /**
   * @brief Gets the keyed mutex interface com_ptr for the shared texture.
   * @return const winrt::com_ptr<IDXGIKeyedMutex>& (may be empty if not initialized).
   */
  const winrt::com_ptr<IDXGIKeyedMutex> &get_keyed_mutex() const {
    return _keyed_mutex;
  }

  /**
   * @brief Destructor for SharedResourceManager.
   * Uses RAII to automatically dispose resources
   */
  ~SharedResourceManager() {
    if (_frame_metadata) {
      UnmapViewOfFile(_frame_metadata);
      _frame_metadata = nullptr;
    }
  }
};

/**
 * @brief WGC capture management class to handle frame pool, capture session, and frame processing.
 *
 * This class manages the core Windows Graphics Capture functionality including:
 * - Frame pool creation and management with dynamic buffer sizing
 * - Capture session lifecycle management
 * - Frame arrival event handling and processing
 * - Frame rate optimization and adaptive buffering
 * - Integration with shared texture resources and event signaling for inter-process communication
 */
struct WgcCaptureDependencies {
  // Required devices/resources
  IDirect3DDevice winrt_device;  // WinRT Direct3D device (value-type COM handle)
  GraphicsCaptureItem graphics_item;  // Target capture item
  SharedResourceManager &resource_manager;  // Shared inter-process texture/mutex manager
  winrt::com_ptr<ID3D11DeviceContext> d3d_context;  // D3D11 context for copies
};

/**
 * @brief Window-only capture settings. The shared texture keeps the display size and the
 * window's client area is copied to its true position on the display; every other pixel is black.
 */
struct WgcWindowCaptureTarget {
  bool enabled = false;  // Window-only session; never copy a whole display frame
  HWND hwnd = nullptr;  // Captured window, or nullptr when only black is published
  RECT monitor_rect {};  // Capture display rectangle in desktop coordinates
};

/**
 * @brief Apply the session options every WGC session of this helper uses: no capture border
 * where the OS allows it, and the configured minimum update interval.
 * @param verbose Log at info level (the main session); secondary window sessions log
 * failures at debug level only.
 */
void configure_capture_session(const GraphicsCaptureSession &session, const bool verbose) {
  auto session3 = session.try_as<winrt::Windows::Graphics::Capture::IGraphicsCaptureSession3>();
  if (!session3) {
    if (verbose) {
      BOOST_LOG(warning) << "IGraphicsCaptureSession3 not available; skipping IsBorderRequired(false)";
    }
  } else {
    try {
      session3.IsBorderRequired(false);
    } catch (const winrt::hresult_error &ex) {
      if (verbose) {
        BOOST_LOG(warning) << "IsBorderRequired(false) failed (continuing without it): " << ex.code() << " - " << winrt::to_string(ex.message());
      } else {
        BOOST_LOG(debug) << "IsBorderRequired(false) failed for a secondary window session: " << ex.code();
      }
    } catch (...) {
      if (verbose) {
        BOOST_LOG(warning) << "IsBorderRequired(false) threw an unknown exception (continuing without it)";
      }
    }
  }

  if (winrt::Windows::Foundation::Metadata::ApiInformation::IsPropertyPresent(L"Windows.Graphics.Capture.GraphicsCaptureSession", L"MinUpdateInterval")) {
    if (g_config.min_update_interval_100ns > 0) {
      session.MinUpdateInterval(winrt::Windows::Foundation::TimeSpan {g_config.min_update_interval_100ns});
      if (verbose) {
        BOOST_LOG(info) << "WGC MinUpdateInterval set to " << g_config.min_update_interval_100ns << " ticks";
      }
    } else if (verbose) {
      BOOST_LOG(info) << "WGC MinUpdateInterval left at system default";
    }
  }
}

int64_t qpc_now() {
  LARGE_INTEGER qpc {};
  QueryPerformanceCounter(&qpc);
  return qpc.QuadPart;
}

int64_t qpc_frequency() {
  static const int64_t frequency = []() {
    LARGE_INTEGER value {};
    QueryPerformanceFrequency(&value);
    return value.QuadPart > 0 ? value.QuadPart : int64_t {10000000};
  }();
  return frequency;
}

int64_t qpc_ticks(const std::chrono::microseconds duration) {
  return duration.count() * qpc_frequency() / 1000000;
}

/**
 * @brief Delay after which a reused secondary window is drawn from its cached frame when it
 * shows again without delivering a new one.
 */
constexpr auto SECONDARY_REUSE_DELAY = std::chrono::milliseconds(100);

/**
 * @brief One secondary window of the app (menu, dropdown, tooltip, dialog) captured by its own
 * WGC session and drawn over the target. Shared by the layer manager (main thread), the
 * layer's FrameArrived handler and the compositor.
 */
struct SecondaryLayer {
  HWND hwnd = nullptr;
  DWORD window_pid = 0;  ///< Process that owns the HWND, read when the layer was created.
  DWORD content_pid = 0;  ///< App process of the window; equals `window_pid`, UWP frame windows are never layers.
  int64_t created_qpc = 0;
  std::atomic<int64_t> shown_qpc {0};  ///< Latest show of the window.
  std::atomic<int64_t> frame_qpc {0};  ///< Arrival of the cached frame; 0 before the first one.
  std::atomic<bool> first_frame_logged {false};
  std::atomic<bool> closed {false};  ///< The capture item closed or the layer was dropped; never drawn again.
  std::atomic<bool> published {false};  ///< The window is currently published for drawing.

  // Guarded by the capture manager's D3D context mutex.
  winrt::com_ptr<ID3D11Texture2D> cached;  ///< Last frame of the window.
  UINT cached_width = 0;  ///< Size of `cached`.
  UINT cached_height = 0;
  UINT content_width = 0;  ///< Valid part of `cached`.
  UINT content_height = 0;

  // Used by the layer's FrameArrived handler.
  std::mutex pool_mutex;
  SizeInt32 pool_size {0, 0};
  int32_t pool_buffers = 1;
  uint32_t empty_frames = 0;
};

/**
 * @brief A secondary layer as published to the compositor, bottom-up in z-order.
 */
struct PublishedLayer {
  std::shared_ptr<SecondaryLayer> layer;
  bool below_target = false;
};

class WgcCaptureManager {
private:
  enum class scratch_state_e {
    free,
    reserved,
    pending,
    delivering
  };

  struct scratch_texture_t {
    winrt::com_ptr<ID3D11Texture2D> texture;
    winrt::com_ptr<ID3D11RenderTargetView> rtv;  ///< Created on first clear in window-only mode
    scratch_state_e state = scratch_state_e::free;
  };

  struct delivery_frame_t {
    winrt::com_ptr<ID3D11Texture2D> texture;
    size_t scratch_index = 0;
    uint64_t frame_qpc = 0;
  };

  static constexpr auto WGC_DIAGNOSTICS_LOG_INTERVAL = std::chrono::seconds(15);
  static constexpr auto WGC_DRAIN_LOG_INTERVAL = std::chrono::seconds(5);

  std::atomic<bool> _shutting_down {false};
  Direct3D11CaptureFramePool _frame_pool = nullptr;  ///< WinRT frame pool for capture operations
  GraphicsCaptureSession _capture_session = nullptr;  ///< WinRT capture session for monitor/window capture
  winrt::event_token _frame_arrived_token {};  ///< Event token for frame arrival notifications
  std::optional<WgcCaptureDependencies> _deps;  ///< Dependencies for frame processing

  uint32_t _initial_buffer_size = 1;  ///< Minimum steady-state frame buffer size for this stream
  uint32_t _current_buffer_size = 1;  ///< Current frame buffer size for dynamic adjustment
  uint32_t _max_buffer_size = 4;  ///< Maximum allowed buffer size for adaptive growth
  static constexpr uint32_t ABSOLUTE_MAX_BUFFER_SIZE = 4;  ///< Hard cap to bound latency/VRAM

  std::deque<std::chrono::steady_clock::time_point> _drop_timestamps;  ///< Timestamps of recent frame drops for analysis
  std::atomic<int> _outstanding_frames {0};  ///< Number of frames currently being processed
  std::atomic<int> _peak_outstanding {0};  ///< Peak number of outstanding frames (for monitoring)
  std::atomic<int64_t> _last_frame_pool_pressure_us {0};  ///< Last source drop/drain pressure signal
  std::atomic<int64_t> _last_drain_log_us {0};  ///< Last rate-limited drain log timestamp
  std::atomic<int64_t> _last_diagnostics_log_us {0};  ///< Last aggregate diagnostics log timestamp
  std::chrono::steady_clock::time_point _last_quiet_start = std::chrono::steady_clock::now();  ///< Last time frame processing became quiet
  std::chrono::steady_clock::time_point _last_buffer_check = std::chrono::steady_clock::now();  ///< Last time buffer size was checked
  std::atomic<uint64_t> _captured_frames {0};
  std::atomic<uint64_t> _frame_pool_empty_drops {0};
  std::atomic<uint64_t> _drained_pool_frames {0};
  std::atomic<uint64_t> _slow_context_waits {0};
  std::atomic<uint64_t> _slow_mutex_waits {0};
  std::atomic<uint64_t> _slow_shared_mutex_holds {0};
  std::atomic<uint64_t> _slow_copy_submissions {0};
  std::atomic<uint64_t> _published_frames {0};
  std::atomic<uint64_t> _activity_rate_limited_frames {0};
  uint64_t _last_diagnostics_captured_frames = 0;
  uint64_t _last_diagnostics_published_frames = 0;
  uint64_t _last_diagnostics_empty_drops = 0;
  uint64_t _last_diagnostics_drained_frames = 0;
  uint64_t _last_diagnostics_replaced_frames = 0;
  uint64_t _last_diagnostics_scratch_dropped = 0;
  uint64_t _last_diagnostics_slow_context = 0;
  uint64_t _last_diagnostics_slow_mutex = 0;
  uint64_t _last_diagnostics_slow_hold = 0;
  uint64_t _last_diagnostics_slow_copy = 0;
  uint64_t _last_diagnostics_activity_rate_limited = 0;
  uint32_t _activity_admission_generation = 0;
  std::chrono::steady_clock::time_point _last_activity_admitted {};
  std::mutex _delivery_mutex;
  std::condition_variable _delivery_cv;
  std::jthread _delivery_thread;
  std::optional<delivery_frame_t> _pending_delivery_frame;
  bool _delivery_stop = false;
  std::mutex _d3d_context_mutex;
  // Debug-level timers of the copy path; collected only with _d3d_context_mutex held.
  helper_debug_timer_t _keyed_mutex_wait_timer {"WGC helper: shared texture keyed mutex wait"};
  helper_debug_timer_t _publish_copy_timer {"WGC helper: copy and publish to shared texture"};
  helper_debug_timer_t _scratch_copy_timer {"WGC helper: copy or composite into scratch texture"};
  std::array<scratch_texture_t, 3> _scratch_textures;
  std::atomic<uint64_t> _delivery_backpressure_waits {0};
  std::atomic<uint64_t> _delivery_replaced_frames {0};
  std::atomic<uint64_t> _scratch_dropped_frames {0};
  DXGI_FORMAT _capture_format = DXGI_FORMAT_UNKNOWN;  ///< DXGI format for captured frames
  UINT _height = 0;  ///< Capture height in pixels
  UINT _width = 0;  ///< Capture width in pixels
  bool _window_mode = false;  ///< Composite one window at its true position instead of copying the display
  HWND _target_hwnd = nullptr;  ///< Captured window in window mode
  RECT _monitor_rect {};  ///< Capture display rectangle in desktop coordinates
  UINT _pool_width = 0;  ///< Frame pool width (display width, or window size in window mode)
  UINT _pool_height = 0;  ///< Frame pool height (display height, or window size in window mode)

  // Secondary windows (WGC_IPC_FLAG_WINDOW_SET). With no layer published, every target frame
  // takes the window-only path above unchanged.
  bool _layer_mode = false;  ///< Set once before capture starts.
  std::mutex _layers_mutex;
  std::vector<PublishedLayer> _published_layers;  ///< Bottom-up; guarded by `_layers_mutex`.
  std::atomic<bool> _layers_published {false};  ///< `_published_layers` is not empty.
  std::atomic<uint64_t> _layers_generation {1};  ///< Bumped with every publication, under `_layers_mutex`.
  std::atomic<bool> _recomposite_pending {false};  ///< A layer changed without a new target frame.
  std::atomic<bool> _last_frame_had_layers {false};  ///< The latest queued frame drew secondary layers.
  std::atomic<int> _layer_callbacks {0};  ///< Layer FrameArrived handlers in flight.
  std::atomic<uint64_t> _target_recomposite_requests {0};  ///< Recomposites requested from target frames (logged at debug).
  // Recomposites run on their own thread, so neither the main thread nor a FrameArrived
  // handler ever waits for the context lock behind the shared keyed mutex for them.
  std::mutex _recomposite_mutex;
  std::condition_variable _recomposite_cv;
  bool _recomposite_stop = false;  ///< Guarded by `_recomposite_mutex`.
  std::thread _recomposite_thread;
  // Guarded by `_d3d_context_mutex`.
  scratch_texture_t _base;  ///< The target alone on black, as the window-only path composites it.
  bool _base_valid = false;
  std::optional<size_t> _last_target_only_scratch;  ///< Scratch holding the latest frame composited without layers.
  platf::dxgi::window_policy::window_blit_t _last_target_blit {true, false, 0, 0, 0, 0, 0, 0};
  uint64_t _last_layered_qpc = 0;  ///< Latest frame time queued in layer mode; queued times never go back.

  struct layer_draw_t {
    std::shared_ptr<SecondaryLayer> layer;
    platf::dxgi::window_policy::rect_i bounds;
    bool below_target;
  };

  /**
   * @brief Per-caller buffers for collect_drawable_layers(), reused so that compositing a
   * frame does not allocate once they reached their size.
   */
  struct collect_buffers_t {
    std::vector<PublishedLayer> layers;
    uint64_t layers_generation = 0;  ///< Publication `layers` was copied from; 0 for none.
    std::vector<uint32_t> pids;
    uint64_t pids_generation = UINT64_MAX;  ///< Window set `pids` was copied from.
    std::vector<layer_draw_t> draws;
  };

  std::mutex _target_collect_mutex;  ///< Target FrameArrived handlers can overlap.
  collect_buffers_t _target_collect;  ///< Guarded by `_target_collect_mutex`.
  collect_buffers_t _recomposite_collect;  ///< Recomposite thread only.

public:
  /**
   * @brief Constructor for WgcCaptureManager.
   *
   * Initializes the capture manager for the given dimensions and capture format and
   * stores the dependencies bundle used during capture operations.
   *
   * @param capture_format DXGI format for captured frames.
   * @param width Capture width in pixels.
   * @param height Capture height in pixels.
   * @param deps Bundle of dependencies: winrt IDirect3DDevice, GraphicsCaptureItem,
   *             reference to SharedResourceManager and D3D11 context com_ptr.
   * @param window_target Window-only capture settings; disabled for display capture.
   */
  WgcCaptureManager(DXGI_FORMAT capture_format, UINT width, UINT height, WgcCaptureDependencies deps, WgcWindowCaptureTarget window_target = {}):
      _deps(std::move(deps)),
      _capture_format(capture_format),
      _height(height),
      _width(width),
      _window_mode(window_target.enabled),
      _target_hwnd(window_target.hwnd),
      _monitor_rect(window_target.monitor_rect),
      _pool_width(width),
      _pool_height(height) {
    if (_window_mode && _target_hwnd && _deps->graphics_item) {
      const auto item_size = _deps->graphics_item.Size();
      _pool_width = static_cast<UINT>(std::max(1, item_size.Width));
      _pool_height = static_cast<UINT>(std::max(1, item_size.Height));
    }
    _max_buffer_size = std::clamp<uint32_t>(
      g_config.max_frame_buffer_size ? g_config.max_frame_buffer_size : ABSOLUTE_MAX_BUFFER_SIZE,
      1,
      ABSOLUTE_MAX_BUFFER_SIZE
    );
    _initial_buffer_size = std::clamp<uint32_t>(
      g_config.initial_frame_buffer_size ? g_config.initial_frame_buffer_size : 1,
      1,
      _max_buffer_size
    );
    _current_buffer_size = _initial_buffer_size;

    const auto now = std::chrono::steady_clock::now();
    _last_quiet_start = now;
    _last_buffer_check = now;
    _last_diagnostics_log_us = std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch()).count();

    _delivery_thread = std::jthread([this](std::stop_token stop_token) {
      delivery_thread_proc(stop_token);
    });
  }

  /**
   * @brief Destructor for WgcCaptureManager.
   *
   * Automatically cleans up capture session and frame pool resources.
   */
  ~WgcCaptureManager() noexcept {
    shutdown();
  }

  void shutdown() noexcept {
    if (_shutting_down.exchange(true, std::memory_order_acq_rel)) {
      return;
    }

    cleanup_capture_session();
    cleanup_frame_pool();

    // Ensure any in-flight FrameArrived callbacks have finished before destructing.
    // This avoids use-after-free on shutdown paths triggered by display reinit/HDR changes.
    auto last_log = std::chrono::steady_clock::now();
    while (_outstanding_frames.load(std::memory_order_acquire) > 0 || _layer_callbacks.load(std::memory_order_acquire) > 0) {
      auto now = std::chrono::steady_clock::now();
      if (now - last_log > std::chrono::seconds(2)) {
        last_log = now;
        BOOST_LOG(warning) << "Waiting for " << (_outstanding_frames.load(std::memory_order_relaxed) + _layer_callbacks.load(std::memory_order_relaxed))
                           << " in-flight frame(s) to finish before shutdown...";
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    stop_recomposite_thread();
    stop_delivery_thread();
  }

private:
  void stop_recomposite_thread() noexcept {
    {
      std::lock_guard lock(_recomposite_mutex);
      _recomposite_stop = true;
    }
    _recomposite_cv.notify_all();
    if (_recomposite_thread.joinable()) {
      _recomposite_thread.join();
    }
  }

  void stop_delivery_thread() noexcept {
    {
      std::lock_guard lock(_delivery_mutex);
      _delivery_stop = true;
      _pending_delivery_frame.reset();
      for (auto &scratch: _scratch_textures) {
        scratch.state = scratch_state_e::free;
      }
    }

    _delivery_cv.notify_all();
    if (_delivery_thread.joinable()) {
      _delivery_thread.request_stop();
      _delivery_thread.join();
    }
  }

  void cleanup_capture_session() noexcept {
    try {
      if (_capture_session) {
        _capture_session.Close();
        _capture_session = nullptr;
      }
    } catch (const winrt::hresult_error &ex) {
      BOOST_LOG(error) << "Exception during _capture_session.Close(): " << ex.code() << " - " << winrt::to_string(ex.message());
    } catch (...) {
      BOOST_LOG(error) << "Unknown exception during _capture_session.Close()";
    }
  }

  void cleanup_frame_pool() noexcept {
    try {
      if (_frame_pool) {
        if (_frame_arrived_token.value != 0) {
          _frame_pool.FrameArrived(_frame_arrived_token);  // Remove handler
          _frame_arrived_token.value = 0;
        }
        _frame_pool.Close();
        _frame_pool = nullptr;
      }
    } catch (const winrt::hresult_error &ex) {
      BOOST_LOG(error) << "Exception during _frame_pool.Close(): " << ex.code() << " - " << winrt::to_string(ex.message());
    } catch (...) {
      BOOST_LOG(error) << "Unknown exception during _frame_pool.Close()";
    }
  }

public:
  /**
   * @brief Deleted copy constructor to prevent resource duplication.
   */
  WgcCaptureManager(const WgcCaptureManager &) = delete;

  /**
   * @brief Deleted copy assignment operator to prevent resource duplication.
   */
  WgcCaptureManager &operator=(const WgcCaptureManager &) = delete;

  /**
   * @brief Deleted move constructor to prevent resource transfer issues.
   */
  WgcCaptureManager(WgcCaptureManager &&) = delete;
  WgcCaptureManager &operator=(WgcCaptureManager &&) = delete;

  /**
   * @brief Recreates the frame pool with a new buffer size for dynamic adjustment.
   * @param buffer_size The number of frames to buffer.
   * @returns true if the frame pool was recreated successfully, false otherwise.
   */
  bool create_or_adjust_frame_pool(uint32_t buffer_size) {
    if (!_deps || !_deps->winrt_device || _capture_format == DXGI_FORMAT_UNKNOWN) {
      return false;
    }

    if (_frame_pool) {
      // Use the proper Recreate method instead of closing and re-creating
      try {
        _frame_pool.Recreate(
          _deps->winrt_device,
          (_capture_format == DXGI_FORMAT_R16G16B16A16_FLOAT) ? DirectXPixelFormat::R16G16B16A16Float : DirectXPixelFormat::B8G8R8A8UIntNormalized,
          buffer_size,
          SizeInt32 {static_cast<int32_t>(_pool_width), static_cast<int32_t>(_pool_height)}
        );

        _current_buffer_size = buffer_size;
        BOOST_LOG(info) << "Frame pool recreated with buffer size: " << buffer_size;
        return true;
      } catch (const winrt::hresult_error &ex) {
        BOOST_LOG(error) << "Failed to recreate frame pool: " << ex.code() << " - " << winrt::to_string(ex.message());
        return false;
      }
    } else {
      // Initial creation case - create new frame pool
      _frame_pool = Direct3D11CaptureFramePool::CreateFreeThreaded(
        _deps->winrt_device,
        (_capture_format == DXGI_FORMAT_R16G16B16A16_FLOAT) ? DirectXPixelFormat::R16G16B16A16Float : DirectXPixelFormat::B8G8R8A8UIntNormalized,
        buffer_size,
        SizeInt32 {static_cast<int32_t>(_pool_width), static_cast<int32_t>(_pool_height)}
      );

      if (_frame_pool) {
        _current_buffer_size = buffer_size;
        BOOST_LOG(info) << "Frame pool created with buffer size: " << buffer_size;
        return true;
      }
    }

    return false;
  }

  /**
   * @brief Processes a frame when the frame arrived event is triggered.
   * @param sender The frame pool that triggered the event.
   */
  void process_frame(Direct3D11CaptureFramePool const &sender) {
    // Track callback invocations
    static std::atomic<uint64_t> callback_count {0};
    auto cc = ++callback_count;
    if (cc == 1) {
      BOOST_LOG(info) << "First FrameArrived callback invoked";
    }

    Direct3D11CaptureFrame frame = nullptr;
    uint32_t drained_frames = 0;

    try {
      if (drain_to_latest()) {
        while (auto next_frame = sender.TryGetNextFrame()) {
          if (frame) {
            ++drained_frames;
          }
          frame = std::move(next_frame);
        }
      } else {
        frame = sender.TryGetNextFrame();
      }
    } catch (const winrt::hresult_error &ex) {
      BOOST_LOG(error) << "WinRT error retrieving WGC frame: " << ex.code() << " - " << winrt::to_string(ex.message());
      return;
    }

    if (!frame) {
      // Frame drop detected - record timestamp for sliding window analysis
      auto now = std::chrono::steady_clock::now();
      _drop_timestamps.push_back(now);
      record_frame_pool_pressure(now);

      const auto total_empty_drops = _frame_pool_empty_drops.fetch_add(1, std::memory_order_relaxed) + 1;
      if (total_empty_drops <= 5 || total_empty_drops % 120 == 0) {
        BOOST_LOG(info) << "WGC frame pool returned no frame"
                        << " (recent drops in 5s window=" << _drop_timestamps.size()
                        << ", total=" << total_empty_drops << ")";
      }
    } else {
      // Frame successfully retrieved
      SizeInt32 content_size {0, 0};
      try {
        auto surface = frame.Surface();
        if (_window_mode) {
          // Only window capture needs the content size (blit bounds and pool resizes).
          content_size = frame.ContentSize();
        }

        // Get frame timing information from the WGC frame
        uint64_t frame_qpc = frame.SystemRelativeTime().count();
        record_frame_arrival(drained_frames);
        if (admit_activity_frame()) {
          queue_frame_for_delivery(std::move(frame), surface, frame_qpc, content_size);
        }
      } catch (const winrt::hresult_error &ex) {
        // Log error
        BOOST_LOG(error) << "WinRT error in frame processing: " << ex.code() << " - " << winrt::to_string(ex.message());
      }

      // A window resize changes the content size; the pool must follow it, but only
      // after the frame has been released back to the pool.
      frame = nullptr;
      if (_window_mode) {
        resize_window_frame_pool(content_size);
      }
    }

    // Check if we need to adjust frame buffer size
    check_and_adjust_frame_buffer();
  }

private:
  void resize_window_frame_pool(const SizeInt32 &content_size) {
    if (content_size.Width <= 0 || content_size.Height <= 0) {
      return;
    }

    const auto width = static_cast<UINT>(content_size.Width);
    const auto height = static_cast<UINT>(content_size.Height);
    if (width == _pool_width && height == _pool_height) {
      return;
    }

    BOOST_LOG(info) << "Window content size changed [" << _pool_width << 'x' << _pool_height
                    << " -> " << width << 'x' << height << "]; resizing frame pool";
    _pool_width = width;
    _pool_height = height;
    create_or_adjust_frame_pool(_current_buffer_size);
  }

  /**
   * @brief Map the window's client area in a WGC window frame onto the capture display.
   * @return An empty blit when the window is minimized, gone, or not on the display.
   */
  platf::dxgi::window_policy::window_blit_t query_window_blit(const winrt::com_ptr<ID3D11Texture2D> &frame_tex, const SizeInt32 &content_size) const {
    platf::dxgi::window_policy::window_blit_t none {};
    none.empty = true;

    if (!_target_hwnd || !IsWindow(_target_hwnd) || IsIconic(_target_hwnd)) {
      return none;
    }

    RECT frame_bounds {};
    if (FAILED(DwmGetWindowAttribute(_target_hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &frame_bounds, sizeof(frame_bounds)))) {
      return none;
    }

    RECT client {};
    POINT client_origin {0, 0};
    if (!GetClientRect(_target_hwnd, &client) || !ClientToScreen(_target_hwnd, &client_origin)) {
      return none;
    }

    D3D11_TEXTURE2D_DESC desc {};
    frame_tex->GetDesc(&desc);
    const auto content_w = static_cast<std::int32_t>(std::min<std::int64_t>(content_size.Width, desc.Width));
    const auto content_h = static_cast<std::int32_t>(std::min<std::int64_t>(content_size.Height, desc.Height));

    // The output is sized from the shared texture so the destination can never exceed it.
    const platf::dxgi::window_policy::rect_i output {
      _monitor_rect.left,
      _monitor_rect.top,
      _monitor_rect.left + static_cast<LONG>(_width),
      _monitor_rect.top + static_cast<LONG>(_height),
    };
    const platf::dxgi::window_policy::rect_i bounds {frame_bounds.left, frame_bounds.top, frame_bounds.right, frame_bounds.bottom};
    const platf::dxgi::window_policy::rect_i client_screen {
      client_origin.x,
      client_origin.y,
      client_origin.x + client.right,
      client_origin.y + client.bottom,
    };
    return platf::dxgi::window_policy::compute_window_blit(output, bounds, client_screen, content_w, content_h);
  }

  /**
   * @brief Fill a scratch texture with opaque black. Must be called with the D3D context lock held.
   */
  bool clear_scratch_texture(scratch_texture_t &scratch) {
    if (!scratch.texture) {
      return false;
    }

    if (!scratch.rtv) {
      winrt::com_ptr<ID3D11Device> device;
      _deps->d3d_context->GetDevice(device.put());
      if (!device || FAILED(device->CreateRenderTargetView(scratch.texture.get(), nullptr, scratch.rtv.put()))) {
        BOOST_LOG(error) << "Failed to create render target view for window-only scratch texture";
        scratch.rtv = nullptr;
        return false;
      }
    }

    // Zero RGB is black for both BGRA8 and FP16 scRGB.
    constexpr float black[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    _deps->d3d_context->ClearRenderTargetView(scratch.rtv.get(), black);
    return true;
  }

  /**
   * @brief Copy the window's client area into the scratch texture and black out the rest.
   * Must be called with the D3D context lock held.
   * @param blit Placement from query_window_blit, computed before taking the lock.
   * @return false when the frame must be dropped.
   */
  bool composite_window_frame(scratch_texture_t &scratch, const winrt::com_ptr<ID3D11Texture2D> &frame_tex, const platf::dxgi::window_policy::window_blit_t &blit) {
    if ((blit.empty || !blit.covers_output) && !clear_scratch_texture(scratch)) {
      return false;
    }
    if (blit.empty) {
      return true;
    }

    const D3D11_BOX src_box {blit.src_left, blit.src_top, 0, blit.src_right, blit.src_bottom, 1};
    _deps->d3d_context->CopySubresourceRegion(scratch.texture.get(), 0, blit.dst_x, blit.dst_y, 0, frame_tex.get(), 0, &src_box);
    return true;
  }

  /**
   * @brief Secondary layers that may be drawn now, with their current bounds.
   *
   * Every layer is checked again right before compositing, outside the context lock: the
   * window must still exist, be visible, not minimized or cloaked, belong to the same process
   * as when its layer was created, and that process must still be in the app window set. A
   * hidden window or a reused HWND of another process is never drawn, even before the layer
   * manager handles its event.
   */
  void collect_drawable_layers(collect_buffers_t &buffers) {
    buffers.draws.clear();
    if (!_layer_mode) {
      return;
    }

    // Refreshed even when nothing is published, so the buffers always name the publication
    // they reflect and an unchanged empty publication never looks like a layer change.
    if (buffers.layers_generation != _layers_generation.load(std::memory_order_acquire)) {
      std::lock_guard lock(_layers_mutex);
      buffers.layers.assign(_published_layers.begin(), _published_layers.end());
      buffers.layers_generation = _layers_generation.load(std::memory_order_relaxed);
    }
    if (buffers.layers.empty()) {
      return;
    }

    refresh_window_set_pids(reinterpret_cast<std::uintptr_t>(_target_hwnd), buffers.pids, buffers.pids_generation);
    if (buffers.pids.empty()) {
      return;
    }

    const auto now = qpc_now();
    const auto reuse_delay = qpc_ticks(SECONDARY_REUSE_DELAY);
    for (const auto &published : buffers.layers) {
      const auto &layer = *published.layer;
      if (layer.closed.load(std::memory_order_acquire) ||
          !platf::dxgi::window_policy::layer_content_ready(
            layer.frame_qpc.load(std::memory_order_acquire),
            layer.shown_qpc.load(std::memory_order_acquire),
            now,
            reuse_delay
          )) {
        continue;
      }

      HWND hwnd = layer.hwnd;
      if (!IsWindow(hwnd) || !IsWindowVisible(hwnd) || IsIconic(hwnd)) {
        continue;
      }
      DWORD window_pid = 0;
      GetWindowThreadProcessId(hwnd, &window_pid);
      if (window_pid != layer.window_pid || !std::binary_search(buffers.pids.begin(), buffers.pids.end(), static_cast<uint32_t>(window_pid))) {
        continue;
      }
      DWORD cloaked = 0;
      if (SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked != 0) {
        continue;
      }
      RECT bounds {};
      if (FAILED(DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &bounds, sizeof(bounds)))) {
        continue;
      }

      buffers.draws.push_back(layer_draw_t {
        .layer = published.layer,
        .bounds = {bounds.left, bounds.top, bounds.right, bounds.bottom},
        .below_target = published.below_target,
      });
    }
  }

  /**
   * @brief Frame time for a frame queued in layer mode; never earlier than the previous one,
   * since recomposites use the current time. Call with the context lock held.
   */
  uint64_t layered_frame_qpc(const uint64_t frame_qpc) {
    _last_layered_qpc = std::max(_last_layered_qpc, frame_qpc);
    return _last_layered_qpc;
  }

  /**
   * @brief Composite and queue a target frame in layer mode.
   *
   * Composited and queued under the context lock, like recomposites, so frames are queued in
   * the order they were composited. When the published layers changed after they were read
   * for this frame (a menu closed meanwhile), a recomposite follows so the change is never
   * lost behind this frame.
   */
  void queue_layered_target_frame(
    Direct3D11CaptureFrame frame,
    const winrt::com_ptr<ID3D11Texture2D> &frame_tex,
    const size_t scratch_index,
    const platf::dxgi::window_policy::window_blit_t &window_blit,
    const uint64_t frame_qpc
  ) {
    std::lock_guard collect_lock(_target_collect_mutex);
    // This frame is composited from the current layers, so an earlier recomposite request
    // is covered; a layer change after this point requests a new one.
    _recomposite_pending.store(false, std::memory_order_release);
    collect_drawable_layers(_target_collect);
    const auto &layers = _target_collect.draws;

    bool queued = false;
    {
      std::lock_guard context_lock(_d3d_context_mutex);
      auto &scratch = _scratch_textures[scratch_index];
      bool copied = false;
      const auto scratch_copy_start = std::chrono::steady_clock::now();
      if (!layers.empty() && composite_target_with_layers(scratch, frame_tex, window_blit, layers)) {
        copied = true;
      } else {
        copied = composite_window_frame(scratch, frame_tex, window_blit);
        if (copied) {
          note_target_only_frame(scratch_index, window_blit);
        }
      }
      _scratch_copy_timer.collect(std::chrono::steady_clock::now() - scratch_copy_start);
      frame = nullptr;

      // Only a real publication change counts. With no layer published and none in the
      // latest frame there is nothing a recomposite could change, so the worker is not woken.
      if (_target_collect.layers_generation != _layers_generation.load(std::memory_order_acquire) &&
          (_layers_published.load(std::memory_order_acquire) || _last_frame_had_layers.load(std::memory_order_acquire))) {
        const auto requests = _target_recomposite_requests.fetch_add(1, std::memory_order_relaxed) + 1;
        if (requests <= 5 || requests % 120 == 0) {
          BOOST_LOG(debug) << "Secondary layers changed while a target frame was composited; recompositing"
                           << " (count=" << requests << ")";
        }
        request_recomposite();
      }
      if (copied) {
        queued = enqueue_scratch_texture(scratch_index, layered_frame_qpc(frame_qpc));
      } else {
        release_reserved_scratch_texture(scratch_index);
      }
    }
    if (queued) {
      _delivery_cv.notify_one();
    }
  }

  /**
   * @brief Create `_base` and make it hold the target alone. Call with the context lock held.
   *
   * After a frame composited without layers `_base` is stale; that frame's scratch texture
   * holds exactly the target alone on black, so it seeds `_base`. Scratch textures are only
   * written under the context lock and never by a layered frame while `_base` is stale, so
   * that scratch still holds the frame here.
   */
  bool ensure_base() {
    if (!ensure_base_texture()) {
      return false;
    }
    if (_base_valid) {
      return true;
    }
    if (_last_target_only_scratch && *_last_target_only_scratch < _scratch_textures.size() &&
        _scratch_textures[*_last_target_only_scratch].texture) {
      _deps->d3d_context->CopyResource(_base.texture.get(), _scratch_textures[*_last_target_only_scratch].texture.get());
    } else if (!clear_scratch_texture(_base)) {
      return false;
    }
    _base_valid = true;
    return true;
  }

  /**
   * @brief Create the display-sized `_base` texture once. Call with the context lock held.
   */
  bool ensure_base_texture() {
    if (!_base.texture) {
      winrt::com_ptr<ID3D11Device> device;
      _deps->d3d_context->GetDevice(device.put());
      if (!device) {
        return false;
      }

      D3D11_TEXTURE2D_DESC desc = {};
      desc.Width = _width;
      desc.Height = _height;
      desc.MipLevels = 1;
      desc.ArraySize = 1;
      desc.Format = _capture_format;
      desc.SampleDesc.Count = 1;
      desc.Usage = D3D11_USAGE_DEFAULT;
      desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
      if (FAILED(device->CreateTexture2D(&desc, nullptr, _base.texture.put()))) {
        BOOST_LOG(error) << "Failed to create the window-only base texture";
        _base.texture = nullptr;
        return false;
      }
      _base_valid = false;
    }
    return true;
  }

  /**
   * @brief Remember a frame composited without layers. Call with the context lock held.
   */
  void note_target_only_frame(const size_t scratch_index, const platf::dxgi::window_policy::window_blit_t &blit) {
    _last_target_only_scratch = scratch_index;
    _last_target_blit = blit;
    _base_valid = false;
    _last_frame_had_layers.store(false, std::memory_order_release);
  }

  /**
   * @brief Composite a target frame into `_base`, then `_base` and the layers into the scratch.
   * Call with the context lock held.
   */
  bool composite_target_with_layers(
    scratch_texture_t &scratch,
    const winrt::com_ptr<ID3D11Texture2D> &frame_tex,
    const platf::dxgi::window_policy::window_blit_t &blit,
    const std::vector<layer_draw_t> &layers
  ) {
    if (!ensure_base_texture() || !composite_window_frame(_base, frame_tex, blit)) {
      _base_valid = false;
      return false;
    }
    _base_valid = true;
    _last_target_blit = blit;
    compose_layers(scratch, layers);
    _last_frame_had_layers.store(true, std::memory_order_release);
    return true;
  }

  /**
   * @brief Draw `_base` and the layers bottom-up into the scratch. Layers below the target
   * are drawn first and the target's client area is copied over them from `_base`.
   * Call with the context lock held and `_base` valid.
   */
  void compose_layers(scratch_texture_t &scratch, const std::vector<layer_draw_t> &layers) {
    const bool any_below = std::any_of(layers.begin(), layers.end(), [](const layer_draw_t &layer) {
      return layer.below_target;
    });

    if (!any_below) {
      _deps->d3d_context->CopyResource(scratch.texture.get(), _base.texture.get());
    } else {
      clear_scratch_texture(scratch);
      for (const auto &layer : layers) {
        if (layer.below_target) {
          draw_layer(scratch, layer);
        }
      }
      if (!_last_target_blit.empty) {
        const UINT width = _last_target_blit.src_right - _last_target_blit.src_left;
        const UINT height = _last_target_blit.src_bottom - _last_target_blit.src_top;
        const D3D11_BOX target_box {_last_target_blit.dst_x, _last_target_blit.dst_y, 0, _last_target_blit.dst_x + width, _last_target_blit.dst_y + height, 1};
        _deps->d3d_context->CopySubresourceRegion(scratch.texture.get(), 0, _last_target_blit.dst_x, _last_target_blit.dst_y, 0, _base.texture.get(), 0, &target_box);
      }
    }

    for (const auto &layer : layers) {
      if (!layer.below_target) {
        draw_layer(scratch, layer);
      }
    }
  }

  /**
   * @brief Copy a layer's cached frame (opaque) to its true position. Context lock held.
   */
  void draw_layer(scratch_texture_t &scratch, const layer_draw_t &draw) {
    const auto &layer = *draw.layer;
    if (!layer.cached || layer.content_width == 0 || layer.content_height == 0) {
      return;
    }
    const auto blit = platf::dxgi::window_policy::compute_layer_blit(
      output_rect(),
      draw.bounds,
      static_cast<std::int32_t>(layer.content_width),
      static_cast<std::int32_t>(layer.content_height)
    );
    if (blit.empty) {
      return;
    }
    const D3D11_BOX src_box {blit.src_left, blit.src_top, 0, blit.src_right, blit.src_bottom, 1};
    _deps->d3d_context->CopySubresourceRegion(scratch.texture.get(), 0, blit.dst_x, blit.dst_y, 0, layer.cached.get(), 0, &src_box);
  }

  /**
   * @brief Copy the valid part of a layer frame into the layer's cache. Context lock held.
   */
  bool cache_layer_frame(SecondaryLayer &layer, const winrt::com_ptr<ID3D11Texture2D> &frame_tex, const SizeInt32 &content_size) {
    D3D11_TEXTURE2D_DESC desc {};
    frame_tex->GetDesc(&desc);
    if (desc.Format != _capture_format || content_size.Width <= 0 || content_size.Height <= 0) {
      return false;
    }
    const auto width = static_cast<UINT>(std::min<std::int64_t>(content_size.Width, desc.Width));
    const auto height = static_cast<UINT>(std::min<std::int64_t>(content_size.Height, desc.Height));
    if (width == 0 || height == 0) {
      return false;
    }

    if (!layer.cached || layer.cached_width < width || layer.cached_height < height) {
      winrt::com_ptr<ID3D11Device> device;
      _deps->d3d_context->GetDevice(device.put());
      if (!device) {
        return false;
      }
      D3D11_TEXTURE2D_DESC cache_desc = {};
      cache_desc.Width = std::max(width, layer.cached_width);
      cache_desc.Height = std::max(height, layer.cached_height);
      cache_desc.MipLevels = 1;
      cache_desc.ArraySize = 1;
      cache_desc.Format = _capture_format;
      cache_desc.SampleDesc.Count = 1;
      cache_desc.Usage = D3D11_USAGE_DEFAULT;
      winrt::com_ptr<ID3D11Texture2D> texture;
      if (FAILED(device->CreateTexture2D(&cache_desc, nullptr, texture.put()))) {
        BOOST_LOG(warning) << "Failed to create a secondary window cache texture";
        return false;
      }
      layer.cached = texture;
      layer.cached_width = cache_desc.Width;
      layer.cached_height = cache_desc.Height;
    }

    const D3D11_BOX src_box {0, 0, 0, width, height, 1};
    _deps->d3d_context->CopySubresourceRegion(layer.cached.get(), 0, 0, 0, 0, frame_tex.get(), 0, &src_box);
    layer.content_width = width;
    layer.content_height = height;
    return true;
  }

  /**
   * @brief Follow a layer window's size with its frame pool, after the frame was released.
   */
  void resize_layer_pool(SecondaryLayer &layer, Direct3D11CaptureFramePool const &sender, const SizeInt32 &content_size) {
    if (content_size.Width <= 0 || content_size.Height <= 0) {
      return;
    }
    std::lock_guard lock(layer.pool_mutex);
    if (content_size.Width == layer.pool_size.Width && content_size.Height == layer.pool_size.Height) {
      return;
    }
    try {
      sender.Recreate(_deps->winrt_device, pixel_format(), layer.pool_buffers, content_size);
      layer.pool_size = content_size;
    } catch (const winrt::hresult_error &ex) {
      BOOST_LOG(debug) << "Failed to resize a secondary window frame pool: " << ex.code();
    }
  }

  /**
   * @brief Give a layer's frame pool a second buffer once it reports dropped frames.
   */
  void grow_layer_pool(SecondaryLayer &layer, Direct3D11CaptureFramePool const &sender) {
    std::lock_guard lock(layer.pool_mutex);
    if (layer.pool_buffers >= 2 || ++layer.empty_frames < 2 || layer.pool_size.Width <= 0 || layer.pool_size.Height <= 0) {
      return;
    }
    try {
      sender.Recreate(_deps->winrt_device, pixel_format(), 2, layer.pool_size);
      layer.pool_buffers = 2;
    } catch (const winrt::hresult_error &ex) {
      BOOST_LOG(debug) << "Failed to grow a secondary window frame pool: " << ex.code();
    }
  }

  void recomposite_thread_proc() noexcept {
    std::unique_lock lock(_recomposite_mutex);
    while (!_recomposite_stop) {
      _recomposite_cv.wait(lock, [this]() {
        return _recomposite_stop || _recomposite_pending.load(std::memory_order_acquire);
      });
      if (_recomposite_stop) {
        break;
      }

      lock.unlock();
      bool done = true;
      try {
        done = try_recomposite();
      } catch (...) {
        BOOST_LOG(error) << "Unexpected error while recompositing secondary windows";
      }
      lock.lock();
      if (!done && !_recomposite_stop) {
        // No scratch texture was free; the delivery thread frees one within a frame.
        _recomposite_cv.wait_for(lock, std::chrono::milliseconds(2), [this]() {
          return _recomposite_stop;
        });
      }
    }
  }

  /**
   * @brief Publish a frame for a secondary window change that came without a target frame:
   * a layer delivered new content, showed, moved or went away. Recomposite thread only.
   * @return false when no scratch texture was free; the request then stays pending.
   */
  bool try_recomposite() {
    if (!_layer_mode || _shutting_down.load(std::memory_order_acquire) ||
        !_recomposite_pending.exchange(false, std::memory_order_acq_rel)) {
      return true;
    }

    collect_drawable_layers(_recomposite_collect);
    const auto &layers = _recomposite_collect.draws;
    if (layers.empty() && !_last_frame_had_layers.load(std::memory_order_acquire)) {
      // The latest frame already shows the target alone.
      return true;
    }

    auto scratch_index = reserve_scratch_texture();
    if (!scratch_index) {
      _recomposite_pending.store(true, std::memory_order_release);
      return false;
    }
    if (!ensure_scratch_texture(*scratch_index)) {
      release_reserved_scratch_texture(*scratch_index);
      return true;
    }

    bool queued = false;
    {
      // Composed and queued under the context lock, like layer-mode target frames, so frames
      // are queued in the order they were composited.
      std::lock_guard context_lock(_d3d_context_mutex);
      if (ensure_base()) {
        compose_layers(_scratch_textures[*scratch_index], layers);
        _last_frame_had_layers.store(!layers.empty(), std::memory_order_release);
        queued = enqueue_scratch_texture(*scratch_index, layered_frame_qpc(static_cast<uint64_t>(qpc_now())));
      } else {
        release_reserved_scratch_texture(*scratch_index);
      }
      if (_recomposite_collect.layers_generation != _layers_generation.load(std::memory_order_acquire)) {
        _recomposite_pending.store(true, std::memory_order_release);
      }
    }
    if (queued) {
      _delivery_cv.notify_one();
    }
    return true;
  }

  bool drain_to_latest() const {
    return (g_config.flags & platf::dxgi::WGC_IPC_FLAG_DRAIN_TO_LATEST) != 0;
  }

  bool allow_buffer_decrease() const {
    return (g_config.flags & platf::dxgi::WGC_IPC_FLAG_ALLOW_BUFFER_DECREASE) != 0;
  }

  static int64_t steady_time_us(const std::chrono::steady_clock::time_point &time) {
    return std::chrono::duration_cast<std::chrono::microseconds>(time.time_since_epoch()).count();
  }

  void record_frame_pool_pressure(const std::chrono::steady_clock::time_point &now) {
    _last_frame_pool_pressure_us.store(steady_time_us(now), std::memory_order_relaxed);
  }

  static bool should_log_rate_limited(std::atomic<int64_t> &last_log_us, const std::chrono::steady_clock::time_point &now, std::chrono::microseconds interval) {
    const auto now_us = steady_time_us(now);
    auto last_us = last_log_us.load(std::memory_order_relaxed);
    while (last_us == 0 || now_us - last_us >= interval.count()) {
      if (last_log_us.compare_exchange_weak(last_us, now_us, std::memory_order_acq_rel, std::memory_order_relaxed)) {
        return true;
      }
    }
    return false;
  }

  double expected_frame_ms() const {
    return 1000.0 / static_cast<double>(std::max(1, g_config.target_fps));
  }

  double approximate_extra_pool_latency_ms(uint32_t buffer_size) const {
    return buffer_size > _initial_buffer_size ?
             static_cast<double>(buffer_size - _initial_buffer_size) * expected_frame_ms() :
             0.0;
  }

  void record_frame_arrival(uint32_t drained_frames) {
    _captured_frames.fetch_add(1, std::memory_order_relaxed);

    if (drained_frames > 0) {
      const auto now = std::chrono::steady_clock::now();
      record_frame_pool_pressure(now);
      const auto total_drained = _drained_pool_frames.fetch_add(drained_frames, std::memory_order_relaxed) + drained_frames;
      if (should_log_rate_limited(_last_drain_log_us, now, std::chrono::duration_cast<std::chrono::microseconds>(WGC_DRAIN_LOG_INTERVAL))) {
        BOOST_LOG(info) << "WGC drained " << drained_frames << " queued frame(s) from frame pool"
                        << " (total_drained=" << total_drained
                        << ", buffer=" << _current_buffer_size << "/" << _max_buffer_size
                        << ", approx_extra_pool_latency_ms=" << approximate_extra_pool_latency_ms(_current_buffer_size) << ")";
      }
    }
  }

  void maybe_log_capture_diagnostics(const std::chrono::steady_clock::time_point &now) {
    const auto now_us = steady_time_us(now);
    auto last_log_us = _last_diagnostics_log_us.load(std::memory_order_relaxed);
    const auto interval_us = std::chrono::duration_cast<std::chrono::microseconds>(WGC_DIAGNOSTICS_LOG_INTERVAL).count();

    while (now_us - last_log_us >= interval_us) {
      if (!_last_diagnostics_log_us.compare_exchange_weak(last_log_us, now_us, std::memory_order_acq_rel, std::memory_order_relaxed)) {
        continue;
      }

      const double interval_s = static_cast<double>(now_us - last_log_us) / 1000000.0;
      if (interval_s <= 0.0) {
        return;
      }

      const auto captured = _captured_frames.load(std::memory_order_relaxed);
      const auto published = _published_frames.load(std::memory_order_relaxed);
      const auto empty_drops = _frame_pool_empty_drops.load(std::memory_order_relaxed);
      const auto drained = _drained_pool_frames.load(std::memory_order_relaxed);
      const auto replaced = _delivery_replaced_frames.load(std::memory_order_relaxed);
      const auto scratch_dropped = _scratch_dropped_frames.load(std::memory_order_relaxed);
      const auto slow_context = _slow_context_waits.load(std::memory_order_relaxed);
      const auto slow_mutex = _slow_mutex_waits.load(std::memory_order_relaxed);
      const auto slow_hold = _slow_shared_mutex_holds.load(std::memory_order_relaxed);
      const auto slow_copy = _slow_copy_submissions.load(std::memory_order_relaxed);
      const auto activity_rate_limited = _activity_rate_limited_frames.load(std::memory_order_relaxed);

      const auto captured_delta = captured - _last_diagnostics_captured_frames;
      const auto published_delta = published - _last_diagnostics_published_frames;
      const auto empty_drop_delta = empty_drops - _last_diagnostics_empty_drops;
      const auto drained_delta = drained - _last_diagnostics_drained_frames;
      const auto replaced_delta = replaced - _last_diagnostics_replaced_frames;
      const auto scratch_dropped_delta = scratch_dropped - _last_diagnostics_scratch_dropped;
      const auto slow_context_delta = slow_context - _last_diagnostics_slow_context;
      const auto slow_mutex_delta = slow_mutex - _last_diagnostics_slow_mutex;
      const auto slow_hold_delta = slow_hold - _last_diagnostics_slow_hold;
      const auto slow_copy_delta = slow_copy - _last_diagnostics_slow_copy;
      const auto activity_rate_limited_delta = activity_rate_limited - _last_diagnostics_activity_rate_limited;

      _last_diagnostics_captured_frames = captured;
      _last_diagnostics_published_frames = published;
      _last_diagnostics_empty_drops = empty_drops;
      _last_diagnostics_drained_frames = drained;
      _last_diagnostics_replaced_frames = replaced;
      _last_diagnostics_scratch_dropped = scratch_dropped;
      _last_diagnostics_slow_context = slow_context;
      _last_diagnostics_slow_mutex = slow_mutex;
      _last_diagnostics_slow_hold = slow_hold;
      _last_diagnostics_slow_copy = slow_copy;
      _last_diagnostics_activity_rate_limited = activity_rate_limited;

      BOOST_LOG(info) << "WGC capture diagnostics: interval_s=" << interval_s
                      << " buffer=" << _current_buffer_size << "/" << _max_buffer_size
                      << " approx_extra_pool_latency_ms=" << approximate_extra_pool_latency_ms(_current_buffer_size)
                      << " capture_fps=" << (static_cast<double>(captured_delta) / interval_s)
                      << " publish_fps=" << (static_cast<double>(published_delta) / interval_s)
                      << " drained=" << drained_delta
                      << " activity_rate_limited=" << activity_rate_limited_delta
                      << " empty_drops=" << empty_drop_delta
                      << " delivery_replaced=" << replaced_delta
                      << " scratch_dropped=" << scratch_dropped_delta
                      << " slow_context=" << slow_context_delta
                      << " slow_mutex=" << slow_mutex_delta
                      << " slow_shared_hold=" << slow_hold_delta
                      << " slow_copy=" << slow_copy_delta;
      return;
    }
  }

  bool admit_activity_frame() {
    const auto generation = g_activity_admission_generation.load(std::memory_order_acquire);
    if (generation != _activity_admission_generation) {
      _activity_admission_generation = generation;
      _last_activity_admitted = {};
    }

    const auto admission_fps = g_activity_admission_fps.load(std::memory_order_relaxed);
    if (admission_fps <= 0) {
      return true;
    }

    const auto now = std::chrono::steady_clock::now();
    const auto minimum_interval = std::chrono::nanoseconds(std::chrono::seconds(1)) / admission_fps;
    if (_last_activity_admitted.time_since_epoch().count() != 0 &&
        now - _last_activity_admitted < minimum_interval) {
      _activity_rate_limited_frames.fetch_add(1, std::memory_order_relaxed);
      return false;
    }

    _last_activity_admitted = now;
    return true;
  }

  /**
   * @brief Prunes old frame drop timestamps from the sliding window.
   * @param now Current timestamp for comparison.
   */
  void prune_old_drop_timestamps(const std::chrono::steady_clock::time_point &now) {
    while (!_drop_timestamps.empty() && now - _drop_timestamps.front() > std::chrono::seconds(5)) {
      _drop_timestamps.pop_front();
    }
  }

  /**
   * @brief Checks if buffer size should be increased due to recent frame drops.
   * @param now Current timestamp.
   * @return true if buffer was increased, false otherwise.
   */
  bool try_increase_buffer_size(const std::chrono::steady_clock::time_point &now) {
    if (_drop_timestamps.size() >= 2 && _current_buffer_size < _max_buffer_size) {
      uint32_t new_buffer_size = _current_buffer_size + 1;
      BOOST_LOG(info) << "Detected " << _drop_timestamps.size() << " frame drops in 5s window, increasing buffer from "
                      << _current_buffer_size << " to " << new_buffer_size
                      << " (approx_extra_pool_latency_ms=" << approximate_extra_pool_latency_ms(new_buffer_size) << ")";
      create_or_adjust_frame_pool(new_buffer_size);
      _drop_timestamps.clear();  // Reset after adjustment
      _peak_outstanding = 0;  // Reset peak tracking
      _last_quiet_start = now;  // Reset quiet timer
      return true;
    }
    return false;
  }

  /**
   * @brief Checks if buffer size should be decreased due to sustained quiet period.
   * @param now Current timestamp.
   * @return true if buffer was decreased, false otherwise.
   */
  bool try_decrease_buffer_size(const std::chrono::steady_clock::time_point &now) {
    if (!allow_buffer_decrease()) {
      return false;
    }

    constexpr auto quiet_period = std::chrono::seconds(30);
    const auto last_pressure_us = _last_frame_pool_pressure_us.load(std::memory_order_relaxed);
    const auto quiet_period_us = std::chrono::duration_cast<std::chrono::microseconds>(quiet_period).count();
    const bool recent_pool_pressure = last_pressure_us > 0 &&
                                      steady_time_us(now) - last_pressure_us < quiet_period_us;
    const bool is_quiet = platf::dxgi::wgc_policy::buffer_pool_is_quiet(
      allow_buffer_decrease(),
      !_drop_timestamps.empty(),
      recent_pool_pressure,
      _peak_outstanding.load(),
      _current_buffer_size
    );

    if (!is_quiet) {
      _last_quiet_start = now;  // Reset quiet timer
      return false;
    }

    // Check if we've been quiet for 30 seconds
    if (now - _last_quiet_start >= quiet_period && _current_buffer_size > _initial_buffer_size) {
      uint32_t new_buffer_size = _current_buffer_size - 1;
      BOOST_LOG(info) << "Sustained quiet period (30s) with peak occupancy " << _peak_outstanding.load()
                      << " <= " << (_current_buffer_size - 1) << ", decreasing buffer from "
                      << _current_buffer_size << " to " << new_buffer_size
                      << " (approx_extra_pool_latency_ms=" << approximate_extra_pool_latency_ms(new_buffer_size) << ")";
      create_or_adjust_frame_pool(new_buffer_size);
      _peak_outstanding = 0;  // Reset peak tracking
      _last_quiet_start = now;  // Reset quiet timer
      return true;
    }

    return false;
  }

  bool ensure_scratch_texture(size_t index) {
    if (!_deps || index >= _scratch_textures.size()) {
      return false;
    }

    auto &scratch = _scratch_textures[index];
    if (scratch.texture) {
      return true;
    }

    winrt::com_ptr<ID3D11Device> device;
    _deps->d3d_context->GetDevice(device.put());
    if (!device) {
      BOOST_LOG(error) << "Failed to get D3D11 device for WGC scratch texture";
      return false;
    }

    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = _width;
    desc.Height = _height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = _capture_format;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

    HRESULT hr = device->CreateTexture2D(&desc, nullptr, scratch.texture.put());
    if (FAILED(hr)) {
      BOOST_LOG(error) << "Failed to create WGC scratch texture: " << std::format(": 0x{:08X}", hr);
      return false;
    }

    return true;
  }

  std::optional<size_t> reserve_scratch_texture() {
    std::lock_guard lock(_delivery_mutex);

    if (_delivery_stop || _shutting_down.load(std::memory_order_acquire)) {
      return std::nullopt;
    }

    for (size_t i = 0; i < _scratch_textures.size(); ++i) {
      if (_scratch_textures[i].state == scratch_state_e::free) {
        _scratch_textures[i].state = scratch_state_e::reserved;
        return i;
      }
    }

    if (_pending_delivery_frame) {
      const auto stale_index = _pending_delivery_frame->scratch_index;
      if (stale_index >= _scratch_textures.size()) {
        _pending_delivery_frame.reset();
        return std::nullopt;
      }

      _pending_delivery_frame.reset();
      _scratch_textures[stale_index].state = scratch_state_e::reserved;

      const auto replaced = _delivery_replaced_frames.fetch_add(1, std::memory_order_relaxed) + 1;
      if (replaced <= 5 || replaced % 120 == 0) {
        BOOST_LOG(debug) << "WGC delivery replaced stale queued scratch frame"
                         << " (count=" << replaced << ")";
      }
      return stale_index;
    }

    const auto dropped = _scratch_dropped_frames.fetch_add(1, std::memory_order_relaxed) + 1;
    if (dropped <= 5 || dropped % 120 == 0) {
      BOOST_LOG(debug) << "WGC scratch handoff had no free texture; dropping frame"
                       << " (count=" << dropped << ")";
    }
    return std::nullopt;
  }

  void release_reserved_scratch_texture(size_t index) {
    std::lock_guard lock(_delivery_mutex);
    if (index < _scratch_textures.size() && _scratch_textures[index].state == scratch_state_e::reserved) {
      _scratch_textures[index].state = scratch_state_e::free;
    }
  }

  bool enqueue_scratch_texture(size_t index, uint64_t frame_qpc) {
    std::lock_guard lock(_delivery_mutex);

    if (index >= _scratch_textures.size() || !_scratch_textures[index].texture) {
      return false;
    }

    if (_delivery_stop || _shutting_down.load(std::memory_order_acquire)) {
      _scratch_textures[index].state = scratch_state_e::free;
      return false;
    }

    if (_pending_delivery_frame) {
      const auto stale_index = _pending_delivery_frame->scratch_index;
      _pending_delivery_frame.reset();
      if (stale_index < _scratch_textures.size()) {
        _scratch_textures[stale_index].state = scratch_state_e::free;
      }

      const auto replaced = _delivery_replaced_frames.fetch_add(1, std::memory_order_relaxed) + 1;
      if (replaced <= 5 || replaced % 120 == 0) {
        BOOST_LOG(debug) << "WGC delivery replaced stale queued scratch frame"
                         << " (count=" << replaced << ")";
      }
    }

    _scratch_textures[index].state = scratch_state_e::pending;
    _pending_delivery_frame = delivery_frame_t {
      .texture = _scratch_textures[index].texture,
      .scratch_index = index,
      .frame_qpc = frame_qpc
    };
    return true;
  }

  /**
   * @brief Copies the next WGC frame into helper-owned scratch storage and queues it for delivery.
   * @param frame The WGC frame object; it is released before the shared IPC mutex can block.
   * @param surface The captured D3D11 surface.
   * @param frame_qpc The QPC timestamp from when the frame was captured.
   * @param content_size The valid size of the frame surface (used in window-only mode).
   */
  void queue_frame_for_delivery(Direct3D11CaptureFrame frame, winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DSurface surface, uint64_t frame_qpc, SizeInt32 content_size) {
    if (!_deps) {
      return;
    }

    // Get DXGI access
    winrt::com_ptr<IDirect3DDxgiInterfaceAccess> ia;
    if (FAILED(winrt::get_unknown(surface)->QueryInterface(__uuidof(IDirect3DDxgiInterfaceAccess), ia.put_void()))) {
      BOOST_LOG(error) << "Failed to query IDirect3DDxgiInterfaceAccess";
      return;
    }

    // Get underlying texture
    winrt::com_ptr<ID3D11Texture2D> frame_tex;
    if (FAILED(ia->GetInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<::IUnknown **>(frame_tex.put_void())))) {
      BOOST_LOG(error) << "Failed to get ID3D11Texture2D from interface";
      return;
    }

    auto scratch_index = reserve_scratch_texture();
    if (!scratch_index) {
      return;
    }

    if (!ensure_scratch_texture(*scratch_index)) {
      release_reserved_scratch_texture(*scratch_index);
      return;
    }

    // The window queries are syscalls into the window manager; keep them out of the context lock.
    platf::dxgi::window_policy::window_blit_t window_blit {};
    if (_window_mode) {
      window_blit = query_window_blit(frame_tex, content_size);
    }

    if (_layer_mode) {
      queue_layered_target_frame(std::move(frame), frame_tex, *scratch_index, window_blit, frame_qpc);
      return;
    }

    bool copied = true;
    {
      std::lock_guard context_lock(_d3d_context_mutex);
      const auto scratch_copy_start = std::chrono::steady_clock::now();
      if (_window_mode) {
        copied = composite_window_frame(_scratch_textures[*scratch_index], frame_tex, window_blit);
      } else {
        _deps->d3d_context->CopyResource(_scratch_textures[*scratch_index].texture.get(), frame_tex.get());
      }
      _scratch_copy_timer.collect(std::chrono::steady_clock::now() - scratch_copy_start);
    }

    // From here on the delivery thread owns only the helper scratch texture, not
    // the Direct3D11CaptureFrame/WGC frame-pool buffer. The helper can wait for
    // the main process' shared keyed mutex without starving WGC FrameArrived.
    frame = nullptr;

    if (!copied) {
      release_reserved_scratch_texture(*scratch_index);
      return;
    }

    if (!enqueue_scratch_texture(*scratch_index, frame_qpc)) {
      return;
    }
    _delivery_cv.notify_one();
  }

  void delivery_thread_proc(std::stop_token stop_token) noexcept {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);

    DWORD task_idx = 0;
    safe_mmcss_handle mmcss_handle = nullptr;
    HANDLE raw_mmcss_handle = AvSetMmThreadCharacteristicsW(L"Pro Audio", &task_idx);
    if (!raw_mmcss_handle) {
      raw_mmcss_handle = AvSetMmThreadCharacteristicsW(L"Games", &task_idx);
    }
    if (raw_mmcss_handle) {
      mmcss_handle.reset(raw_mmcss_handle);
      AvSetMmThreadPriority(mmcss_handle.get(), AVRT_PRIORITY_HIGH);
    } else {
      BOOST_LOG(warning) << "Failed to set WGC delivery thread MMCSS characteristics: " << GetLastError();
    }

    for (;;) {
      std::optional<delivery_frame_t> frame;
      {
        std::unique_lock lock(_delivery_mutex);
        _delivery_cv.wait(lock, [&]() {
          return _delivery_stop || stop_token.stop_requested() || _pending_delivery_frame.has_value();
        });

        if (_delivery_stop || stop_token.stop_requested()) {
          break;
        }

        frame = std::move(_pending_delivery_frame);
        _pending_delivery_frame.reset();
        if (frame && frame->scratch_index < _scratch_textures.size()) {
          _scratch_textures[frame->scratch_index].state = scratch_state_e::delivering;
        }
      }
      _delivery_cv.notify_all();

      if (!frame || !frame->texture) {
        continue;
      }

      try {
        copy_frame_to_shared_texture(frame->texture, frame->frame_qpc);
      } catch (const winrt::hresult_error &ex) {
        BOOST_LOG(error) << "WinRT error in WGC delivery thread: " << ex.code() << " - " << winrt::to_string(ex.message());
      } catch (...) {
        BOOST_LOG(error) << "Unknown error in WGC delivery thread";
      }

      {
        std::lock_guard lock(_delivery_mutex);
        if (frame->scratch_index < _scratch_textures.size() &&
            _scratch_textures[frame->scratch_index].state == scratch_state_e::delivering) {
          _scratch_textures[frame->scratch_index].state = scratch_state_e::free;
        }
      }
    }
  }

  /**
   * @brief Copies the captured texture to the shared texture and signals the main process.
   * @param frame_tex The captured D3D11 texture.
   * @param frame_qpc The QPC timestamp from when the frame was captured.
   */
  void copy_frame_to_shared_texture(const winrt::com_ptr<ID3D11Texture2D> &frame_tex, uint64_t frame_qpc) {
    if (!_deps || !frame_tex) {
      return;
    }

    // Take the helper-local D3D context lock before the cross-process keyed
    // mutex. The callback thread also uses this context for scratch copies; if
    // GPU load makes that copy slow, waiting here must not block Sunshine from
    // acquiring the shared WGC texture.
    const auto context_wait_start = std::chrono::steady_clock::now();
    std::unique_lock context_lock(_d3d_context_mutex);
    const auto context_wait = std::chrono::steady_clock::now() - context_wait_start;

    const auto mutex_wait_start = std::chrono::steady_clock::now();
    HRESULT hr = _deps->resource_manager.get_keyed_mutex()->AcquireSync(0, 200);
    const auto mutex_wait = std::chrono::steady_clock::now() - mutex_wait_start;
    if (hr == WAIT_TIMEOUT) {
      const auto slow_mutex_count = _slow_mutex_waits.fetch_add(1, std::memory_order_relaxed) + 1;
      if (slow_mutex_count <= 5 || slow_mutex_count % 120 == 0) {
        BOOST_LOG(debug) << "Timed out acquiring keyed mutex for WGC frame copy; dropping frame"
                         << " (count=" << slow_mutex_count << ")";
      }
      return;
    }
    if (hr != S_OK && hr != WAIT_ABANDONED) {
      BOOST_LOG(error) << "Failed to acquire mutex key 0: " << std::format(": 0x{:08X}", hr);
      return;
    }
    if (hr == WAIT_ABANDONED) {
      BOOST_LOG(error) << "Keyed mutex was abandoned; continuing with lock held";
    }

    const auto shared_mutex_hold_start = std::chrono::steady_clock::now();
    auto release_shared_mutex = util::fail_guard([&]() {
      const HRESULT rel_hr = _deps->resource_manager.get_keyed_mutex()->ReleaseSync(0);
      if (FAILED(rel_hr)) {
        BOOST_LOG(warning) << "Failed to release mutex key 0: " << std::format(": 0x{:08X}", rel_hr);
      }
    });

    // Copy frame data while holding the keyed mutex. The main process acquires
    // the same mutex before snapshotting this shared texture into a pool-owned frame.
    const auto copy_start = std::chrono::steady_clock::now();
    _deps->d3d_context->CopyResource(_deps->resource_manager.get_shared_texture().get(), frame_tex.get());
    const auto copy_submit = std::chrono::steady_clock::now() - copy_start;

    // Publish the metadata while still holding the shared keyed mutex so the
    // consumer can never lock a newer texture while reading an older frame id.
    _deps->resource_manager.publish_frame_metadata(frame_qpc);

    const HRESULT rel_hr = _deps->resource_manager.get_keyed_mutex()->ReleaseSync(0);
    release_shared_mutex.disable();
    if (FAILED(rel_hr)) {
      BOOST_LOG(warning) << "Failed to release mutex key 0: " << std::format(": 0x{:08X}", rel_hr);
      return;
    }
    const auto shared_mutex_hold = std::chrono::steady_clock::now() - shared_mutex_hold_start;
    _keyed_mutex_wait_timer.collect(mutex_wait);
    _publish_copy_timer.collect(shared_mutex_hold);
    context_lock.unlock();

    // Signal only after releasing the mutex so a woken consumer can acquire the
    // frame without waiting on the producer's normal release path.
    _deps->resource_manager.signal_frame_ready();
    _published_frames.fetch_add(1, std::memory_order_relaxed);

    const auto context_wait_ms = std::chrono::duration<double, std::milli>(context_wait).count();
    const auto mutex_wait_ms = std::chrono::duration<double, std::milli>(mutex_wait).count();
    const auto shared_mutex_hold_ms = std::chrono::duration<double, std::milli>(shared_mutex_hold).count();
    const auto copy_submit_ms = std::chrono::duration<double, std::milli>(copy_submit).count();
    const bool slow_context = context_wait_ms > 1.0;
    const bool slow_mutex = mutex_wait_ms > 1.0;
    const bool slow_shared_hold = shared_mutex_hold_ms > 1.0;
    const bool slow_copy = copy_submit_ms > 1.0;
    if (slow_context || slow_mutex || slow_shared_hold || slow_copy) {
      const auto slow_context_count = slow_context ? (_slow_context_waits.fetch_add(1, std::memory_order_relaxed) + 1) : _slow_context_waits.load(std::memory_order_relaxed);
      const auto slow_mutex_count = slow_mutex ? (_slow_mutex_waits.fetch_add(1, std::memory_order_relaxed) + 1) : _slow_mutex_waits.load(std::memory_order_relaxed);
      const auto slow_hold_count = slow_shared_hold ? (_slow_shared_mutex_holds.fetch_add(1, std::memory_order_relaxed) + 1) : _slow_shared_mutex_holds.load(std::memory_order_relaxed);
      const auto slow_copy_count = slow_copy ? (_slow_copy_submissions.fetch_add(1, std::memory_order_relaxed) + 1) : _slow_copy_submissions.load(std::memory_order_relaxed);
      const bool should_log_slow_context = slow_context && (slow_context_count <= 5 || slow_context_count % 120 == 0);
      const bool should_log_slow_mutex = slow_mutex && (slow_mutex_count <= 5 || slow_mutex_count % 120 == 0);
      const bool should_log_slow_hold = slow_shared_hold && (slow_hold_count <= 5 || slow_hold_count % 120 == 0);
      const bool should_log_slow_copy = slow_copy && (slow_copy_count <= 5 || slow_copy_count % 120 == 0);
      if (should_log_slow_context || should_log_slow_mutex || should_log_slow_hold || should_log_slow_copy) {
        BOOST_LOG(debug) << "WGC helper copy timing: context_wait_ms=" << context_wait_ms
                         << " mutex_wait_ms=" << mutex_wait_ms
                         << " shared_mutex_hold_ms=" << shared_mutex_hold_ms
                         << " copy_submit_ms=" << copy_submit_ms
                         << " slow_context_count=" << slow_context_count
                         << " slow_mutex_count=" << slow_mutex_count
                         << " slow_shared_hold_count=" << slow_hold_count
                         << " slow_copy_count=" << slow_copy_count;
      }
    }

    // Log first frame and frame 100 for quick sanity checks, but avoid long-running spam.
    static std::atomic<uint64_t> frame_count {0};
    const auto count = ++frame_count;
    if (count == 1 || count == 100) {
      BOOST_LOG(info) << "Published frame " << count << " to main process";
    }
  }

public:
  /**
   * @brief Creates a capture session for the specified capture item.
   * @returns true if the session was created successfully, false otherwise.
   */
  bool create_capture_session() {
    if (!_frame_pool) {
      return false;
    }

    if (!_deps || !_deps->graphics_item) {
      BOOST_LOG(error) << "Cannot create capture session: missing capture item";
      return false;
    }

    _frame_arrived_token = _frame_pool.FrameArrived([this](Direct3D11CaptureFramePool const &sender, winrt::Windows::Foundation::IInspectable const &) {
      if (_shutting_down.load(std::memory_order_acquire)) {
        return;
      }

      struct outstanding_guard_t {
        std::atomic<int> &counter;
        const int value;

        explicit outstanding_guard_t(std::atomic<int> &c):
            counter(c),
            value(c.fetch_add(1, std::memory_order_acq_rel) + 1) {}

        ~outstanding_guard_t() {
          counter.fetch_sub(1, std::memory_order_acq_rel);
        }
      } guard {_outstanding_frames};

      int prev_peak = _peak_outstanding.load(std::memory_order_relaxed);
      while (guard.value > prev_peak &&
             !_peak_outstanding.compare_exchange_weak(prev_peak, guard.value, std::memory_order_release, std::memory_order_relaxed)) {
      }

      process_frame(sender);
    });

    try {
      _capture_session = _frame_pool.CreateCaptureSession(_deps->graphics_item);
    } catch (const winrt::hresult_error &ex) {
      BOOST_LOG(error) << "CreateCaptureSession threw: " << ex.code() << " - " << winrt::to_string(ex.message());
      return false;
    } catch (...) {
      BOOST_LOG(error) << "CreateCaptureSession threw an unknown exception";
      return false;
    }

    if (!_capture_session) {
      BOOST_LOG(error) << "Failed to create GraphicsCaptureSession (returned null)";
      return false;
    }

    configure_capture_session(_capture_session, true);
    return true;
  }

  /**
   * @brief Composite the app's secondary windows over the target from now on.
   * Call once before capture starts.
   */
  void enable_secondary_layers() {
    _layer_mode = _window_mode;
    if (_layer_mode && !_recomposite_thread.joinable()) {
      _recomposite_thread = std::thread([this]() {
        recomposite_thread_proc();
      });
    }
  }

  /**
   * @brief The capture output in desktop coordinates, sized from the shared texture.
   */
  platf::dxgi::window_policy::rect_i output_rect() const {
    return {
      _monitor_rect.left,
      _monitor_rect.top,
      _monitor_rect.left + static_cast<LONG>(_width),
      _monitor_rect.top + static_cast<LONG>(_height),
    };
  }

  DXGI_FORMAT capture_format() const {
    return _capture_format;
  }

  DirectXPixelFormat pixel_format() const {
    return (_capture_format == DXGI_FORMAT_R16G16B16A16_FLOAT) ? DirectXPixelFormat::R16G16B16A16Float : DirectXPixelFormat::B8G8R8A8UIntNormalized;
  }

  /**
   * @brief Replace the secondary layers drawn over the target (bottom-up) and recomposite.
   */
  void set_secondary_layers(std::vector<PublishedLayer> layers) {
    if (!_layer_mode) {
      return;
    }
    {
      std::lock_guard lock(_layers_mutex);
      _published_layers = std::move(layers);
      _layers_published.store(!_published_layers.empty(), std::memory_order_release);
      _layers_generation.fetch_add(1, std::memory_order_acq_rel);
    }
    request_recomposite();
  }

  /**
   * @brief Ask the recomposite thread for a new frame from the cached target and layers.
   * Never blocks on the D3D context.
   */
  void request_recomposite() {
    if (!_layer_mode) {
      return;
    }
    {
      std::lock_guard lock(_recomposite_mutex);
      _recomposite_pending.store(true, std::memory_order_release);
    }
    _recomposite_cv.notify_one();
  }

  /**
   * @brief Cache a new frame of a secondary window. Called from the layer's FrameArrived handler.
   */
  void on_layer_frame(const std::shared_ptr<SecondaryLayer> &layer, Direct3D11CaptureFramePool const &sender) {
    // Counted before the shutdown check, so shutdown() either sees this handler or the handler sees shutdown.
    struct callback_guard_t {
      std::atomic<int> &counter;

      explicit callback_guard_t(std::atomic<int> &c):
          counter(c) {
        counter.fetch_add(1, std::memory_order_acq_rel);
      }

      ~callback_guard_t() {
        counter.fetch_sub(1, std::memory_order_acq_rel);
      }
    } guard {_layer_callbacks};

    if (_shutting_down.load(std::memory_order_acquire) || !layer || layer->closed.load(std::memory_order_acquire)) {
      return;
    }

    Direct3D11CaptureFrame frame = nullptr;
    try {
      while (auto next_frame = sender.TryGetNextFrame()) {
        frame = std::move(next_frame);
      }
    } catch (const winrt::hresult_error &ex) {
      BOOST_LOG(debug) << "WinRT error retrieving a secondary window frame: " << ex.code();
      return;
    }

    if (!frame) {
      grow_layer_pool(*layer, sender);
      return;
    }

    SizeInt32 content_size {0, 0};
    winrt::com_ptr<ID3D11Texture2D> frame_tex;
    try {
      content_size = frame.ContentSize();
      auto surface = frame.Surface();
      winrt::com_ptr<IDirect3DDxgiInterfaceAccess> ia;
      if (FAILED(winrt::get_unknown(surface)->QueryInterface(__uuidof(IDirect3DDxgiInterfaceAccess), ia.put_void())) ||
          FAILED(ia->GetInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<::IUnknown **>(frame_tex.put_void())))) {
        return;
      }
    } catch (const winrt::hresult_error &ex) {
      BOOST_LOG(debug) << "WinRT error reading a secondary window frame: " << ex.code();
      return;
    }

    bool cached = false;
    {
      std::lock_guard context_lock(_d3d_context_mutex);
      cached = cache_layer_frame(*layer, frame_tex, content_size);
    }
    frame_tex = nullptr;
    frame = nullptr;

    if (cached) {
      const auto now = qpc_now();
      layer->frame_qpc.store(now, std::memory_order_release);
      if (!layer->first_frame_logged.exchange(true, std::memory_order_acq_rel)) {
        const auto delay_ms = (now - layer->created_qpc) * 1000 / qpc_frequency();
        BOOST_LOG(info) << "Secondary window 0x" << std::hex << reinterpret_cast<std::uintptr_t>(layer->hwnd) << std::dec
                        << " first frame after " << delay_ms << " ms";
      }
      // A hidden layer's content is only drawn once it shows again; that show recomposites.
      if (layer->published.load(std::memory_order_acquire)) {
        request_recomposite();
      }
    }

    resize_layer_pool(*layer, sender, content_size);
  }

  /**
   * @brief Publish one opaque black frame to the main process.
   *
   * Window-only sessions use this before the first window frame and when no target
   * window exists, so the stream never falls back to showing the desktop.
   * @return true if the frame was queued for delivery.
   */
  bool publish_black_frame() {
    if (!_deps) {
      return false;
    }

    auto scratch_index = reserve_scratch_texture();
    if (!scratch_index) {
      return false;
    }

    if (!ensure_scratch_texture(*scratch_index)) {
      release_reserved_scratch_texture(*scratch_index);
      return false;
    }

    bool cleared = false;
    {
      std::lock_guard context_lock(_d3d_context_mutex);
      cleared = clear_scratch_texture(_scratch_textures[*scratch_index]);
    }
    if (!cleared) {
      release_reserved_scratch_texture(*scratch_index);
      return false;
    }

    LARGE_INTEGER qpc {};
    QueryPerformanceCounter(&qpc);
    if (!enqueue_scratch_texture(*scratch_index, static_cast<uint64_t>(qpc.QuadPart))) {
      return false;
    }
    _delivery_cv.notify_one();
    return true;
  }

  /**
   * @brief Starts the capture session if available.
   */
  void start_capture() const {
    if (_capture_session) {
      _capture_session.StartCapture();
      BOOST_LOG(info) << "Helper process started. Capturing frames using WGC...";
    }
  }

private:
  void check_and_adjust_frame_buffer() {
    auto now = std::chrono::steady_clock::now();

    // Check every 1 second for buffer adjustments
    if (auto time_since_last_check = std::chrono::duration_cast<std::chrono::milliseconds>(now - _last_buffer_check);
        time_since_last_check.count() < 1000) {
      return;
    }

    _last_buffer_check = now;

    // 1) Prune old drop timestamps (older than 5 seconds)
    prune_old_drop_timestamps(now);

    // 2) Try to increase buffer count if we have recent drops
    if (try_increase_buffer_size(now)) {
      maybe_log_capture_diagnostics(now);
      return;
    }

    // 3) Try to decrease buffer count if we've been quiet
    try_decrease_buffer_size(now);
    maybe_log_capture_diagnostics(now);
  }
};

/**
 * @brief Window events seen since the layer manager last scanned the app's windows.
 * Written by the WinEvent callback (main thread) and by capture item Closed handlers.
 */
struct layer_events_t {
  std::atomic<bool> dirty {false};
  std::atomic<int64_t> dirty_since_qpc {0};  ///< First event since the last scan.
  // Main thread only (the WinEvent callback runs on the thread that installed the hooks).
  std::array<HWND, 64> destroyed {};
  size_t destroyed_count = 0;
};

layer_events_t &layer_events() {
  static auto *events = new layer_events_t {};
  return *events;
}

void mark_layers_dirty() {
  auto &events = layer_events();
  int64_t expected = 0;
  events.dirty_since_qpc.compare_exchange_strong(expected, qpc_now(), std::memory_order_acq_rel);
  events.dirty.store(true, std::memory_order_release);
}

/**
 * @brief WinEvent callback for the app's secondary windows. Only marks the window state dirty
 * (and remembers destroyed windows); no window queries and no WinRT calls here.
 */
void CALLBACK layer_event_proc(HWINEVENTHOOK /*hook*/, DWORD event, HWND hwnd, LONG id_object, LONG id_child, DWORD /*event_thread*/, DWORD /*event_time*/) {
  // Object events also fire for the cursor, carets and child objects; only whole windows
  // matter. System events (foreground, menu popups, minimize) always concern a window.
  const bool object_event = event >= EVENT_OBJECT_CREATE && event <= EVENT_OBJECT_END;
  if (object_event && (hwnd == nullptr || id_object != OBJID_WINDOW || id_child != CHILDID_SELF)) {
    return;
  }

  if (event == EVENT_OBJECT_DESTROY) {
    auto &events = layer_events();
    if (events.destroyed_count < events.destroyed.size()) {
      events.destroyed[events.destroyed_count] = hwnd;
    }
    ++events.destroyed_count;
  }
  mark_layers_dirty();
}

/**
 * @brief Shows the app's secondary windows (menus, dropdowns, tooltips, context menus,
 * dialogs) in a window-only stream. Runs on the helper's main thread.
 *
 * Each visible top-level window of a process in the app window set that lies on the capture
 * display gets its own WGC window session; the capture manager draws the cached frames over
 * the target in z-order. Windows of any other process are never captured. Sessions of windows
 * that hide but stay alive are kept, so a reused HWND (Qt reuses menu windows) shows again
 * without a new session. A closed or failed layer drops only that layer.
 */
class WindowLayerManager {
public:
  static constexpr size_t MAX_LAYER_SESSIONS = 23;  ///< 24 WGC sessions in total with the target's.
  static constexpr auto SCAN_INTERVAL = std::chrono::milliseconds(100);  ///< Safety net for lost events.
  static constexpr auto MIN_EVENT_SCAN_INTERVAL = std::chrono::milliseconds(4);  ///< Coalesces event bursts.
  static constexpr auto CREATE_RETRY_INTERVAL = std::chrono::milliseconds(500);

  WindowLayerManager(WgcCaptureManager &capture, IDirect3DDevice device, HWND target):
      _capture(capture),
      _device(std::move(device)),
      _target(target),
      _output(capture.output_rect()) {
    GetWindowThreadProcessId(_target, &_target_window_pid);
  }

  WindowLayerManager(const WindowLayerManager &) = delete;
  WindowLayerManager &operator=(const WindowLayerManager &) = delete;

  ~WindowLayerManager() noexcept {
    for (HWINEVENTHOOK hook : _hooks) {
      UnhookWinEvent(hook);
    }
    _hooks.clear();
    set_location_hook(false);
    for (auto &session : _sessions) {
      close_session(session);
    }
    _sessions.clear();
    try {
      _capture.set_secondary_layers({});
    } catch (...) {
    }
  }

  /**
   * @brief Install the window event hooks and scan once.
   */
  void start() {
    // Events of all processes: membership is decided per window in update().
    const std::array<std::pair<DWORD, DWORD>, 4> ranges {{
      {EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_MENUPOPUPEND},  // foreground, menu popups start and end
      {EVENT_SYSTEM_MINIMIZESTART, EVENT_SYSTEM_MINIMIZEEND},
      {EVENT_OBJECT_DESTROY, EVENT_OBJECT_REORDER},  // destroy, show, hide, reorder
      {EVENT_OBJECT_CLOAKED, EVENT_OBJECT_UNCLOAKED},
    }};
    for (const auto &[first, last] : ranges) {
      if (HWINEVENTHOOK hook = add_hook(first, last, 0)) {
        _hooks.push_back(hook);
      }
    }
    BOOST_LOG(info) << "Window-only secondary windows enabled (" << _hooks.size() << " event hooks)";

    _generation = g_window_set_generation.load(std::memory_order_acquire);
    update(qpc_now());
  }

  /**
   * @brief Scan when events arrived, the window set changed or the safety interval passed.
   * Called on every main loop iteration; compositing happens on the capture manager's threads.
   * @return How long the main loop may wait for messages or the wake event before the next
   * call is due, in milliseconds. Capped at 16 ms, the default timer resolution, so the
   * loop's other checks run as often as with the plain loop.
   */
  DWORD tick() {
    const auto now = qpc_now();
    const auto generation = g_window_set_generation.load(std::memory_order_acquire);
    const bool dirty = layer_events().dirty.load(std::memory_order_acquire);
    if (generation != _generation || now - _last_scan_qpc >= qpc_ticks(SCAN_INTERVAL) ||
        (dirty && now - _last_scan_qpc >= qpc_ticks(MIN_EVENT_SCAN_INTERVAL))) {
      _generation = generation;
      update(now);
    }

    if (_reuse_deadline_qpc != 0 && now >= _reuse_deadline_qpc) {
      _reuse_deadline_qpc = 0;
      _capture.request_recomposite();
    }

    auto next = _last_scan_qpc + qpc_ticks(SCAN_INTERVAL);
    if (layer_events().dirty.load(std::memory_order_acquire)) {
      next = std::min(next, _last_scan_qpc + qpc_ticks(MIN_EVENT_SCAN_INTERVAL));
    }
    if (_reuse_deadline_qpc != 0) {
      next = std::min(next, _reuse_deadline_qpc);
    }
    // Rounded up, so the loop never spins with a 0 ms wait before a deadline.
    const auto delta = next - qpc_now();
    const auto frequency = qpc_frequency();
    const auto wait_ms = delta <= 0 ? int64_t {0} : std::min<int64_t>((delta * 1000 + frequency - 1) / frequency, 16);
    return static_cast<DWORD>(wait_ms);
  }

private:
  struct session_t {
    std::shared_ptr<SecondaryLayer> layer;
    GraphicsCaptureItem item {nullptr};
    Direct3D11CaptureFramePool pool {nullptr};
    GraphicsCaptureSession session {nullptr};
    winrt::event_token frame_token {};
    winrt::event_token closed_token {};
    bool visible = false;
    int64_t last_shown_qpc = 0;
    RECT bounds {};
  };

  struct enum_context_t {
    HWND target;
    HWND foreground;
    platf::dxgi::window_policy::rect_i target_client;
    const std::vector<uint32_t> &pids;
    std::vector<platf::dxgi::window_policy::app_window_t> &windows;
    std::unordered_set<std::uintptr_t> &logged_see_through;
  };

  static bool window_class_is(HWND hwnd, const wchar_t *expected) {
    wchar_t class_name[64] {};
    return GetClassNameW(hwnd, class_name, static_cast<int>(std::size(class_name))) > 0 && wcscmp(class_name, expected) == 0;
  }

  static BOOL CALLBACK enum_app_window(HWND hwnd, LPARAM param) {
    auto &context = *reinterpret_cast<enum_context_t *>(param);
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    const bool is_target = hwnd == context.target;
    // The process check comes first; other windows are never queried further. Windows are
    // matched by the process that owns the HWND only: UWP frame windows belong to the frame
    // host, which is never in the set, so they never become layers.
    if (!is_target && !std::binary_search(context.pids.begin(), context.pids.end(), static_cast<uint32_t>(pid))) {
      return TRUE;
    }

    platf::dxgi::window_policy::app_window_t window;
    window.id = reinterpret_cast<std::uintptr_t>(hwnd);
    window.pid = pid;
    window.visible = IsWindowVisible(hwnd) != FALSE;
    if (!window.visible && !is_target) {
      return TRUE;
    }
    DWORD cloaked = 0;
    window.cloaked = SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked != 0;
    window.iconic = IsIconic(hwnd) != FALSE;
    window.root = GetAncestor(hwnd, GA_ROOT) == hwnd;
    window.shadow = window_class_is(hwnd, L"SysShadow");
    RECT bounds {};
    if (SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &bounds, sizeof(bounds))) ||
        GetWindowRect(hwnd, &bounds)) {
      window.bounds = {bounds.left, bounds.top, bounds.right, bounds.bottom};
    }
    if (const auto ex_style = GetWindowLongPtrW(hwnd, GWL_EXSTYLE); (ex_style & WS_EX_LAYERED) != 0) {
      BYTE alpha = 255;
      DWORD layered_flags = 0;
      // Fails for windows drawn with UpdateLayeredWindow, which have per-pixel alpha.
      const bool has_attributes = GetLayeredWindowAttributes(hwnd, nullptr, &alpha, &layered_flags) != FALSE;
      window.transparent = has_attributes && (layered_flags & LWA_ALPHA) != 0 && alpha == 0;
      window.click_through = (ex_style & WS_EX_TRANSPARENT) != 0;
      if (!is_target) {
        const auto style = GetWindowLongPtrW(hwnd, GWL_STYLE);
        window.see_through_cover = platf::dxgi::window_policy::large_see_through_window(
          !has_attributes,
          (style & WS_CAPTION) == WS_CAPTION,
          GetWindow(hwnd, GW_OWNER) != nullptr,
          hwnd == context.foreground,
          window.bounds,
          context.target_client
        );
        if (window.see_through_cover && window.visible && context.logged_see_through.insert(window.id).second) {
          BOOST_LOG(info) << "Window-only: not drawing a large see-through window 0x" << std::hex << window.id << std::dec
                          << ' ' << describe(hwnd, pid);
        }
      }
    }
    context.windows.push_back(window);
    return TRUE;
  }

  static HWINEVENTHOOK add_hook(const DWORD first, const DWORD last, const DWORD pid) {
    HWINEVENTHOOK hook = SetWinEventHook(first, last, nullptr, layer_event_proc, pid, 0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    if (!hook) {
      BOOST_LOG(warning) << "Failed to install a window event hook (0x" << std::hex << first << std::dec << "): " << GetLastError();
    }
    return hook;
  }

  /**
   * @brief Location changes also fire for every cursor and caret move, so they are taken from
   * the target's process only and only while a layer is shown (a moved target or popup must
   * then recomposite). Moves in other app processes are found by the periodic scan.
   */
  void set_location_hook(const bool enabled) {
    if (enabled && !_location_hook && _target_window_pid != 0) {
      _location_hook = add_hook(EVENT_OBJECT_LOCATIONCHANGE, EVENT_OBJECT_LOCATIONCHANGE, _target_window_pid);
    } else if (!enabled && _location_hook) {
      UnhookWinEvent(_location_hook);
      _location_hook = nullptr;
    }
  }

  session_t *find_session(HWND hwnd) {
    for (auto &session : _sessions) {
      if (session.layer->hwnd == hwnd) {
        return &session;
      }
    }
    return nullptr;
  }

  static std::string describe(HWND hwnd, const DWORD pid) {
    std::string executable = "unknown";
    if (HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)) {
      std::wstring path(MAX_PATH * 4, L'\0');
      DWORD size = static_cast<DWORD>(path.size());
      if (QueryFullProcessImageNameW(process, 0, path.data(), &size)) {
        path.resize(size);
        executable = wide_to_utf8(std::filesystem::path(path).filename().wstring());
      }
      CloseHandle(process);
    }
    wchar_t class_name[256] {};
    GetClassNameW(hwnd, class_name, static_cast<int>(std::size(class_name)));
    // Titles are deliberately not logged; they can contain document names.
    return "pid=" + std::to_string(pid) + " exe=" + executable + " class=" + wide_to_utf8(class_name);
  }

  /**
   * @brief Start a WGC session for a secondary window. Returns null when the window cannot be captured.
   */
  session_t *create_session(HWND hwnd, const DWORD content_pid, const int64_t now) {
    if (const auto failed = _failed.find(hwnd); failed != _failed.end()) {
      if (now - failed->second < qpc_ticks(CREATE_RETRY_INTERVAL)) {
        return nullptr;
      }
      _failed.erase(failed);
    }

    session_t session;
    session.layer = std::make_shared<SecondaryLayer>();
    auto &layer = *session.layer;
    layer.hwnd = hwnd;
    layer.content_pid = content_pid;  // the window's own process: UWP frame windows are never layers
    layer.created_qpc = now;
    GetWindowThreadProcessId(hwnd, &layer.window_pid);

    try {
      auto factory = winrt::get_activation_factory<GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
      const HRESULT hr = factory->CreateForWindow(hwnd, winrt::guid_of<GraphicsCaptureItem>(), winrt::put_abi(session.item));
      if (FAILED(hr)) {
        winrt::throw_hresult(hr);
      }
      if (!session.item) {
        winrt::throw_hresult(E_FAIL);
      }

      const auto item_size = session.item.Size();
      layer.pool_size = SizeInt32 {std::max(1, item_size.Width), std::max(1, item_size.Height)};
      session.pool = Direct3D11CaptureFramePool::CreateFreeThreaded(_device, _capture.pixel_format(), layer.pool_buffers, layer.pool_size);

      auto *capture = &_capture;
      std::shared_ptr<SecondaryLayer> shared_layer = session.layer;
      session.frame_token = session.pool.FrameArrived([capture, shared_layer](Direct3D11CaptureFramePool const &sender, winrt::Windows::Foundation::IInspectable const &) {
        capture->on_layer_frame(shared_layer, sender);
      });
      // A closed item drops this layer only; unlike the target it never ends the helper.
      session.closed_token = session.item.Closed([shared_layer](GraphicsCaptureItem const &, winrt::Windows::Foundation::IInspectable const &) {
        shared_layer->closed.store(true, std::memory_order_release);
        mark_layers_dirty();
        SetEvent(layer_wake_event());
      });

      session.session = session.pool.CreateCaptureSession(session.item);
      configure_capture_session(session.session, false);
      session.session.StartCapture();
    } catch (const winrt::hresult_error &ex) {
      BOOST_LOG(debug) << "Window-only: cannot capture secondary window 0x" << std::hex << reinterpret_cast<std::uintptr_t>(hwnd)
                       << std::dec << " (" << ex.code() << ")";
      close_session(session);
      _failed[hwnd] = now;
      if (_failed.size() > 256) {
        _failed.clear();
      }
      return nullptr;
    }

    BOOST_LOG(debug) << "Window-only: secondary window 0x" << std::hex << reinterpret_cast<std::uintptr_t>(hwnd) << std::dec
                     << ' ' << describe(hwnd, content_pid);
    _sessions.push_back(std::move(session));
    return &_sessions.back();
  }

  static void close_session(session_t &session) noexcept {
    if (session.layer) {
      session.layer->closed.store(true, std::memory_order_release);
    }
    try {
      if (session.session) {
        session.session.Close();
        session.session = nullptr;
      }
    } catch (...) {
    }
    try {
      if (session.pool) {
        if (session.frame_token.value != 0) {
          session.pool.FrameArrived(session.frame_token);
          session.frame_token.value = 0;
        }
        session.pool.Close();
        session.pool = nullptr;
      }
    } catch (...) {
    }
    try {
      if (session.item && session.closed_token.value != 0) {
        session.item.Closed(session.closed_token);
        session.closed_token.value = 0;
      }
      session.item = nullptr;
    } catch (...) {
    }
  }

  void drop_sessions_if(const std::function<bool(const session_t &)> &predicate) {
    for (auto it = _sessions.begin(); it != _sessions.end();) {
      if (predicate(*it)) {
        close_session(*it);
        it = _sessions.erase(it);
      } else {
        ++it;
      }
    }
  }

  /**
   * @brief Rescan the app's windows, start and stop layer sessions and publish the layers.
   */
  void update(const int64_t now) {
    _last_scan_qpc = now;
    auto &events = layer_events();
    events.dirty.store(false, std::memory_order_release);
    const auto event_qpc = events.dirty_since_qpc.exchange(0, std::memory_order_acq_rel);
    const auto shown_at = event_qpc != 0 ? event_qpc : now;

    // Destroyed windows lose their layer at once. On overflow the IsWindow check below finds them.
    const auto destroyed_count = std::min(events.destroyed_count, events.destroyed.size());
    events.destroyed_count = 0;
    if (destroyed_count > 0) {
      const auto destroyed_end = events.destroyed.begin() + static_cast<std::ptrdiff_t>(destroyed_count);
      drop_sessions_if([&](const session_t &session) {
        return std::find(events.destroyed.begin(), destroyed_end, session.layer->hwnd) != destroyed_end;
      });
    }

    refresh_window_set_pids(reinterpret_cast<std::uintptr_t>(_target), _pids, _pids_generation);
    const auto &pids = _pids;

    // The target's client area on the desktop; empty while it is hidden or minimized.
    platf::dxgi::window_policy::rect_i target_client {0, 0, 0, 0};
    if (RECT client {}; IsWindowVisible(_target) && !IsIconic(_target) && GetClientRect(_target, &client)) {
      POINT origin {0, 0};
      if (ClientToScreen(_target, &origin)) {
        target_client = {origin.x, origin.y, origin.x + client.right, origin.y + client.bottom};
      }
    }

    _windows.clear();
    if (!pids.empty()) {
      enum_context_t context {_target, GetForegroundWindow(), target_client, pids, _windows, _logged_see_through};
      EnumWindows(enum_app_window, reinterpret_cast<LPARAM>(&context));
    }

    std::array<platf::dxgi::window_policy::layer_t, platf::dxgi::window_policy::max_secondary_layers + 1> planned {};
    const auto count = platf::dxgi::window_policy::plan_layers(
      _windows,
      reinterpret_cast<std::uintptr_t>(_target),
      pids,
      _output,
      planned
    );

    if (!_cap_warned) {
      const auto qualifying = std::count_if(_windows.begin(), _windows.end(), [&](const auto &window) {
        return platf::dxgi::window_policy::is_secondary_window(window, reinterpret_cast<std::uintptr_t>(_target), pids, _output);
      });
      if (static_cast<size_t>(qualifying) > platf::dxgi::window_policy::max_secondary_layers) {
        _cap_warned = true;
        BOOST_LOG(warning) << "Window-only: " << qualifying << " secondary windows are visible; only the topmost "
                           << platf::dxgi::window_policy::max_secondary_layers << " are shown";
      }
    }

    // Pass 1: the planned windows, bottom-up, and the sessions they already have. Those
    // sessions are marked visible first, so making room for new sessions in pass 2 never
    // evicts a window shown in this scan.
    struct entry_t {
      HWND hwnd;
      const platf::dxgi::window_policy::app_window_t *window;
      bool below_target;
      session_t *session;
    };
    std::array<entry_t, platf::dxgi::window_policy::max_secondary_layers + 1> entries {};
    size_t entry_count = 0;
    for (auto &session : _sessions) {
      session.visible = false;
    }
    for (size_t i = 0; i < count; ++i) {
      const auto &planned_layer = planned[i];
      if (planned_layer.is_target) {
        continue;
      }
      const auto window = std::find_if(_windows.begin(), _windows.end(), [&](const auto &w) {
        return w.id == planned_layer.id;
      });
      // A window below the target whose part on the display lies under the target's client
      // area is fully hidden by it; it gets no layer, so a game with a window behind it keeps
      // the single-window composite.
      if (window == _windows.end() ||
          (planned_layer.below_target && platf::dxgi::window_policy::hidden_below_target(_output, window->bounds, target_client))) {
        continue;
      }
      const auto hwnd = reinterpret_cast<HWND>(planned_layer.id);
      auto *session = find_session(hwnd);
      if (session && session->layer->closed.load(std::memory_order_acquire)) {
        continue;  // dropped below; a new session starts on the next scan
      }
      if (session) {
        session->visible = true;
      }
      entries[entry_count++] = entry_t {hwnd, &*window, planned_layer.below_target, session};
    }

    // Pass 2: start sessions for newly shown windows, evicting hidden ones when at the cap.
    for (size_t i = 0; i < entry_count; ++i) {
      auto &entry = entries[i];
      if (entry.session) {
        continue;
      }
      if (_sessions.size() >= MAX_LAYER_SESSIONS) {
        drop_least_recently_shown();
      }
      if (_sessions.size() >= MAX_LAYER_SESSIONS) {
        continue;
      }
      entry.session = create_session(entry.hwnd, entry.window->pid, now);
      if (entry.session) {
        entry.session->visible = true;
      }
    }

    // Pass 3: publish bottom-up.
    std::vector<PublishedLayer> published;
    published.reserve(entry_count);
    bool bounds_changed = false;
    int64_t reuse_deadline = 0;
    for (size_t i = 0; i < entry_count; ++i) {
      const auto &entry = entries[i];
      auto *session = entry.session;
      if (!session) {
        continue;
      }
      if (session->last_shown_qpc == 0 || !_was_visible.contains(reinterpret_cast<std::uintptr_t>(entry.hwnd))) {
        session->layer->shown_qpc.store(shown_at, std::memory_order_release);
      }
      session->last_shown_qpc = now;
      const RECT bounds {entry.window->bounds.left, entry.window->bounds.top, entry.window->bounds.right, entry.window->bounds.bottom};
      if (!EqualRect(&bounds, &session->bounds)) {
        session->bounds = bounds;
        bounds_changed = true;
      }
      const auto layer_shown = session->layer->shown_qpc.load(std::memory_order_acquire);
      const auto layer_frame = session->layer->frame_qpc.load(std::memory_order_acquire);
      if (layer_frame != 0 && layer_frame < layer_shown) {
        // A reused window: drawn from its cached frame if nothing new arrives in time.
        const auto deadline = layer_shown + qpc_ticks(SECONDARY_REUSE_DELAY);
        reuse_deadline = reuse_deadline == 0 ? deadline : std::min(reuse_deadline, deadline);
      }
      published.push_back(PublishedLayer {session->layer, entry.below_target});
    }

    _was_visible.clear();
    for (const auto &layer : published) {
      _was_visible.insert(reinterpret_cast<std::uintptr_t>(layer.layer->hwnd));
    }
    for (auto &session : _sessions) {
      session.layer->published.store(session.visible, std::memory_order_release);
    }

    // Closed layers, windows that are gone, and hidden windows whose process left the set.
    drop_sessions_if([&](const session_t &session) {
      if (session.layer->closed.load(std::memory_order_acquire) || !IsWindow(session.layer->hwnd)) {
        return true;
      }
      return !session.visible && !std::binary_search(pids.begin(), pids.end(), static_cast<uint32_t>(session.layer->window_pid));
    });

    _reuse_deadline_qpc = reuse_deadline;
    set_location_hook(!published.empty());

    std::vector<std::pair<HWND, bool>> order;
    order.reserve(published.size());
    for (const auto &layer : published) {
      order.emplace_back(layer.layer->hwnd, layer.below_target);
    }
    if (order != _published_order) {
      _published_order = std::move(order);
      _capture.set_secondary_layers(std::move(published));
    } else if (bounds_changed) {
      _capture.request_recomposite();
    }
  }

  void drop_least_recently_shown() {
    auto oldest = _sessions.end();
    for (auto it = _sessions.begin(); it != _sessions.end(); ++it) {
      if (!it->visible && (oldest == _sessions.end() || it->last_shown_qpc < oldest->last_shown_qpc)) {
        oldest = it;
      }
    }
    if (oldest != _sessions.end()) {
      close_session(*oldest);
      _sessions.erase(oldest);
    }
  }

  WgcCaptureManager &_capture;
  IDirect3DDevice _device;
  HWND _target;
  DWORD _target_window_pid = 0;
  platf::dxgi::window_policy::rect_i _output;
  std::vector<HWINEVENTHOOK> _hooks;  ///< Removed in the destructor.
  std::list<session_t> _sessions;
  std::unordered_map<HWND, int64_t> _failed;  ///< Windows whose session could not start, and when.
  std::unordered_set<std::uintptr_t> _was_visible;  ///< Layers published by the previous scan.
  std::unordered_set<std::uintptr_t> _logged_see_through;  ///< See-through windows already logged.
  std::vector<platf::dxgi::window_policy::app_window_t> _windows;  ///< Scan buffer.
  std::vector<uint32_t> _pids;  ///< App window set for the target.
  uint64_t _pids_generation = UINT64_MAX;
  HWINEVENTHOOK _location_hook = nullptr;
  std::vector<std::pair<HWND, bool>> _published_order;
  uint64_t _generation = 0;
  int64_t _last_scan_qpc = 0;
  int64_t _reuse_deadline_qpc = 0;
  bool _cap_warned = false;
};

/**
 * @brief Callback procedure for desktop switch events.
 *
 * This function handles EVENT_SYSTEM_DESKTOPSWITCH events to detect when the system
 * transitions to or from secure desktop mode (such as UAC prompts or lock screens).
 * If secure desktop is not active yet, it starts a short confirmation window so we can
 * still catch the secure-desktop transition after the event settles.
 *
 * @param h_win_event_hook Handle to the event hook (unused).
 * @param event The event type that occurred.
 * @param hwnd Handle to the window (unused).
 * @param id_object Object identifier (unused).
 * @param id_child Child object identifier (unused).
 * @param dw_event_thread Thread that generated the event (unused).
 * @param dwms_event_time Time the event occurred (unused).
 */
void CALLBACK desktop_switch_hook_proc(HWINEVENTHOOK /*h_win_event_hook*/, DWORD event, HWND /*hwnd*/, LONG /*id_object*/, LONG /*id_child*/, DWORD /*dw_event_thread*/, DWORD /*dwms_event_time*/) {
  if (event == EVENT_SYSTEM_DESKTOPSWITCH) {
    BOOST_LOG(info) << "Desktop switch detected!";

    const bool secure_desktop_active = platf::dxgi::is_secure_desktop_active();
    BOOST_LOG(info) << "Desktop switch - Secure desktop: " << (secure_desktop_active ? "YES" : "NO");

    if (secure_desktop_active) {
      notify_main_process_about_secure_desktop();
    } else if (!secure_desktop_active && g_secure_desktop_detected) {
      BOOST_LOG(info) << "Returned to normal desktop";
      g_secure_desktop_detected = false;
      g_pending_secure_desktop_check_deadline.reset();
    } else {
      g_pending_secure_desktop_check_deadline = std::chrono::steady_clock::now() + SECURE_DESKTOP_CONFIRMATION_WINDOW;
      BOOST_LOG(debug) << "Desktop switch was not immediately secure; waiting briefly for confirmation";
    }
  }
}

std::filesystem::path get_roaming_log_directory() {
  PWSTR roaming_path = nullptr;
  HRESULT hr = SHGetKnownFolderPath(FOLDERID_RoamingAppData, KF_FLAG_CREATE, nullptr, &roaming_path);
  if (FAILED(hr) || roaming_path == nullptr) {
    if (roaming_path) {
      CoTaskMemFree(roaming_path);
    }
    return {};
  }

  std::filesystem::path result(roaming_path);
  CoTaskMemFree(roaming_path);
  result /= L"Sunshine";

  std::error_code ec;
  std::filesystem::create_directories(result, ec);
  if (ec) {
    return {};
  }

  return result;
}

/**
 * @brief Helper function to get the WGC helper log path.
 *
 * Logs live under Roaming/Sunshine so they sit with the other Sunshine logs.
 * When session-mode logging is enabled, the returned file path is used to derive
 * the base name and output directory (typically Roaming/Sunshine/logs).
 * Falls back to the system temporary directory (or current directory) if the roaming
 * folder cannot be resolved.
 *
 * @return Path to the log file as a UTF-8 string.
 */
std::string get_temp_log_path() {
  if (auto roaming_dir = get_roaming_log_directory(); !roaming_dir.empty()) {
    auto log_file = roaming_dir / L"sunshine_wgc_helper.log";
    return wide_to_utf8(log_file.wstring());
  }

  std::wstring temp_path(MAX_PATH, L'\0');
  if (auto len = GetTempPathW(MAX_PATH, temp_path.data()); len == 0 || len > MAX_PATH) {
    return "sunshine_wgc_helper.log";
  }
  temp_path.resize(wcslen(temp_path.data()));
  std::wstring wlog = temp_path + L"sunshine_wgc_helper.log";
  return wide_to_utf8(wlog);
}

/**
 * @brief Helper function to handle IPC messages from the main process.
 *
 * Processes incoming messages from the main Sunshine process via named pipe:
 *
 * - Configuration messages: Receives and stores config_data_t structure with display settings
 *
 * @param message The received message bytes from the named pipe.
 *
 */
void handle_ipc_message(std::span<const uint8_t> message) {
  if (message.size() == sizeof(platf::dxgi::window_set_data_t)) {
    platf::dxgi::window_set_data_t update {};
    memcpy(&update, message.data(), sizeof(update));
    if (update.magic != platf::dxgi::WGC_WINDOW_SET_MESSAGE_MAGIC || update.pid_count > platf::dxgi::WGC_WINDOW_SET_MAX_PIDS ||
        !std::is_sorted(update.pids, update.pids + update.pid_count)) {
      BOOST_LOG(warning) << "Ignoring invalid window set update";
      return;
    }

    {
      auto &state = window_set_state();
      std::lock_guard lock(state.mutex);
      state.target_hwnd = update.target_hwnd;
      state.pids.assign(update.pids, update.pids + update.pid_count);
      ++state.generation;
    }
    g_window_set_generation.fetch_add(1, std::memory_order_acq_rel);
    if (HANDLE wake = layer_wake_event()) {
      SetEvent(wake);
    }
    BOOST_LOG(debug) << "Window set updated: " << update.pid_count << " process(es)";
    return;
  }

  if (message.size() == sizeof(platf::dxgi::activity_admission_data_t)) {
    platf::dxgi::activity_admission_data_t update {};
    memcpy(&update, message.data(), sizeof(update));
    if (update.magic != platf::dxgi::WGC_ACTIVITY_ADMISSION_MESSAGE_MAGIC || update.admission_fps <= 0) {
      BOOST_LOG(warning) << "Ignoring invalid WGC activity admission update";
      return;
    }

    g_activity_admission_fps.store(update.admission_fps, std::memory_order_release);
    g_activity_admission_generation.fetch_add(1, std::memory_order_acq_rel);
    BOOST_LOG(info) << "WGC activity admission updated to " << update.admission_fps << "fps";
    return;
  }

  // Handle config data message
  if (message.size() == sizeof(platf::dxgi::config_data_t)) {
    std::lock_guard lock(g_config_mutex);
    if (g_config_received) {
      return;
    }

    memcpy(&g_config, message.data(), sizeof(platf::dxgi::config_data_t));
    g_config_received = true;
    g_activity_admission_fps.store(g_config.activity_admission_fps, std::memory_order_release);
    g_activity_admission_generation.fetch_add(1, std::memory_order_acq_rel);
    // If log_level in config differs from current, update log filter
    if (INITIAL_LOG_LEVEL != g_config.log_level) {
      // Update log filter to new log level
      boost::log::core::get()->set_filter(
        severity >= g_config.log_level
      );
      BOOST_LOG(info) << "Log level updated from config: " << g_config.log_level;
    }
    BOOST_LOG(info) << "Received config data: hdr: " << g_config.dynamic_range
                    << ", display: '" << winrt::to_string(g_config.display_name) << "'"
                    << ", adapter LUID: " << std::hex << g_config.adapter_luid.HighPart
                    << ":" << g_config.adapter_luid.LowPart << std::dec
                    << ", target_fps: " << g_config.target_fps
                    << ", activity_admission_fps: " << g_config.activity_admission_fps
                    << ", min_update_interval_100ns: " << g_config.min_update_interval_100ns
                    << ", initial_buffers: " << g_config.initial_frame_buffer_size
                    << ", max_buffers: " << g_config.max_frame_buffer_size
                    << ", force_sdr_capture: " << ((g_config.flags & platf::dxgi::WGC_IPC_FLAG_FORCE_SDR_CAPTURE_FORMAT) ? "yes" : "no")
                    << ", drain_to_latest: " << ((g_config.flags & platf::dxgi::WGC_IPC_FLAG_DRAIN_TO_LATEST) ? "yes" : "no")
                    << ", allow_buffer_decrease: " << ((g_config.flags & platf::dxgi::WGC_IPC_FLAG_ALLOW_BUFFER_DECREASE) ? "yes" : "no")
                    << ", window_capture: " << ((g_config.flags & platf::dxgi::WGC_IPC_FLAG_WINDOW_CAPTURE) ? "yes" : "no")
                    << ", window_set: " << ((g_config.flags & platf::dxgi::WGC_IPC_FLAG_WINDOW_SET) ? "yes" : "no")
                    << ", target_hwnd: 0x" << std::hex << g_config.target_hwnd << std::dec;
    g_config_cv.notify_all();
  }
}

/**
 * @brief Helper function to setup the communication pipe with the main process.
 *
 * Configures the AsyncNamedPipe with callback functions for message handling:
 * - on_message: Delegates to handle_ipc_message() for processing received data
 * - on_error: Handles pipe communication errors (currently empty handler)
 *
 * @param pipe Reference to the AsyncNamedPipe to configure.
 *
 * @return true if the pipe was successfully configured and started, false otherwise.
 */
bool setup_pipe_callbacks(AsyncNamedPipe &pipe) {
  auto on_message = [](std::span<const uint8_t> message) {
    handle_ipc_message(message);
  };

  auto on_error = [](std::string_view /*err*/) {
    // Error handler, intentionally left empty or log as needed
  };

  return pipe.start(on_message, on_error);
}

/**
 * @brief Helper function to process window messages for desktop hooks.
 *
 * @param shutdown_requested Reference to shutdown flag that may be set if WM_QUIT is received.
 * @return true if messages were processed, false if shutdown was requested.
 */
bool process_window_messages(bool &shutdown_requested) {
  MSG msg;
  while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
    if (msg.message == WM_QUIT) {
      shutdown_requested = true;
      return false;
    }
  }
  return true;
}

/**
 * @brief Helper function to setup desktop switch hook for secure desktop detection.
 *
 * @return true if the hook was successfully installed, false otherwise.
 */
bool setup_desktop_switch_hook() {
  BOOST_LOG(info) << "Setting up desktop switch hook...";

  HWINEVENTHOOK raw_hook = SetWinEventHook(
    EVENT_SYSTEM_DESKTOPSWITCH,
    EVENT_SYSTEM_DESKTOPSWITCH,
    nullptr,
    desktop_switch_hook_proc,
    0,
    0,
    WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS
  );

  if (!raw_hook) {
    BOOST_LOG(error) << "Failed to set up desktop switch hook: " << GetLastError();
    return false;
  }

  g_desktop_switch_hook.reset(raw_hook);
  BOOST_LOG(info) << "Desktop switch hook installed successfully";
  return true;
}

/**
 * @brief Main application entry point for the Windows Graphics Capture helper process.
 *
 * This standalone executable serves as a helper process for Sunshine's Windows Graphics
 * Capture functionality. The main function performs these key operations:
 *
 * 1. **System Initialization**: Sets up logging, DPI awareness, thread priority, and MMCSS
 * 2. **IPC Setup**: Establishes named pipe communication with the main Sunshine process
 * 3. **D3D11 Device Creation**: Creates hardware-accelerated D3D11 device and WinRT interop
 * 4. **Monitor Selection**: Identifies and configures the target monitor for capture
 * 5. **Shared Resource Creation**: Sets up shared D3D11 texture for inter-process frame sharing
 * 6. **WGC Setup**: Initializes Windows Graphics Capture frame pool and capture session
 * 7. **Desktop Monitoring**: Sets up hooks to detect secure desktop transitions
 * 8. **Main Loop**: Processes window messages and handles capture events until shutdown
 *
 * @param argc Number of command line arguments. Expects at least 2 arguments.
 * @param argv Array of command line argument strings. argv[1] should contain the pipe name GUID.
 * @return 0 on successful completion, 1 on initialization failure.
 */
int main(int argc, char *argv[]) {
  // Set up default config and log level
  // Use session-mode logs so rare startup deadlocks don't get overwritten by the next helper run.
  auto log_deinit = logging::init(2, get_temp_log_path());

  // Check command line arguments for pipe names
  if (argc < 2) {
    BOOST_LOG(error) << "Usage: " << argv[0] << " <pipe_name_guid>";
    return 1;
  }

  std::string pipe_name = argv[1];
  BOOST_LOG(info) << "Using pipe name: " << pipe_name;

  // Initialize system settings (DPI awareness, thread priority, MMCSS)
  SystemInitializer system_initializer;
  if (!system_initializer.initialize_all()) {
    BOOST_LOG(error) << "System initialization failed, exiting...";
    return 1;
  }

  // Debug: Verify system settings
  BOOST_LOG(info) << "DPI awareness set: " << (system_initializer.is_dpi_awareness_set() ? "YES" : "NO");
  BOOST_LOG(info) << "Thread priority set: " << (system_initializer.is_thread_priority_set() ? "YES" : "NO");
  BOOST_LOG(info) << "GPU scheduling priority set: " << (system_initializer.is_gpu_priority_set() ? "YES" : "NO");
  BOOST_LOG(info) << "MMCSS characteristics set: " << (system_initializer.is_mmcss_characteristics_set() ? "YES" : "NO");

  BOOST_LOG(info) << "Starting Windows Graphics Capture helper process...";

  // Create named pipe for communication with main process using provided pipe name
  AnonymousPipeFactory pipe_factory;

  auto comm_pipe = pipe_factory.create_client(pipe_name);
  auto pipe_shared = std::make_shared<AsyncNamedPipe>(std::move(comm_pipe));
  g_communication_pipe_weak = pipe_shared;  // Store weak reference for desktop hook callback

  if (!setup_pipe_callbacks(*pipe_shared)) {
    BOOST_LOG(error) << "Failed to start communication pipe";
    return 1;
  }

  constexpr auto max_wait = std::chrono::milliseconds(5000);
  {
    std::unique_lock lock(g_config_mutex);
    if (!g_config_cv.wait_for(lock, max_wait, []() {
          return g_config_received;
        })) {
      BOOST_LOG(error) << "Timed out waiting for config data from main process (" << max_wait.count() << "ms)";
      return 1;
    }
  }

  // Create D3D11 device and context using the same adapter as the main process
  D3D11DeviceManager d3d11_manager;
  if (!d3d11_manager.initialize_all(g_config.adapter_luid)) {
    BOOST_LOG(error) << "D3D11 device initialization failed, exiting...";
    return 1;
  }

  // Monitor management
  DisplayManager display_manager;
  if (!display_manager.select_monitor(g_config)) {
    BOOST_LOG(error) << "Monitor selection failed, exiting...";
    return 1;
  }

  if (!display_manager.get_monitor_info()) {
    BOOST_LOG(error) << "Failed to get monitor info, exiting...";
    return 1;
  }

  // Create GraphicsCaptureItem for monitor using interop.
  // Virtual display topology can churn briefly while Sunshine is applying display settings,
  // so retry monitor selection/item creation before treating startup as fatal.
  GraphicsCaptureItem item = nullptr;
  if (!display_manager.create_graphics_capture_item(item)) {
    constexpr int kMaxCreateItemAttempts = 8;
    bool created = false;
    for (int attempt = 1; attempt <= kMaxCreateItemAttempts && !created; ++attempt) {
      std::this_thread::sleep_for(std::chrono::milliseconds(250));
      if (!display_manager.select_monitor(g_config)) {
        BOOST_LOG(warning) << "GraphicsCaptureItem retry " << attempt
                           << "/" << kMaxCreateItemAttempts
                           << ": monitor selection failed";
        continue;
      }
      if (!display_manager.get_monitor_info()) {
        BOOST_LOG(warning) << "GraphicsCaptureItem retry " << attempt
                           << "/" << kMaxCreateItemAttempts
                           << ": failed to query monitor info";
        continue;
      }
      if (display_manager.create_graphics_capture_item(item)) {
        BOOST_LOG(info) << "GraphicsCaptureItem created after retry " << attempt
                        << "/" << kMaxCreateItemAttempts;
        created = true;
        break;
      }
    }
    if (!created) {
      BOOST_LOG(error) << "Failed to create GraphicsCaptureItem after retries";
      return 1;
    }
  }

  // Calculate final resolution based on config and monitor info
  display_manager.configure_capture_resolution(item);

  // WGC silently stops delivering frames once the capture item closes; display mode
  // changes, DWM restarts and monitor removals all do this (especially on Windows 10).
  // Exit so the main process notices the broken pipe and reinitializes capture instead
  // of freezing on the last delivered frame.
  auto on_item_closed = [](GraphicsCaptureItem const &, winrt::Windows::Foundation::IInspectable const &) {
    BOOST_LOG(warning) << "GraphicsCaptureItem closed; shutting down so capture can be reinitialized";
    g_capture_item_closed.store(true, std::memory_order_release);
  };
  item.Closed(on_item_closed);

  // Window-only sessions capture a single window, composited at its true position on
  // this display. Without a usable window the helper publishes black instead of
  // falling back to the display, so the desktop never reaches the stream. A closed
  // window exits the helper so the main process can pick a new target.
  const bool window_mode = (g_config.flags & platf::dxgi::WGC_IPC_FLAG_WINDOW_CAPTURE) != 0;
  GraphicsCaptureItem window_item = nullptr;
  if (window_mode) {
    const auto target_hwnd = reinterpret_cast<HWND>(static_cast<std::uintptr_t>(g_config.target_hwnd));
    if (target_hwnd && display_manager.create_window_capture_item(target_hwnd, window_item)) {
      window_item.Closed(on_item_closed);
      BOOST_LOG(info) << "Window-only capture of window 0x" << std::hex << g_config.target_hwnd << std::dec;
    } else {
      window_item = nullptr;
      BOOST_LOG(info) << "Window-only capture has no target window; publishing black";
    }
  }
  const bool black_mode = window_mode && !window_item;
  // The app's menus, popups and dialogs are composited over the target only when the main
  // process asks for it; otherwise the window-only path is exactly the single-window one.
  const bool secondary_windows = window_mode && window_item && (g_config.flags & platf::dxgi::WGC_IPC_FLAG_WINDOW_SET) != 0;

  // Use FP16 whenever the stream is HDR or the target output is already in
  // Advanced Color, except when the main process asks for SDR-compatible
  // capture so RTX HDR/TrueHDR can synthesize the HDR frame itself.
  const bool force_sdr_capture =
    g_config_received &&
    ((g_config.flags & platf::dxgi::WGC_IPC_FLAG_FORCE_SDR_CAPTURE_FORMAT) != 0);
  const auto capture_surface_format = platf::dxgi::wgc_policy::select_capture_surface_format(
    g_config_received,
    force_sdr_capture,
    g_config.dynamic_range,
    g_config.advanced_color_capture
  );
  const DXGI_FORMAT capture_format =
    capture_surface_format == platf::dxgi::wgc_policy::capture_surface_format::rgba16_float ?
      DXGI_FORMAT_R16G16B16A16_FLOAT :
      DXGI_FORMAT_B8G8R8A8_UNORM;

  // Create shared resource manager for texture, keyed mutex, and metadata
  SharedResourceManager shared_resource_manager;
  if (!shared_resource_manager.initialize_all(d3d11_manager.get_device(), display_manager.get_width(), display_manager.get_height(), capture_format)) {
    return 1;
  }

  // Send shared handle data via named pipe to main process
  platf::dxgi::shared_handle_data_t handle_data = shared_resource_manager.get_shared_handle_data();
  BOOST_LOG(info) << "Prepared shared handle message - Size: " << sizeof(handle_data) << " bytes, Handle: 0x" << std::hex << reinterpret_cast<uintptr_t>(handle_data.texture_handle) << std::dec;
  std::span<const uint8_t> handle_message(reinterpret_cast<const uint8_t *>(&handle_data), sizeof(handle_data));

  // Wait for connection and send the handle data
  BOOST_LOG(info) << "Waiting for main process to connect...";
  {
    constexpr int connect_timeout_ms = 5000;
    int connect_waited_ms = 0;
    while (!pipe_shared->is_connected() && connect_waited_ms < connect_timeout_ms) {
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
      connect_waited_ms += 50;
    }

    if (!pipe_shared->is_connected()) {
      BOOST_LOG(error) << "Timed out waiting for main process to connect (" << connect_timeout_ms << "ms)";
      return 1;
    }
  }
  BOOST_LOG(info) << "Connected! Sending duplicated handle data...";
  pipe_shared->send(handle_message);
  BOOST_LOG(info) << "Duplicated handle data sent successfully to main process";

  // Create dependencies for capture manager
  WgcCaptureDependencies deps {
    d3d11_manager.get_winrt_device(),
    window_item ? window_item : item,
    shared_resource_manager,
    d3d11_manager.get_context()
  };

  const WgcWindowCaptureTarget window_target {
    .enabled = window_mode,
    .hwnd = window_item ? reinterpret_cast<HWND>(static_cast<std::uintptr_t>(g_config.target_hwnd)) : nullptr,
    .monitor_rect = display_manager.monitor_rect(),
  };

  // Create WGC capture manager
  WgcCaptureManager wgc_capture_manager {capture_format, display_manager.get_width(), display_manager.get_height(), std::move(deps), window_target};
  if (secondary_windows) {
    wgc_capture_manager.enable_secondary_layers();
  }
  if (window_mode && !wgc_capture_manager.publish_black_frame()) {
    BOOST_LOG(warning) << "Failed to publish the initial window-only black frame";
  }

  if (!black_mode) {
    const auto initial_frame_buffer_size = std::clamp<uint32_t>(
      g_config.initial_frame_buffer_size ? g_config.initial_frame_buffer_size : 1,
      1,
      std::max<uint32_t>(1, g_config.max_frame_buffer_size ? g_config.max_frame_buffer_size : 1)
    );
    if (!wgc_capture_manager.create_or_adjust_frame_pool(initial_frame_buffer_size)) {
      BOOST_LOG(error) << "Failed to create frame pool";
      return 1;
    }

    if (!wgc_capture_manager.create_capture_session()) {
      BOOST_LOG(error) << "Failed to create capture session";
      return 1;
    }
  }

  // Set up desktop switch hook for secure desktop detection
  setup_desktop_switch_hook();

  wgc_capture_manager.start_capture();

  // Declared after the capture manager so its sessions close before the manager shuts down.
  std::optional<WindowLayerManager> layer_manager;
  if (secondary_windows) {
    layer_manager.emplace(wgc_capture_manager, d3d11_manager.get_winrt_device(), window_target.hwnd);
    layer_manager->start();
  }

  // Main message loop
  bool shutdown_requested = false;
  if (layer_manager) {
    // Window events are delivered through this thread's message queue, so wait on it
    // instead of sleeping: a shown menu is handled as soon as its event is posted.
    while (pipe_shared->is_connected() && !shutdown_requested && !g_capture_item_closed.load(std::memory_order_acquire)) {
      if (!process_window_messages(shutdown_requested)) {
        break;
      }

      poll_pending_secure_desktop_transition();
      const DWORD wait_ms = layer_manager->tick();

      HANDLE wake = layer_wake_event();
      MsgWaitForMultipleObjects(wake ? 1 : 0, wake ? &wake : nullptr, FALSE, wait_ms, QS_ALLINPUT);
    }
  } else {
    while (pipe_shared->is_connected() && !shutdown_requested && !g_capture_item_closed.load(std::memory_order_acquire)) {
      // Process window messages and check for shutdown
      if (!process_window_messages(shutdown_requested)) {
        break;
      }

      poll_pending_secure_desktop_transition();

      std::this_thread::sleep_for(std::chrono::milliseconds(1));  // Reduced from 5ms for lower IPC jitter
    }
  }

  if (g_capture_item_closed.load(std::memory_order_acquire)) {
    BOOST_LOG(info) << "Capture item closed, shutting down...";
  } else {
    BOOST_LOG(info) << "Main process disconnected, shutting down...";
  }

  pipe_shared->stop();

  // Flush logs before exit
  boost::log::core::get()->flush();

  BOOST_LOG(info) << "WGC Helper process terminated";

  return 0;
}
