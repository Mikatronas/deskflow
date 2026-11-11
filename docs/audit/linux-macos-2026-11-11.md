# Linux–macOS audit, 2026-11-11

## Scope and evidence

Source baseline: `0d43a9031`; installed Linux and Mac cores report 1.27.0. Reviewed the active Linux/Mac settings, recent INFO logs, Linux clipboard bridge and Mac watcher source, and the input, clipboard, transport, authentication, recovery, and build paths listed below. This is a source/configuration audit and Linux baseline validation, not an exhaustive security certification or measured end-to-end performance study. No application behavior or live configuration was changed.

Linux build completed; all 32 existing CTest suites passed with a virtual display outside the restricted sandbox. Initial failures were local-socket/display restrictions and disappeared in that environment. Qt build tools were installed. The Mac client was inspected over SSH; a Mac source build and interactive reproduction remain outstanding. Existing tests do not establish lossless physical input on this setup.

## Findings, ranked

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

Repeat detection remembers one logical key, not the physical held-key set. For the sequence A-down, B-down, A-repeat, the repeated A is classified as a fresh press. Releasing any key clears the single last-key value. Logical key identity is recalculated for releases using current modifier state rather than retained from the press. Lock/layout transitions can therefore change the release identity. This is not yet proof of the reported fast-typing loss.

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

A separate compiled `deskflow-cursor-park` process is running on the Mac and tails the client log. Its source was not located/read in this audit, so its behavior is not established. Recent logs include repeated rapid screen transitions and cursor-visibility warnings. Neither proves the helper is responsible.

Recommendation: locate helper source and trace cursor ownership before integrating or removing it. Cursor state should eventually have one owner in the application, driven by events rather than parsing human-readable logs.

## Transport and security review

`TCPSocket` already enables TCP_NODELAY. `ServerProxy::handleMessage` sends a no-op response for successfully handled messages as a historical delayed-ACK workaround: benchmark packet overhead before removing it. Mouse movement coalescing exists; preserve ordering barriers around clicks, scrolling and keyboard input. Bulk clipboard traffic shares the transport, so evaluate input latency under transfer load before deciding between bounded scheduling and a separate authenticated bulk channel.

Both live configurations enable TLS and peer fingerprint checking. `SecureSocket` disables TLS versions below 1.2 and uses explicit fingerprint trust; the permissive certificate callback is part of that pinning design, not evidence that all peers are accepted. Packet, string/vector and clipboard assembly paths contain size checks. This review did not fuzz all decoders, audit dependency advisories, or validate every handshake/error path. Keep encryption and pairing; removing them is not a justified performance optimization.

## Language and build decision

Keep C++20 with Objective-C++ at the macOS API boundary and Qt for the UI. These findings concern semantics, blocking work and state management; none establishes language overhead as a bottleneck. A Rust rewrite would require revalidating mature capture/injection integrations and has no demonstrated speed benefit here. Rust remains an option for a separately justified new component, not an audit conclusion.

The initial CMake configuration leaves CMAKE_BUILD_TYPE empty. Do not use this build to claim optimized performance: benchmark an explicit Release/RelWithDebInfo build on both machines. Evaluate LTO and profile-guided optimization only after correctness and profiling; avoid fast-math in input accumulation and avoid CPU-specific flags in portable packages.

## Required measurement and implementation gates

1. Record mouse model, wheel mode, keyboard layout, effective capture backend and physical-event types. Capture only deliberate test sequences, not ordinary typing.
2. Build a controlled Linux-to-Mac input receiver and measure counts/order for at least 100,000 scripted keyboard transitions, rollover, keypad repeats and capture changes. Repeat with real hardware. Require zero lost/duplicated transitions and no stuck modifiers.
3. Measure capture-to-send, receive-to-inject, queue delay and application-observed delivery separately. Report p50/p95/p99/max and CPU/wakeups at idle, normal use and high polling rates. One-way measurements require clock calibration; do not subtract unsynchronized timestamps. Suggested initial local processing target: p99 below 1 ms per endpoint, subject to measured platform constraints—not a current result.
4. Verify equal cumulative scrolling for equal input regardless of chunking; test both directions/axes/scales, apps, wheel modes and gesture boundaries. Validate native Mac feel with the user.
5. Stress large/slow clipboard transfers, stale owners, simultaneous changes, empty content, secrets and disconnects while typing. Set explicit latency and memory budgets after baseline measurement.
6. Exercise sleep/wake, network loss, server restart, portal/device recreation, secure input, permission changes and cursor handoff. Require recovery without crossing screens as a workaround.
7. Only then implement the ranked fixes in small validated changes. First fractional scrolling and key-state correctness; then integrated clipboard and recovery; then measured optimizations and additional features.

## Outstanding evidence

The physical mouse and exact scroll symptoms are unanswered. No end-to-end latency numbers, live key-loss reproduction, Mac rebuild, helper-source audit, long-running soak, or decoder fuzzing has been completed. These remain required before calling the fork fully optimized or the runtime audit complete. The 32 passing Linux suites are a useful baseline, not substitutes for these checks.
