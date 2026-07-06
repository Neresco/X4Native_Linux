// ---------------------------------------------------------------------------
// x4native_crash_test — Framework isolation test extension
//
// Deliberately crashes inside event callbacks to verify that the framework's
// SEH isolation catches the fault, removes the offending subscriber, and the
// game keeps running. Self-validating: watch this extension's log for the
// [PASS]/[FAIL] verdict (a summary also goes to the framework log).
//
// Test sequence (frame counts, ~60 fps at the main menu):
//   frame   1: crash_frame faults inside on_native_frame_update
//              → framework must catch + remove it (never fires again)
//   frame 120: raise "crash_test_named" → crash_named faults
//              → framework must catch + remove it
//   frame 240: raise "crash_test_named" again
//              → removed handler must NOT fire (counter stays 1)
//   frame 360: verdict — each crasher fired exactly once and we are
//              still alive dispatching frames
//   frame 600: heartbeat — 10s past both faults, still running
//
// NOT a modding example — do not copy crash handlers into a real extension.
// ---------------------------------------------------------------------------
#include <x4n_core.h>
#include <x4n_events.h>
#include <x4n_log.h>

static int  g_frame_crashes = 0;   // invocations of the frame-event crasher
static int  g_named_crashes = 0;   // invocations of the named-event crasher
static int  g_frame          = 0;
static bool g_verdict_logged = false;

// Deliberate access violation. volatile forces the store to be emitted.
static void fault() {
    *(volatile int*)nullptr = 42;
}

// Crashes on the per-frame event — the framework's highest-frequency path.
static void crash_frame(const X4NativeFrameUpdate*) {
    ++g_frame_crashes;
    x4n::log::warn("crash_test: crash_frame invocation #{} — faulting NOW", g_frame_crashes);
    fault();
}

// Crashes on a custom named event.
static void crash_named(void*) {
    ++g_named_crashes;
    x4n::log::warn("crash_test: crash_named invocation #{} — faulting NOW", g_named_crashes);
    fault();
}

// Never crashes — drives the sequence and judges the result.
static void sequencer(const X4NativeFrameUpdate*) {
    ++g_frame;

    if (g_frame == 120) {
        x4n::log::info("crash_test: frame 120 — raising crash_test_named (handler should fault once)");
        x4n::raise("crash_test_named");
    }

    if (g_frame == 240) {
        x4n::log::info("crash_test: frame 240 — raising crash_test_named again (handler should be gone)");
        x4n::raise("crash_test_named");
    }

    if (g_frame == 360 && !g_verdict_logged) {
        g_verdict_logged = true;
        const bool frame_ok = (g_frame_crashes == 1);
        const bool named_ok = (g_named_crashes == 1);
        // Reaching this line at all proves the game survived both faults.
        if (frame_ok && named_ok) {
            x4n::log::info("crash_test: [PASS] frame-crasher fired {} time(s), "
                           "named-crasher fired {} time(s), game alive at frame {}",
                           g_frame_crashes, g_named_crashes, g_frame);
            x4n::log::global.info("crash_test: [PASS] SEH isolation verified — "
                                  "both faulting subscribers caught, removed, game alive");
        } else {
            x4n::log::error("crash_test: [FAIL] frame-crasher fired {} time(s) (want 1), "
                            "named-crasher fired {} time(s) (want 1)",
                            g_frame_crashes, g_named_crashes);
            x4n::log::global.error("crash_test: [FAIL] isolation check failed — see extension log");
        }
    }

    if (g_frame == 600) {
        x4n::log::info("crash_test: heartbeat — frame 600, ~10s after both faults, still running");
    }
}

X4N_EXTENSION {
    x4n::log::info("crash_test: init — this extension deliberately crashes to test framework isolation");

    // Order matters for the frame event: the crasher subscribes first so the
    // dispatcher must survive its fault and still reach the sequencer in the
    // same frame's snapshot.
    x4n::on("on_native_frame_update", crash_frame);
    x4n::on("crash_test_named",       crash_named);
    x4n::on("on_native_frame_update", sequencer);
}

X4N_SHUTDOWN {
    x4n::log::info("crash_test: shutdown (crashes seen — frame: {}, named: {})",
                   g_frame_crashes, g_named_crashes);
}
