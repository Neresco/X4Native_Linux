# In-game multiplayer join bridge (experimental)

This optional API-v1 extension connects X4's in-game join UI to a background
join coordinator. The same source builds on Windows and Linux. It does not
implement a public server, authentication, admin roles or snapshot networking.
The coordinator must perform authorization, compatibility and integrity checks
before reporting a snapshot as verified. There is no legacy direct-join fallback.

Build this directory with CMake, then package `extension/content.xml`, the
platform's `x4native.*.json` renamed to `x4native.json`, and the resulting
`native/x4mp_join.dll` or `native/x4mp_join.so`. The matching X4MP Lua package
must load `ui/x4mp_join_menu.lua` after `ui/x4mp_menu.lua`. API-v1 prefix fields
are used; validate the exact game/framework combination before deployment.

## Local coordinator contract, version 1

Set `X4MP_JOIN_DIRECTORY` to an existing absolute, owner-private directory when
starting the game. Without configuration the UI reports an unavailable service.
No credentials belong in these messages. Use a fresh directory per game process
and have the coordinator running before the game. This is deployment setup,
not an external interface the player must operate during a game.

| File | Producer | Contents |
| --- | --- | --- |
| `catalog.txt` | Coordinator | One `id\|display name\|address` record per line |
| `request.json` | UI bridge | `{"id":"join-1","ip":"example.net"}` |
| `cancel.json` | UI bridge | `{"id":"join-1"}` |
| `status.txt` | Coordinator | `request-id\|phase\|message\|native-address\|save-slot` |

Publish complete files by atomic replacement. The consumer claims request and
cancel files by rename before processing, so the UI can publish another message.
Catalog/status reads are capped at 16 KiB. Messages use UTF-8; field delimiters,
newlines and NUL bytes cannot appear inside a field. These local files are not
an authentication boundary against other software running as the same user.

Phases are `requesting`, `waiting`, `syncing`, `validating`, `verified`,
`connected`, `error`, `disconnected`. A verified response must name the current
request, a compatible native endpoint and the configured `save_001` slot.
The current native X4MP extension must be launched with `X4MP_SAVE=save_001`;
its existing `X4MP_STREAM_PORT` configures the native connection port.
The coordinator atomically installs the validated `save_001.xml.gz` in the
game's selected private profile before publishing `verified`.

Lua refreshes the game save list, checks validity and raises the existing
native join request from a UI callback. A stale or cancelled response cannot
start a load. Duplicate verification cannot load the same request twice.
Status is replayed on UI polls to survive menu rebuilds. No game API is called
from a worker/network callback. Cancellation currently covers waiting and
transfer/validation; once world loading begins the existing native lifecycle
owns connection/teardown. Do not advertise that as complete disconnect support.

Run CTest for the native bridge boundaries. The release package's
`tests/join_menu.lua` exercises menu rebuilding, load order and join-state
regressions with game boundaries stubbed. Those checks do not prove visual
acceptance, Linux gameplay, admin authorization or a connected universe.
