# Linux–macOS audit, 2026-11-11

## Scope and evidence

Source baseline: `0d43a9031`; installed Linux and Mac cores report 1.27.0. Reviewed the active Linux/Mac settings, recent INFO logs, Linux clipboard bridge and Mac watcher source, and the input, clipboard, transport, authentication, recovery, and build paths listed below. This includes source/configuration review, optimized builds on both machines, native Mac delivery tests, direct LAN measurements and an isolated sanitizer reproducer. It is not an exhaustive security certification or a physical-device-to-application latency study. No application behavior or live configuration was changed.

Linux build completed; all 32 existing CTest suites passed with a virtual display outside the restricted sandbox. Initial failures were local-socket/display restrictions and disappeared in that environment. Qt build tools were installed. The follow-up below adds a native Mac build, controlled OS injection, optimized Linux testing, reproducible defects and network measurements. Physical-device-to-application delivery is distinct from these component tests.

## Findings, ranked

### High: clipboard format ID can become a negative array index (sanitizer-confirmed)

`IClipboard::unmarshall` converts an untrusted uint32 format ID to `enum class Format` (underlying signed int) and only tests `format < TotalFormats`. A value with all bits set becomes -1 on the tested build, passes that check and reaches `Clipboard::add`, which indexes its format storage using the negative value. Existing length validation does not prevent this.

The opt-in `ClipboardTests::auditMalformedFormatId` was run in a separate GCC ASan/UBSan process with halt-on-error, using one format record with no payload. UBSan stopped with a null-reference error in string assignment from `Clipboard::add`, reached through `IClipboard::unmarshall`. The child exited 1 as expected for this defect; see `clipboard-sanitizer.txt`. Nothing malformed was sent to the running applications.

The parser is used by network clipboard receive paths on both client and server. Network reachability is established by source inspection, not a live exploit. With the user's current TLS/fingerprint settings, a connected trusted peer is required; this does not establish unauthenticated Internet exposure or code execution. Fix by validating the raw unsigned ID against the valid format range before conversion and defensively checking storage indexes. Treat this as a release blocker for the customized clipboard integration.

