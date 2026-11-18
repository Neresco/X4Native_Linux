# Experimental multiplayer validation

AI assistance is being used for this contribution. Test outcomes and limits are
recorded explicitly so reviewers can assess the work independently. We are keen
to help with further development and welcome guidance on useful next steps.

## Evidence boundaries

| Layer | Observed evidence | Not established |
| --- | --- | --- |
| CPU rendering | Linux 9.00 official NoSteam executable enumerated/ran with Mesa CPU Vulkan and no hardware GPU devices | Renderer bypass, arbitrary versions or comparable performance on other hardware |
| Universe | Unpaused clock and sampled NPC motion with zero clients; native host callbacks continued | Economy transactions, action authority, occupied-sector/player capacity |
| Host lifecycle | Real callback regression failed on original source and passed with existing-world preservation; bounded runtime retained universe/native progression | General game save recovery or connected gameplay |
| Windows menu | User confirmed in-game picker opens, controls survive resizing, snapshot sync/load reaches the universe | Actual network admission/streaming; a loaded save is not a live connection |
| Native compatibility | GCC/Linux and MSVC/Windows builds; real loopback pending-connect setup; session/name restoration and load/readiness guards | Interactive Linux client, all ABI revisions or multi-user correctness |
| Transport | Host-local bounded JOIN probe received WELCOME; a separately attempted SSH-forward path was denied by server policy | End-to-end client handshake, rendering host streams or reconnect/sector mapping |
| Managed Docker bootstrap | Docker-format image built from source; bounded non-root/read-only/offline CPU Vulkan and mounted licensed-resource preparation passed; official SteamCMD download and Linux-native console startup passed without login | Fully authenticated fresh game download, authenticated NoSteam URL retrieval, new managed-image universe/save/restart qualification |

An isolated two-CPU/12-GiB software-rendered host baseline progressed close to real
time with zero gameplay clients. Recorded peak container memory was roughly
8.6-8.9 GiB in those runs. These are bounded observations on one hardware setup,
not minimum system requirements or player-count claims. Draw suppression reduced
cost in a same-world comparison while still initializing Vulkan; rendering-dependent
behavior remains unqualified. No proprietary assets or private setup/logs are
included in this contribution.

## Reproducible source checks

Each native example has its own CMake project:

```sh
cmake -S examples/x4mp -B build-host -DCMAKE_BUILD_TYPE=Release
cmake --build build-host --parallel 2
ctest --test-dir build-host --output-on-failure
cmake -S examples/x4mp_join -B build-join -DCMAKE_BUILD_TYPE=Release
cmake --build build-join --parallel 2
ctest --test-dir build-join --output-on-failure
python3 tools/test_join_service.py
python3 -m unittest discover -s docker/tests -p 'test_*.py'
luajit docker/tests/test_host_runtime.lua
```

On Windows, use a fresh MSVC developer environment and corresponding CMake
generator; do not reuse tracked build caches. The top-level framework CMake in
this fork is Linux-specific despite inherited Windows documentation. The companion
release has `tests/join_menu.lua`, which stubs game/GUI boundaries and runs the
actual shared menu source without desktop input or a game process.

Native tests cover existing-world readiness/wait/retry/duplicate activation,
continuing callbacks, Windows/POSIX pending errors, bounded reconnects, menu-role/
address/name restoration, duplicate-load prevention and strict second-pass gating.
Bridge tests cover bounded input, request identity, cancellation and replay after
UI rebuild. The adapter tests cover checksum/path/size bounds, gzip integrity,
atomic promotion/previous-save preservation, cancellation and real native greeting.
Container tests use inert executable/installer fixtures; they never start X4.

## Required real acceptance before release

1. Authenticated fresh Linux acquisition of the matching game/DLC and official
   NoSteam executable, with restart/cache reuse and no credentials in output/image.
2. Managed image loads an existing/new world, saves on schedule and acknowledges
   a real stop save, then reloads without changing accepted persistent state.
3. Definitive MD readiness survives/replays across UI rebuild without a timeout
   or player-validity shortcut.
4. Windows and Linux clients request, receive approval, sync/load, handshake,
   observe native streams and sector mappings, disconnect and reconnect while
   the host universe retains ownership/progress.
5. Unauthorized save access/admission, forged roles, duplicate approvals,
   expiry/revocation, admin disconnect/reconnect and owner recovery are rejected/
   handled by the server, not cosmetic client flags.
6. Occupied-sector load and gameplay outcomes are measured with bounded resources
   and independent checks on other services. Preserve the first failure and stop
   only the owned workload. Image import/transfer need separate impact budgets.

Keep the PRs as drafts until the remaining protocol, packaging and runtime
acceptance gaps are addressed. Build/fixture checks must not be presented as
successful multi-user gameplay or unattended production deployment.
