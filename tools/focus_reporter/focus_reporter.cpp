/**
 * @file tools/focus_reporter/focus_reporter.cpp
 * @brief Standalone Windows reporter: says which kind of field has keyboard focus, over UDP.
 *
 * WHY THIS EXISTS
 * ---------------
 * Sunshine already reports the focused field to Moonlight, in the control stream, as the
 * 0x3003 "Set Text Field Focus" packet. That is the better channel in every way except
 * one: it needs the host to be this build. Someone already running a stock Sunshine,
 * Apollo or Vibepollo has a working setup, and "replace your host" is not a reasonable
 * price for a keyboard layout. A separate process cannot inject into an encrypted control
 * stream, so the same signal gets its own channel instead: this program, one UDP datagram
 * per focus change, straight to the handheld.
 *
 * It is the SAME classifier. text_field_watcher.cpp is compiled into this executable
 * verbatim - the ordered rules R0-R8, the provisional Win32 gate, the up-down buddy probe,
 * the bounded ancestor walk, the debounce and the Win32 safety poll are the same object
 * code Sunshine runs. Nothing about which fields count as numeric is decided twice.
 *
 * IT MUST NOT BE A PREP COMMAND. src/process.cpp aborts a launch when a Do step exits
 * non-zero, so a prep command that fails, or one that never returns, presents to the user
 * as "the host accepted the launch and then RTSP timed out". That is what broke this
 * feature the first time it was attempted. This program is never on the launch path: it is
 * started by the user, by hand or from Startup, and Vibepollo does not know it exists.
 *
 * IDLE UNTIL ASKED
 * ----------------
 * Leaving a UI Automation client subscribed to every focus change on the machine, all day,
 * for the sake of a keyboard that is not on screen, would be rude. So the client drives it:
 *
 *   client -> host   VLFOCUS2 <token> hello          every 3s while its keyboard panel is up
 *   host -> client   VLFOCUS2 <token> <kind> <flags> on every change, plus a 1s keepalive
 *
 * On the first accepted hello this starts the watcher; ten seconds after the last one it
 * stops it again. Between streams the process is a thread blocked in recvfrom() on a
 * 100 ms timeout and nothing else: no UI Automation client, no focus subscription, no
 * polling of anybody's windows.
 *
 * SILENT
 * ------
 * WIN32 subsystem, so no console window ever appears. No tray icon, no window, no dialog,
 * no message box, not even on a fatal error - it exits with a code nobody will see.
 * Writes nothing to disk unless --verbose names a log file.
 */

#include "src/platform/windows/text_field_watcher.h"

#include "src/logging.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

// clang-format off
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
// clang-format on

using namespace std::literals;

namespace {

  /// Every datagram in both directions starts with this.
  constexpr std::string_view MAGIC = "VLFOCUS2";
  /// The word a client sends to say it is streaming and would like reports.
  constexpr std::string_view HELLO = "hello";

  /// Where this listens. Must match FocusHintListener.DEFAULT_HOST_PORT on the client.
  constexpr std::uint16_t DEFAULT_LISTEN_PORT = 47997;

  /**
   * @brief How long after the last hello the watcher is torn down.
   *
   * The client repeats its hello every three seconds, so this survives three consecutive
   * lost datagrams. Shorter would drop the watcher on a busy wireless link; much longer
   * would leave UI Automation subscribed for a noticeable while after a stream ends, which
   * is the thing this design exists to avoid.
   */
  constexpr auto CLIENT_TIMEOUT = 10s;

  /// Socket receive timeout, and therefore the rate the publish state is polled at.
  constexpr auto POLL_INTERVAL = 100ms;

  /**
   * @brief How often the current state is repeated to a quiet client.
   *
   * The wire carries absolute state, not edges, so a lost datagram is only ever a delay:
   * the next one puts the client right. This is what bounds that delay.
   */
  constexpr auto KEEPALIVE_INTERVAL = 1000ms;

  /**
   * @brief How many times a CHANGE is repeated, one per poll.
   *
   * A change is the datagram whose loss the user actually feels - it is the one that opens
   * the keyboard - so it goes out three times over 200 ms rather than waiting up to a
   * second for the keepalive. The client discards the repeats: its policy is keyed on the
   * report, so an identical one costs nothing.
   */
  constexpr int REPEATS_ON_CHANGE = 3;

  /// Longest datagram either side will look at. Matches FocusHint.MAX_BYTES.
  constexpr int MAX_DATAGRAM = 96;

  struct options_t {
    std::string token;
    std::string client_filter;  ///< Optional --client: only this address may say hello
    std::uint16_t port {DEFAULT_LISTEN_PORT};
    bool numeric_hints {false};
    bool verbose {false};
    std::string log_file {"focus_reporter.log"};
    bool valid {false};
  };

