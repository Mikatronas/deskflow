# Custom Linux–Mac fixes, 2026-11-11

## Implemented

- Clipboard format IDs are validated as unsigned wire values before enum conversion. Both in-memory clipboard implementations defend their storage indexes.
- Scroll scaling retains fractional remainders on both axes. Mac injection uses pixels (30 pixels per 120-unit notch at scale 1), so sensitivity 0.25 no longer drops individual notches. Wayland forwards sub-notch discrete deltas and converts continuous deltas at 12 wire units/pixel; gesture stop/cancel clears fractional capture residue. No synthetic momentum is added. The legacy wire format still does not convey device or gesture-phase metadata.
- Capture tracks each held physical button, its press-time identity, and consumed hotkey identity. Repeats use the current printable symbol; releases use the original identity. Device pause/removal and capture stop release held keys. Event draining yields after 256 EI events, and pipe accounting uses the actual byte count.
- XKB's level-count-as-index mistake is corrected for function keys. **The original audit's blanket level-zero recommendation was wrong for printable keys:** mixed native testing demonstrated lowercase output for Shift+A when the wire symbol was `a`. Printable symbols must follow Shift/AltGr/NumLock; only F1–F35 bypass modifier-selected VT-switch symbols.
- The Mac client has opt-in `client/keypadDigits=true` translation, preserving original physical button IDs. This replaces the collision-prone KP hotkey workaround without changing other peers' keypad policy.
- Mac keys use Quartz events with explicit modifier flags. Native repeat-tagged synthetic events were filtered before the Qt receiver on this macOS version. Repeats are therefore additional key-down events with no intervening release. They produce the correct characters while keeping keys held, but applications see ordinary key-downs rather than the native autorepeat flag.
- Native Wayland data-control clipboard runs on a dedicated worker, over Deskflow's existing TLS channel. It supports text, HTML, PNG/DIB, empty content and explicit clears. Reads are complete-or-discard with a two-second deadline, a 10 MiB hard cap, cancellation on selection replacement, bounded image dimensions, and offer-attached private-MIME filtering. Writes are nonblocking and deadline-bound. An instance marker prevents echoes. The worker reconnects after compositor connection loss. Portal fallback transfers also gain overall deadlines and complete-payload checks.
- Clipboard changes are published immediately to connected peers, including while the pointer remains on the source. Superseded owners' data is ignored; empty snapshots are transmitted. Mac pasteboard reads check generation and concealed/transient types; self-owned changes do not echo. Mac clipboard detection polls at 100 ms.
- Mac cursor hide/show is balanced. Cursor parking happens synchronously during leave, replacing the log-following helper's delayed warps. Client reconnect interval remains one second with dynamic backoff disabled in the local rollout.

## Validation before deployment

- Linux: 33/33 CTest suites, with a virtual display and local socket access.
- Mac: 28/28 non-destructive CTest suites.
- ASan/UBSan: clipboard parser, key-state, XKB and live Wayland clipboard tests pass (leak detection disabled).
- Live Wayland round trips: Unicode/multiline text, HTML, exact image pixels, 1 MiB text, empty text, explicit clear, echo suppression, oversize rejection and subsequent recovery.
- Native mixed Mac receiver: 3,600/3,600 letters/digits/punctuation with overlapping keys; 1,400/1,400 keypad characters with all ten digits held and nonsequential release order, including 400 repeats; 200/200 characters with Shift released before the letter; 63/63 sustained-held-key characters; 100 Command+A/replacement cycles; no residual command modifiers.
- The earlier single-character test passed 2,000/2,000 characters but **missed failures found by the mixed tests**. The mixed test initially saw a dropped character, all 400 keypad repeats missing, and the proposed XKB uppercase regression. Final code passes the expanded tests. These are OS-injection/component tests, not a real physical-keyboard-to-Mac measurement. The timer requested 1 ms intervals but actual receiver pacing was roughly 8.5 ms/character; no 1 kHz native-delivery claim is made.

Raw final evidence: `linux-fixes-tests.txt`, `mac-mixed-fixes.txt`, and `wayland-clipboard-fixes-sanitizer.txt`.

## Remaining acceptance limits

Hardware scroll feel across applications still needs user acceptance. Long sleep/wake/network soaks, full decoder fuzzing and dependency-wide security review are not covered. The native Wayland backend currently targets the first advertised seat and requires wlr-data-control; systems without it retain the portal path. Clipboard ownership uses Deskflow's existing ordered connection and server arbitration, not a new persistent delivery-acknowledgement protocol. Maximum-size bulk transfers still share the input transport; no universal input-latency guarantee is claimed.