The published [clipboard advisory GHSA-3jp5-g964-cgmh](https://github.com/deskflow/deskflow/security/advisories/GHSA-3jp5-g964-cgmh) addresses truncated headers/payload lengths, not this signed format-ID path. Published advisory metadata was retrieved and is retained in `upstream-advisories.json`; listed patched releases precede 1.27, but this review does not assume that the list exhausts remaining defects.

### High: Mac scroll scale discards movement (confirmed arithmetic defect)

`src/lib/platform/OSXComputer.mm`, `OSXComputer::fakeMouseWheel`; `src/lib/deskflow/ISecondaryComputer.h`, `applyScrollModifier`.

The active Mac setting is `yScrollScale=0.25`. Installed-tag 1.27 source converts each 120-unit notch to three integer lines, then scales and truncates: `int(3 * 0.25) = 0`. Two notches arriving together produce `int(6 * 0.25) = 1`. Thus identical total movement behaves differently depending on event grouping. Current upstream removes the 3x boost but still truncates: a notch becomes `int(1 * 0.25) = 0`. Upgrading alone does not resolve fractional scale loss. No remainder is retained by this method.

Recommendation: preserve fractional deltas through scaling and accumulate output remainders, or use a carefully calibrated pixel injection path. Test both axes, both signs, slow notches, bursts, direction reversals and scale extremes. Do not add synthetic smoothing until raw delivery is correct.

### High: Wayland scroll resolution is discarded (confirmed design limitation)

`src/lib/platform/EiComputer.cpp`, `onPointerScrollEvent`, `onPointerScrollDiscreteEvent`.

Smooth deltas are converted using a fixed ten-pixels-per-notch ratio and emitted only as full 120-unit clicks. Discrete high-resolution input is also accumulated to whole clicks. Gesture stop/cancel events are ignored. This loses smoothness and can retain a remainder across gestures. The current upstream separation of smooth/discrete remainder units fixes another defect absent from 1.27, but does not preserve continuous motion. Whether this mouse produces smooth or discrete events is still unmeasured. Do not assume duplicate scrolling without capturing actual frames.

Recommendation: explicit wheel versus continuous-scroll semantics, per-device remainders and gesture lifetime, plus negotiated protocol capability if richer metadata is needed. Keep compatibility with old peers intentional.

### High: keyboard repeat/identity tracking is fragile (source-confirmed, symptom linkage unproven)

`src/lib/platform/EiComputer.cpp`, `onKeyEvent`.

Repeat detection remembers one logical key, not the physical held-key set. For the sequence A-down, B-down, A-repeat, the repeated A is classified as a fresh press. Releasing any key clears the single last-key value. Logical key identity is recalculated for releases using current modifier state rather than retained from the press. Lock/layout transitions can therefore change the release identity. The downstream KeyState::fakeKeyDown also detects already-held server buttons and converts them to repeats, so this misclassification alone does not prove dropped input. The separate zero-button hotkey collision below is a more concrete failure.

Recommendation: record physical key identity and press-time logical identity; explicitly distinguish initial press, repeat, release and capture reset. Reproduce rollover, repeated digits, modifiers released on the other screen, and Num Lock changes. Audit synthetic client state alongside the source state before changing delivery.

### High: numpad workaround goes through hotkey/action machinery (confirmed configuration)

The active server layout maps keypad navigation keys to digits using `keystroke(...)` rules. `InputFilter::KeystrokeCondition` consumes hotkey activation/deactivation, and `KeystrokeAction::perform` emits synthetic key-down/up events. These are not a dedicated physical-key translation layer. Their repeat/modifier behavior needs regression coverage, especially when compositor Num Lock reporting changes the mapped identity. Current upstream also has a lock-insensitive hotkey matching fix absent from the installed release.

Recommendation: a per-peer keypad policy at key translation, retaining physical press/release pairing, rather than more hotkey rules. Preserve intentional navigation mode when selected. Do not remove the workaround before validating replacement on the Mac.

### High: clipboard work can block the event path (confirmed synchronous implementation)

`PortalInputCapture::readClipboardSelection`, `PortalClipboard::readSelectionBytes`, `serveSelectionTransfer`.

Portal selection callbacks perform synchronous pipe polling with 200 ms per-iteration waits and image conversion. Reads have a byte cap but no overall transfer deadline; writes attempt the entire remaining payload after poll, which does not guarantee a large blocking write completes immediately. Reads can return partial contents after timeout. This is a latency risk when portal clipboard support is active, not an established cause on the current unsupported-portal setup.

Recommendation: asynchronous bounded transfers, an overall deadline, cancellation, complete-payload validation, and image work off the input dispatch thread. Input should remain responsive during stalled clipboard owners and maximum-size transfers.

### Medium: custom clipboard bridge has delay and failure semantics (confirmed source)

The Linux `mac-clipboard` helper and `watch.js` use SSH, wl-copy/wl-paste and a Mac pasteboard poll every 250 ms. Mac changes can wait almost one polling interval before transport. Linux-to-Mac starts an SSH command per change (connection multiplexing may reduce setup cost). Text is hashed and marked delivered before copying succeeds, so a failed copy can suppress retry of identical content. Empty clipboard updates are ignored; changes between Mac polls may be missed. Linux secret detection queries current types separately from captured text, creating a possible race. One shared hash is not a versioned conflict-resolution protocol.

Recommendation: integrate a Wayland clipboard backend and use Deskflow’s authenticated peer channel; retain secret/transient MIME exclusions, size bounds, explicit clear semantics, delivery acknowledgement, cancellation and loop prevention. Add image support only after text transfer is robust. Do not log clipboard contents.

### Medium: event fairness and recovery need stress coverage

`EiEventQueueBuffer::waitForEvent`, `EiComputer::handleSystemEvent`, `KeyState::updateKeyState`, `ClientApp::retryTime`.

The libei handler drains pending input without a work budget; its queue source explicitly notes potential starvation of other events. The queue pipe-drain expression assigns the boolean result of `read(...) > 0` rather than the byte count, corrupting diagnostic counts (not itself proof of event loss). State resynchronization clears depressed modifiers, which is consistent with why crossing screens might recover a stale state, but logs at INFO do not identify the failing key sequence. Dynamic connection retries eventually reach 300 seconds when enabled; inspect effective settings and network-change triggers before attributing wake recovery to this policy.

Recommendation: bounded fair draining, queue-depth and dispatch-latency counters, reset epochs on capture/device lifecycle changes, immediate retry on meaningful network/wake events, and explicit release of held synthetic input on disconnect.

### Medium: external cursor helper complicates ownership

A separate compiled `deskflow-cursor-park` process is running on the Mac and tails the client log. Follow-up located and reviewed `~/.local/src/cursorhide/park.swift`: every matching leave line warps the cursor to the bottom-right of the main screen and calls CGAssociateMouseAndMouseCursorPosition(1). It does not check the current active screen, track a generation, or cancel queued leave actions after re-entry. A delayed leave action can therefore race with re-entry. Recent logs include repeated rapid screen transitions and cursor-visibility warnings; attribution of a particular observed transition to this race remains unproven.

Recommendation: locate helper source and trace cursor ownership before integrating or removing it. Cursor state should eventually have one owner in the application, driven by events rather than parsing human-readable logs.

## Transport and security review

`TCPSocket` already enables TCP_NODELAY. `ServerProxy::handleMessage` sends a no-op response for successfully handled messages as a historical delayed-ACK workaround: benchmark packet overhead before removing it. Mouse movement coalescing exists; preserve ordering barriers around clicks, scrolling and keyboard input. Bulk clipboard traffic shares the transport, so evaluate input latency under transfer load before deciding between bounded scheduling and a separate authenticated bulk channel.

Both live configurations enable TLS and peer fingerprint checking. `SecureSocket` disables TLS versions below 1.2 and uses explicit fingerprint trust; the permissive certificate callback is part of that pinning design, not evidence that all peers are accepted. Packet, string/vector and clipboard assembly paths contain size checks. Follow-up checked published Deskflow advisories and reproduced the format-ID defect under ASan/UBSan. It did not fuzz all decoders, audit every dependency advisory, or validate every handshake/error path. Keep encryption and pairing; removing them is not a justified performance optimization.

## Language and build decision

Keep C++20 with Objective-C++ at the macOS API boundary and Qt for the UI. These findings concern semantics, blocking work and state management; none establishes language overhead as a bottleneck. A Rust rewrite would require revalidating mature capture/injection integrations and has no demonstrated speed benefit here. Rust remains an option for a separately justified new component, not an audit conclusion.

The initial CMake configuration left CMAKE_BUILD_TYPE empty. Follow-up builds explicitly use RelWithDebInfo (-O2 with symbols) on both machines; their measurements are identified below. Evaluate LTO and profile-guided optimization only after correctness and profiling; avoid fast-math in input accumulation and avoid CPU-specific flags in portable packages.

## Required measurement and implementation gates

1. Record mouse model, wheel mode, keyboard layout, effective capture backend and physical-event types. Capture only deliberate test sequences, not ordinary typing.
2. Build a controlled Linux-to-Mac input receiver and measure counts/order for at least 100,000 scripted keyboard transitions, rollover, keypad repeats and capture changes. Repeat with real hardware. Require zero lost/duplicated transitions and no stuck modifiers.
3. Measure capture-to-send, receive-to-inject, queue delay and application-observed delivery separately. Report p50/p95/p99/max and CPU/wakeups at idle, normal use and high polling rates. One-way measurements require clock calibration; do not subtract unsynchronized timestamps. Suggested initial local processing target: p99 below 1 ms per endpoint, subject to measured platform constraints—not a current result.
4. Verify equal cumulative scrolling for equal input regardless of chunking; test both directions/axes/scales, apps, wheel modes and gesture boundaries. Validate native Mac feel with the user.
5. Stress large/slow clipboard transfers, stale owners, simultaneous changes, empty content, secrets and disconnects while typing. Set explicit latency and memory budgets after baseline measurement.
6. Exercise sleep/wake, network loss, server restart, portal/device recreation, secure input, permission changes and cursor handoff. Require recovery without crossing screens as a workaround.
7. Only then implement the ranked fixes in small validated changes. First fractional scrolling and key-state correctness; then integrated clipboard and recovery; then measured optimizations and additional features.

## Follow-up execution and conclusions

The requested audit was continued beyond source inspection. The following checks are now executed; these are component measurements, not a claim that every end-to-end scenario has passed.

### Build and test evidence

- Linux: GCC 16.2.1, Qt 6.12.0, RelWithDebInfo; 32/32 suites pass, including three **expected failures documenting unfixed bugs**. An expected failure is evidence of a defect, not a fix. The tests will report an unexpected pass when a fix changes that behavior.
- Mac: arm64, macOS 27.2.1, Apple Clang 21, Qt 6.12.0, RelWithDebInfo; native core, GUI and tests build. 28/28 non-destructive suites pass. Native modifier conversion and clipboard UTF-8 conversion are tested separately.
- Existing Mac tests that replace the real clipboard or inject keys into an arbitrary foreground window were excluded. A new opt-in native receiver test instead creates its own focused text window, checks Accessibility permission and stops further cases if focus is lost.
- A first receiver run used an artificial English-only layout list and produced `q` for requested `a` on the actual layout. This was a harness setup error; the final receiver uses AppUtilUnix's real layout list/current language and normal key-state initialization. A separate intermediate run lost focus and stopped that row; it is not counted as an input-loss result.
- The installed applications and configurations were not replaced. Build dependencies were installed on both machines. Test-only source, audit artifacts and the reproducible LAN probe are retained in the fork.

### Confirmed numpad collision

Tracing `ConfigReadContext::parseKeystroke` through `InputFilter::KeystrokeAction`, server forwarding and client `KeyState` shows that all configured keystrokes carry server button **0**. With overlapping remapped digits, `fakeKeyDown` sees the second press as a repeat of the same held button. The first release releases the current mapping; the second release finds no held button.

`audit_hotkeyRollover` executes this against the real client state machine: release results are **true, false** for two presses followed by two releases. This is stronger evidence than the earlier general suspicion about hotkey rules. It explains a concrete failure mode of the active numpad workaround, though it does not prove that all reported lost input has one cause. Fix by preserving physical key identity in translation or assigning collision-free synthetic identities; test arbitrary press/release order.

### Confirmed XKB lookup defect

`EiKeyState::mapKeyFromKeyval` supplies `xkb_keymap_num_levels_for_key(...)` as the zero-based level index. The count is outside the valid range, so lookup falls back to modifier-dependent state. The actual-method reproducer returns **97 (`a`) without Shift and 65 (`A`) with Shift**, contradicting the method's documented base-level semantics. This defect exists in v1.27.0 and the audited head. Test keypad/Num Lock and Ctrl+Alt function keys when correcting it, not just letters.

### Confirmed fractional scrolling defect

The actual `applyScrollModifier` method at scale 0.25 produces **0 total lines for 1,000 separate one-line events**, but **250 lines for a single 1,000-line event**. An installed-version three-line notch also produces zero. This is grouping-dependent data loss, not subjective sensitivity. The reproducer does not inject wheel events into the user's desktop.

### Key-state throughput

The optimized Linux recording-backend test processed 100,000 mapped press/release transitions in approximately **6 ms**, with exact count and alternating order preserved. This excludes physical capture, networking and OS injection; it shows that this particular mapping loop has ample throughput. It does not establish correctness for modifier rollover, which is tested separately.

### Network baseline

`tools/audit/probe_transport.py` starts a bounded, token-authenticated temporary echo socket over SSH, then measures the direct LAN connection with TCP_NODELAY. SSH is orchestration only, not the measured data path. Each size has 100 warmups and 1,000 measured round trips. It checks exact returned payloads and closes the listener after the run.

| Payload | Median RTT | p99 RTT | Maximum RTT |
| --- | ---: | ---: | ---: |
| 16 bytes | 0.092 ms | 0.119 ms | 0.573 ms |
| 256 bytes | 0.092 ms | 0.117 ms | 1.020 ms |
| 4,096 bytes | 0.186 ms | 0.205 ms | 1.072 ms |

These include Python and socket scheduling; they exclude TLS, Deskflow, clipboard contention and application rendering. They establish that idle LAN latency is small, not that the entire input pipeline has this latency.

### Native injection results

See `mac-native-delivery.txt` for the final four 500-character cases: letters/keypad digits, each at requested 10 ms/1 ms gaps. The harness calls the actual OSXKeyState translation/injection path and checks exact text in a dedicated Qt receiver. Timing starts before translation/injection and ends at the receiver's keyPressEvent. Requested gaps are implemented with a running event loop; the earlier QTest polling-based timings were replaced because they included polling delay.

All **2,000/2,000 characters** were delivered exactly in the final timed run. At the requested 1 ms gap, the final continuously dispatched receiver measured letter delivery at p50 **7.856 ms**, p99 **9.815 ms**, and keypad delivery at p50 **7.801 ms**, p99 **9.444 ms**. Actual pacing was around 120 characters/sec, despite the requested 1 ms interval. Earlier count-only runs with a different event-pump strategy delivered 500/500 characters at roughly 400/sec. The strong harness/pacing dependence means these values must not be interpreted as the intrinsic minimum latency of the HID API or Deskflow.

The timer measures this process and receiver, not the Linux capture/network route, display presentation, or another application's handling. Results should guide investigation, not be advertised as universal end-to-end latency.

### Hardware identified without waiting for user input

Kernel input inventory identifies a **Corsair KATAR PRO XT** mouse and **Razer BlackWidow V4** keyboard. Exact active wheel event mode and physical polling rate have not been recorded. No ordinary user keystrokes were captured by the audit.

### Final engineering priority

1. Reject malformed clipboard format IDs before indexing storage; retain a regression test and run sanitizer-backed parser testing.
2. Fix fractional scroll preservation on both conversion boundaries; define wheel versus continuous semantics and remove grouping-dependent loss.
3. Replace numpad hotkey remapping with identity-preserving per-peer translation; fix XKB level selection and cover modifier/repeat/rollover lifecycle.
4. Integrate clipboard transport with asynchronous backend adapters, secret-MIME protection, acknowledgements and bounded transfers; retire the polling SSH bridge after acceptance tests.
5. Bring cursor ownership and recovery into the app, replacing asynchronous log-driven warps. Add input-state reset and diagnostics that do not record typed content.
6. Profile the repaired pipeline under load before optimizing allocations, queue fairness or packet scheduling. Retain C++20/Objective-C++ and TLS. No measured result justifies a rewrite or dropping security.

### Limits and remaining acceptance work

This audit establishes concrete reproducible defects and cross-platform component behavior. It is not an exhaustive proof of safety or zero input loss. The intermittent physical-keyboard-to-Mac failure was not captured end to end; the tests isolate likely contributors instead. Long sleep/wake/network-failure soaks, loaded clipboard/input latency, full decoder fuzzing and dependency-wide vulnerability review are not completed. Published upstream Deskflow advisories were reviewed, and targeted malformed-input testing was performed as described above. Physical-wheel feel across applications requires acceptance testing after the confirmed scroll defect is fixed. These limits must remain visible when describing the work; neither “fully optimized” nor a universal latency promise is supported yet.

## Reproduction commands

Linux optimized build and ordinary suites:

```sh
cmake -S . -B build-audit -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DBUILD_INSTALLER=OFF -DSKIP_BUILD_TESTS=ON
cmake --build build-audit -j 8
xvfb-run -a env QT_QPA_PLATFORM=minimal ctest --test-dir build-audit/src/unittests --output-on-failure
python3 tools/audit/probe_transport.py
```

Native receiver (Mac build tree), run while the dedicated receiver can keep focus:

```sh
DESKFLOW_AUDIT_LIVE=1 QT_QPA_PLATFORM=cocoa build/mac-audit/src/unittests/platform/OSXKeyStateTests audit_nativeTyping
```

The malformed clipboard reproducer must run as an isolated process, not against the live service. The current unfixed code is expected to exit nonzero with a sanitizer diagnostic. Ordinary clipboard cases pass in this same sanitizer build; leak detection was disabled, so this is not a leak audit.

```sh
cmake -S . -B build/sanitize-audit -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DBUILD_INSTALLER=OFF -DSKIP_BUILD_TESTS=ON '-DCMAKE_CXX_FLAGS=-fsanitize=address,undefined -fno-omit-frame-pointer' '-DCMAKE_EXE_LINKER_FLAGS=-fsanitize=address,undefined'
cmake --build build/sanitize-audit --target ClipboardTests -j 8
env QT_QPA_PLATFORM=minimal ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 DESKFLOW_AUDIT_MALFORMED=1 timeout 15s build/sanitize-audit/src/unittests/deskflow/ClipboardTests auditMalformedFormatId
```