  /**
   * @brief Compare without leaking where two strings first differ.
   *
   * The token guards nothing more valuable than which keyboard a handheld draws, but it
   * also decides whether a stranger on the network can make this process start watching
   * focus, so it is not compared with a loop that returns early.
   */
  bool constant_time_equals(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) {
      return false;
    }
    unsigned char diff = 0;
    for (std::size_t i = 0; i < a.size(); ++i) {
      diff |= static_cast<unsigned char>(a[i] ^ b[i]);
    }
    return diff == 0;
  }

  std::vector<std::string_view> split_whitespace(std::string_view text) {
    std::vector<std::string_view> parts;
    std::size_t i = 0;
    while (i < text.size()) {
      while (i < text.size() && (text[i] == ' ' || text[i] == '\t' || text[i] == '\r' || text[i] == '\n')) {
        ++i;
      }
      const std::size_t start = i;
      while (i < text.size() && !(text[i] == ' ' || text[i] == '\t' || text[i] == '\r' || text[i] == '\n')) {
        ++i;
      }
      if (i > start) {
        parts.push_back(text.substr(start, i - start));
      }
    }
    return parts;
  }

  /**
   * @brief Whether a datagram is a hello carrying our token.
   *
   * Deliberately the whole of the parsing this program does. There is no command channel
   * and no other verb: the only thing anybody on the network can ask this process to do is
   * "start reporting to me", and they need the token to ask it.
   */
  bool is_hello(std::string_view payload, std::string_view token) {
    if (payload.size() > static_cast<std::size_t>(MAX_DATAGRAM)) {
      return false;
    }
    const auto parts = split_whitespace(payload);
    if (parts.size() != 3) {
      return false;
    }
    if (parts[0] != MAGIC || parts[2] != HELLO) {
      return false;
    }
    return constant_time_equals(parts[1], token);
  }

  /**
   * @brief The wire word for a classified field.
   *
   * "digits" rather than "numeric" because that is the word the client's parser has always
   * used. `unknown` is a word the client understands and this program never sends: the
   * classifier's R7 returns an empty candidate for anything it cannot place, and an empty
   * candidate is exactly "no field" - the client rests, which is the same thing the
   * in-stream 0x3003 path does with kind 0. Keeping a state the reporter cannot produce
   * would be a promise the classifier does not make.
   */
  const char *kind_word(platf::text_field::kind_e kind) {
    switch (kind) {
      case platf::text_field::kind_e::text:
        return "text";
      case platf::text_field::kind_e::numeric:
        return "digits";
      case platf::text_field::kind_e::password:
        return "password";
      case platf::text_field::kind_e::none:
      default:
        return "none";
    }
  }

  std::string format_report(std::string_view token, const platf::text_field::state_t &state) {
    char flags[4] {};
    std::snprintf(flags, sizeof(flags), "%x", static_cast<unsigned>(state.flags));

    std::string out;
    out.reserve(48);
    out.append(MAGIC);
    out.push_back(' ');
    out.append(token);
    out.push_back(' ');
    out.append(kind_word(state.kind));
    out.push_back(' ');
    out.append(flags);
    return out;
  }

  bool token_is_sane(std::string_view token) {
    if (token.empty() || token.size() > 32) {
      return false;
    }
    return std::all_of(token.begin(), token.end(), [](char c) {
      return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
    });
  }

  /**
   * @brief Parse the command line.
   *
   * `--flag value` and `--flag=value` both work. An unrecognised argument is fatal rather
   * than ignored: this program has no window to complain through, so a typo that silently
   * disabled the token would be undiagnosable.
   */
  options_t parse_options(const std::vector<std::string> &args) {
    options_t options;

    // "--flag value" and "--flag=value", and nothing looser than that: matching a prefix
    // would make "--tokenizer x" set the token, and a mistyped flag that still half-works
    // is worse than one that refuses to start.
    const auto matches = [](const std::string &arg, std::string_view name) {
      return arg == name || (arg.size() > name.size() && arg.compare(0, name.size(), name) == 0 &&
                             arg[name.size()] == '=');
    };
    const auto value_of = [&](std::size_t &i, std::string_view name, const std::string &arg) -> std::string {
      if (arg.size() > name.size()) {
        return arg.substr(name.size() + 1);
      }
      if (i + 1 < args.size()) {
        return args[++i];
      }
      return {};
    };

    for (std::size_t i = 1; i < args.size(); ++i) {
      const std::string &arg = args[i];
      if (matches(arg, "--token")) {
        options.token = value_of(i, "--token", arg);
      } else if (matches(arg, "--port")) {
        const auto text = value_of(i, "--port", arg);
        const long parsed = std::strtol(text.c_str(), nullptr, 10);
        if (parsed <= 0 || parsed > 65535) {
          return options;
        }
        options.port = static_cast<std::uint16_t>(parsed);
      } else if (matches(arg, "--client")) {
        options.client_filter = value_of(i, "--client", arg);
      } else if (matches(arg, "--log")) {
        options.log_file = value_of(i, "--log", arg);
        options.verbose = true;
      } else if (arg == "--numeric-hints") {
        options.numeric_hints = true;
      } else if (arg == "--verbose") {
        options.verbose = true;
      } else {
        return options;  // unrecognised: options.valid stays false
      }
    }

    options.valid = token_is_sane(options.token) && !options.log_file.empty();
    return options;
  }

  /**
   * @brief Everything about who we are currently reporting to.
   */
  struct client_t {
    sockaddr_in address {};
    bool present {false};
    std::chrono::steady_clock::time_point last_hello;
    std::chrono::steady_clock::time_point last_send;
    std::uint64_t last_generation {0};
    int repeats_left {0};
  };

  bool same_endpoint(const sockaddr_in &a, const sockaddr_in &b) {
    return a.sin_addr.s_addr == b.sin_addr.s_addr && a.sin_port == b.sin_port;
  }

  std::string address_text(const sockaddr_in &address) {
    char text[INET_ADDRSTRLEN] {};
    if (!inet_ntop(AF_INET, &address.sin_addr, text, sizeof(text))) {
      return "?";
    }
    return std::string {text} + ":" + std::to_string(ntohs(address.sin_port));
  }

  std::atomic_bool g_quit {false};

  BOOL WINAPI console_handler(DWORD) {
    // Only reached when someone attaches a console to this process on purpose. Harmless to
    // register either way, and it means Ctrl+C behaves during development.
    g_quit.store(true, std::memory_order_release);
    return TRUE;
  }

  int run(const options_t &options) {
    WSADATA wsa {};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
      return 2;
    }

    SOCKET sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock == INVALID_SOCKET) {
      WSACleanup();
      return 3;
    }

    // Deliberately NO SO_REUSEADDR. A second copy of this program - the classic outcome of
    // a Startup shortcut plus a manual run - must fail to bind and exit, rather than both
    // copies half-receiving the client's hellos.
    sockaddr_in local {};
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = htonl(INADDR_ANY);
    local.sin_port = htons(options.port);
    if (bind(sock, reinterpret_cast<sockaddr *>(&local), sizeof(local)) == SOCKET_ERROR) {
      BOOST_LOG(error) << "Focus reporter: could not bind UDP port "sv << options.port
                       << ", another copy is probably already running"sv;
      closesocket(sock);
      WSACleanup();
      return 4;
    }

    DWORD timeout_ms = static_cast<DWORD>(POLL_INTERVAL / 1ms);
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char *>(&timeout_ms), sizeof(timeout_ms));

    in_addr allowed {};
    bool have_filter = false;
    if (!options.client_filter.empty()) {
      if (inet_pton(AF_INET, options.client_filter.c_str(), &allowed) == 1) {
        have_filter = true;
      } else {
        BOOST_LOG(error) << "Focus reporter: --client is not an IPv4 address, refusing to start"sv;
        closesocket(sock);
        WSACleanup();
        return 5;
      }
    }

    BOOST_LOG(info) << "Focus reporter: listening on UDP "sv << options.port
                    << ", idle until a client says hello"sv;

    platf::text_field::set_numeric_hints(options.numeric_hints);

    client_t client;
    bool watching = false;
    char buffer[MAX_DATAGRAM + 1];

    const auto stop_watching = [&](const char *why) {
      if (!watching) {
        return;
      }
      // Unbounded join, exactly as Sunshine does it. Here that is nobody's problem but
      // ours: this is not on any launch path, so a slow UI Automation teardown delays this
      // process and nothing else. That is the whole reason this design is safe to leave
      // installed.
      platf::text_field::stop();
      watching = false;
      BOOST_LOG(info) << "Focus reporter: watcher stopped ("sv << why << ")"sv;
    };

    while (!g_quit.load(std::memory_order_acquire)) {
      sockaddr_in from {};
      int from_len = sizeof(from);
      const int received = recvfrom(
        sock,
        buffer,
        MAX_DATAGRAM,
        0,
        reinterpret_cast<sockaddr *>(&from),
        &from_len
      );

      const auto now = std::chrono::steady_clock::now();

      if (received > 0) {
        const std::string_view payload {buffer, static_cast<std::size_t>(received)};
        const bool allowed_source = !have_filter || from.sin_addr.s_addr == allowed.s_addr;
        if (allowed_source && is_hello(payload, options.token)) {
          const bool is_new = !client.present || !same_endpoint(client.address, from);
          client.address = from;
          client.present = true;
          client.last_hello = now;

          if (!watching) {
            if (platf::text_field::start()) {
              watching = true;
              BOOST_LOG(info) << "Focus reporter: watcher started for "sv << address_text(from);
            } else {
              BOOST_LOG(error) << "Focus reporter: the focus watcher would not start"sv;
            }
          } else if (is_new) {
            BOOST_LOG(info) << "Focus reporter: now reporting to "sv << address_text(from);
          }

          if (is_new) {
            // Answer the very first hello immediately, with whatever the watcher currently
            // believes - "nothing has focus" on a cold start. Without it the client would
            // have no idea whether anything is listening until the user happened to click
            // into a text box, which is the one moment they would rather it just worked.
            client.last_generation = 0;
            client.repeats_left = REPEATS_ON_CHANGE;
          }
        }
        // Anything else is dropped without a word. This socket will hear broadcast noise,
        // port scans and the occasional stray reply, and none of it is worth a log line
        // per datagram.
      }

      if (client.present && now - client.last_hello > CLIENT_TIMEOUT) {
        BOOST_LOG(info) << "Focus reporter: "sv << address_text(client.address)
                        << " stopped saying hello"sv;
        client = client_t {};
        stop_watching("no client");
        continue;
      }

      if (!client.present || !watching) {
        continue;
      }

      // running() is false until the worker has actually subscribed to UI Automation focus
      // events, and stays false forever if it could not. Staying quiet in that case is
      // deliberate: a stream of "no field" from a watcher that never started looks exactly
      // like a working watcher over an empty desktop, whereas silence puts the client's
      // status line on "quiet", which points at this machine. --verbose says which.
      if (!platf::text_field::running()) {
        continue;
      }

      const auto state = platf::text_field::current();
      if (state.generation != client.last_generation) {
        client.last_generation = state.generation;
        client.repeats_left = REPEATS_ON_CHANGE;
      }

      const bool due =
        client.repeats_left > 0 || now - client.last_send >= KEEPALIVE_INTERVAL;
      if (!due) {
        continue;
      }
      if (client.repeats_left > 0) {
        --client.repeats_left;
      }
      client.last_send = now;

      const auto report = format_report(options.token, state);
      sendto(
        sock,
        report.data(),
        static_cast<int>(report.size()),
        0,
        reinterpret_cast<const sockaddr *>(&client.address),
        sizeof(client.address)
      );
    }

    stop_watching("shutting down");
    closesocket(sock);
    WSACleanup();
    return 0;
  }

  std::vector<std::string> command_line_utf8() {
    int argc = 0;
    LPWSTR *wargv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::vector<std::string> args;
    if (!wargv) {
      return args;
    }
    args.reserve(static_cast<std::size_t>(argc));
    for (int i = 0; i < argc; ++i) {
      const int need = WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, nullptr, 0, nullptr, nullptr);
      std::string text;
      if (need > 1) {
        text.resize(static_cast<std::size_t>(need) - 1);
        WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, text.data(), need, nullptr, nullptr);
      }
      args.emplace_back(std::move(text));
    }
    LocalFree(wargv);
    return args;
  }

  int reporter_main() {
    const auto options = parse_options(command_line_utf8());
    if (!options.valid) {
      // No console to print to and no dialog by design, so a bad command line is a silent
      // exit code. tools/focus_reporter/README.md is where the usage lives.
      return 1;
    }

    SetConsoleCtrlHandler(console_handler, TRUE);

    // Nothing is written to disk unless this is asked for. Without --verbose the Boost
    // logging core has no sink at all, and every BOOST_LOG in this file and in
    // text_field_watcher.cpp is discarded before its message is even formatted.
    std::unique_ptr<logging::deinit_t> log_guard;
    if (options.verbose) {
      log_guard = logging::init_single_file(1 /* debug */, options.log_file);
    }

    return run(options);
  }

}  // namespace

// Both entry points, the same way tools/playnite_launcher/main.cpp does it. The target is
// built WIN32_EXECUTABLE so no console window ever appears; which of the two the CRT
// actually calls depends on whether the toolchain is in -municode mode, and defining both
// means the link does not depend on that.
int main(int, char **) {
  return reporter_main();
}

int APIENTRY wWinMain(HINSTANCE, HINSTANCE, LPWSTR, int) {
  return reporter_main();
}
