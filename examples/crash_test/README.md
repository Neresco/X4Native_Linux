# crash_test — Framework Isolation Test

Self-validating test extension that **deliberately crashes** inside event
callbacks to prove the framework's SEH isolation works: a faulting subscriber
must be caught, logged, and removed — and the game must keep running.

This is a framework regression test, **not a modding example** — never copy the
faulting handlers into a real extension. Remove the deployed copy after the
run; otherwise every game launch re-executes two deliberate access violations.

## What It Tests

| # | Path | Fault | Expected framework behavior |
|---|------|-------|------------------------------|
| 1 | `on_native_frame_update` (per-frame, hottest path) | Null write on first frame | Caught + subscriber removed; remaining subscribers in the same frame's snapshot still run |
| 2 | Named event (`crash_test_named`, raised at frame 120) | Null write in handler | Caught + subscriber removed |
| 3 | Removal is real (re-raise at frame 240) | — | Removed handler must NOT fire again (counter stays 1) |
| 4 | Survival (frames 360 / 600) | — | Verdict + heartbeat logged; `X4.exe` still alive |

The MD-event dispatch paths (`md_fire_before/after`) share the same
`seh_call_event` wrapper as the named-event path, so they are covered by test 2.

## How To Run

```powershell
.\scripts\deploy_examples.ps1 -Example crash_test
.\scripts\build_run.ps1 -SkipBuild        # or launch X4 normally
```

The whole sequence runs automatically at the main menu within ~10 seconds of
the framework loading — no save needs to be loaded.

## Expected Output

Extension log (`<profile>\x4native\x4native_crash_test\x4native_crash_test.log`):

```
[info] crash_test: init — this extension deliberately crashes to test framework isolation
[warn] crash_test: crash_frame invocation #1 — faulting NOW
[info] crash_test: frame 120 — raising crash_test_named (handler should fault once)
[warn] crash_test: crash_named invocation #1 — faulting NOW
[info] crash_test: frame 240 — raising crash_test_named again (handler should be gone)
[info] crash_test: [PASS] frame-crasher fired 1 time(s), named-crasher fired 1 time(s), game alive at frame 360
[info] crash_test: heartbeat — frame 600, ~10s after both faults, still running
```

Framework log (`<profile>\x4native\x4native.log`):

```
[error] Event 'on_native_frame_update': subscriber #NN crashed (SEH) — removed
[error] Event 'crash_test_named': subscriber #NN crashed (SEH) — removed
[info]  crash_test: [PASS] SEH isolation verified — both faulting subscribers caught, removed, game alive
```

Any `[FAIL]` line, a missing verdict, or the game process dying is a
regression in the event-dispatch isolation (`src/core/event_system.cpp`).

## Cleanup

```powershell
Remove-Item -Recurse "<X4>\extensions\x4native_crash_test"
```

## When To Re-Run

After any change to:
- `src/core/event_system.cpp` (dispatch loops, `seh_call_event`)
- the subscriber lifecycle (`subscribe`/`unsubscribe`, extension unload cleanup)
- SEH/exception policy at the extension callback boundary

## Not Covered (yet)

- **Hook-callback faults** — already isolated separately with auto-disable in
  `hook_manager.cpp` (`seh_call_hook`); needs a hooked game function that is
  actually called at the menu to test the same way.
- **Dangling shared detours** — two extensions hooking the same game function,
  then one hot-reloading out (the pin-on-unload path in
  `HookManager::protect_dangling_detours`). Needs a sibling extension pair
  (`detour_test_a`/`detour_test_b`).
- **Init-time crashes** — a fault inside `x4native_init` (isolated by
  `seh_call_init`; testing it would prevent this extension from running the
  other cases, so it belongs in a separate fixture).
