# focus_reporter

A small silent Windows program that tells a Moonlight client which kind of field has
keyboard focus, so the client can put up the right on-screen keyboard: letters for a text
box, a number pad for a numeric one, and bullets instead of an echo for a password box.

It is not part of Vibepollo. It does not talk to Vibepollo, is not started by Vibepollo,
is not in the installer, and **must never be added as a prep command** — see
[Not a prep command](#not-a-prep-command).

---

## Why it exists

Sunshine already reports this, properly, inside the stream: the `0x3003` "Set Text Field
Focus" control packet, produced by `src/platform/windows/text_field_watcher.cpp`. That is
the better channel in every way except one — it needs the host to be a build that sends it.

If you are already running a stock Sunshine, Apollo or Vibepollo, that means replacing your
host, which is a lot to ask for a keyboard layout. And a separate program cannot inject a
packet into an encrypted control stream, so the signal gets its own channel instead: this
program, one UDP datagram per focus change, straight to the handheld.

It is the **same classifier**, not a reimplementation. `text_field_watcher.cpp` is compiled
into this executable verbatim, so the ordered rules, the Win32 style-bit gate, the up-down
buddy probe, the bounded UI Automation ancestor walk, the debounce and the safety poll are
the same code Sunshine runs. There is no second opinion about what counts as a numeric
field.

If your host *does* send `0x3003`, you do not need this at all.

---

## What it does, and when

Nothing, until a client asks.

```
client -> host   VLFOCUS2 <token> hello                every 3 seconds while its keyboard panel is up
host -> client   VLFOCUS2 <token> <kind> <flags>       on every change, plus a 1 second keepalive
```

* On the first hello carrying the right token, it starts the UI Automation focus watcher
  and starts reporting to whatever address the hello came from.
* Ten seconds after the last hello, it stops the watcher again.
* In between — which is nearly all the time — the process is one thread blocked in
  `recvfrom()` on a 100 ms timeout. No UI Automation client, no focus subscription, no
  polling of anybody's windows, no measurable CPU.

That is what makes it safe to leave running: "on while I am streaming and have the keyboard
panel up, completely inert otherwise", with no hook into the host software at all.

`<kind>` is one of `none`, `text`, `digits`, `password`. `<flags>` is a hexadecimal mask:
`1` read-only, `2` multiline, `4` classified through UI Automation, `8` guessed from a
label, `10` numeric evidence present (on a `password` that means a PIN or a CVV). The
client understands a fifth kind, `unknown`; this program never sends it, because the
classifier reports anything it cannot place as "no field", the same as it does in the
stream.

The reports are absolute state, not edges: the most recent one is always the current truth,
so a lost datagram is a delay of at most one second and never a stuck keyboard.

---

## Setting it up

### 1. Get the token from the client

On the handheld: **Settings → Focus reporter setup**. It shows a twelve-character token
that this device generated and keeps. Both ends must use the same one.

### 2. Turn the client's listener on

Also in the client's settings, above the setup entry: **"Also listen for a PC that cannot be
rebuilt"**. It is **off by default** and nothing happens until you turn it on. It sits under
**"Let the PC pick the keyboard"**, which also has to be on.

### 3. Run it on the PC

Put `focus_reporter.exe` wherever you like — `%LOCALAPPDATA%\focus_reporter\` is a
reasonable choice, and nothing depends on the path. Then:

```
focus_reporter.exe --token abc123def456
```

No window will appear. That is correct: it has no console, no tray icon and no UI. Check
Task Manager if you want to see it running.

**Windows Firewall will ask once**, the first time a hello arrives, because this binds an
inbound UDP port. Allow it on **Private** networks only. If you dismissed that prompt, the
client will sit on "PC reporter: waiting on 47996" forever; add the rule by hand in
Windows Defender Firewall → Inbound Rules, or delete the rule it created and let it ask
again.

IPv4 only. If your handheld reaches the PC over IPv6, the hellos will not arrive.

### 4. Optional: start it with Windows

The simplest way, and the easiest to undo:

1. Press <kbd>Win</kbd>+<kbd>R</kbd>, type `shell:startup`, press Enter.
2. Right-click in that folder → **New → Shortcut**.
3. Target: `"C:\path\to\focus_reporter.exe" --token abc123def456`
4. Name it `focus_reporter`. Done — it starts at your next sign-in.

If you would rather use Task Scheduler (for example to start it at boot rather than at
sign-in), create a Basic Task, trigger "When I log on", action "Start a program", program
`focus_reporter.exe`, arguments `--token abc123def456`. Untick "Run with highest
privileges" — it does not need administrator and should not have it.

**Do not use Vibepollo's prep commands for either of these.** See below.

---

## Options

| Option | Meaning |
| --- | --- |
| `--token <token>` | **Required.** The token from the client's setup screen. Letters and digits, up to 32 characters. |
| `--port <n>` | UDP port to listen on. Default `47997`. Change it only if something else already has that port, and only alongside the client. |
| `--client <ip>` | Only accept a hello from this address. Optional, and worth setting if you would rather not rely on the token alone. |
| `--numeric-hints` | Turn on the label-guessing tier: a field called "Port" or "Quantity" is reported as numeric even when nothing about the control says so. Off by default because it guesses, and a wrong number pad is worse than a wrong keyboard. |
| `--verbose` | Write a log. Without this, nothing is written to disk at all. |
| `--log <path>` | Write the log here instead of `focus_reporter.log` in the working directory. Implies `--verbose`. |

A mistyped or unrecognised option makes it exit immediately rather than start with half the
settings you meant. There is no window to say so in, so if it does not appear in Task
Manager, check the command line.

---

## Not a prep command

`src/process.cpp` aborts a launch when a Do step exits non-zero, and it waits for one that
does not return. So a prep command that fails, or one that keeps running, stops the stream
from starting — and what the user sees is not an error about the prep command, it is
"the host accepted the launch and then RTSP timed out". That is exactly how this feature
broke the first time it was attempted.

This program is therefore **never on the launch path**. Vibepollo does not start it, does
not stop it, does not know it exists, and cannot be delayed or failed by it. If it crashes,
fails to bind its port, or is not running at all, the only consequence is that the keyboard
on the handheld has to be picked by hand, exactly as it was before.

**If you added Do/Undo prep commands for the old PowerShell reporter, delete them.** They
are the thing that was breaking streams, and this does not replace them with anything.

---

## Removing it

There is nothing to uninstall. It installs no service, registers nothing, writes no
registry keys, and creates no files unless you passed `--verbose`.

1. **Stop it.** Task Manager → Details → `focus_reporter.exe` → End task. (Or just sign
   out; it does not survive a reboot on its own.)
2. **Stop it coming back.** <kbd>Win</kbd>+<kbd>R</kbd> → `shell:startup` → delete the
   `focus_reporter` shortcut. If you used Task Scheduler instead, delete the task there.
3. **Delete the exe**, and the log file if you made one.
4. **Remove the firewall rule**, if you allowed the prompt during setup. Windows Defender
   Firewall → Advanced settings → Inbound Rules → find the `focus_reporter` entry (it
   names the path you ran it from) → Delete. This is the only thing that outlives the
   exe. It is inert once the exe is gone, but it is a leftover and this is how it goes.

That is everything it leaves behind.

On the handheld, turn **"Also listen for a PC that cannot be rebuilt"** back off. That
unbinds the UDP port and stops the hellos.

---

## What it can and cannot see

Out of process, Windows only tells you so much. Honestly:

**Works:** classic Win32 edit controls with `ES_NUMBER` or `ES_PASSWORD`, classic
spin-button buddy edits, WinForms `NumericUpDown`, WinUI/UWP `NumberBox`, WPF-toolkit and
Avalonia up/downs, Qt `QSpinBox`, and `<input type="number">` in Chromium and anything
built on it.

**Does not work, and cannot be made to from outside the application:**

* `inputmode="numeric"`, `"decimal"` or `"tel"` on a web page. Chromium does not publish
  `inputmode` to any accessibility API; it exists only in the in-process TSF input scope.
* `<input type="tel">`.
* A plain text box an application validates as numeric in its own code — a WPF `TextBox`
  with a converter, a `QLineEdit` with a validator, a React input with a regex mask. No
  Windows API describes that intent to anyone. This class of field will always be wrong.
* Anything inside a Java or Swing application. UI Automation sees an opaque window, so the
  keyboard will not raise at all.
* The lock screen, the UAC prompt and Ctrl+Alt+Del. They run on a separate secure desktop
  that no ordinary program can read, by design.

The client always keeps a manual override — the game menu, L1/R1 while typing, and the
both-stick chord — precisely because of the third one.

---

## Security, such as it is

The token is twelve hexadecimal characters and it is compared in constant time, but it is
not a cryptographic authenticator and this is not an encrypted channel. What it actually
buys:

* Nobody without the token can make this process start watching focus.
* Nobody without the token can be sent reports.
* `--client` narrows it further to a single address.

What an attacker on your local network who has the token could learn is the *kind* of field
you have focused — text, digits, password — and nothing about its contents. Nothing here can
read text, send input, start a session, or reach anything outside its own socket. It is
still a local-network-only tool and should not be exposed to the internet.

---

## Building it

It is built by the normal Windows build, as the `focus-reporter` target:

```
cmake --build build --target focus-reporter
```

The output is `focus_reporter.exe`. It is deliberately **not** in the MSI payload and is
not copied next to `sunshine.exe`: it is a file you put somewhere and delete when you are
done with it, and putting it in the installer would make it something you have to uninstall.
