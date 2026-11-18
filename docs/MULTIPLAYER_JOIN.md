# In-game joining and coordinator integration

The companion release package loads a shared `x4mp_join_menu.lua` after the
original menu script. It opens a saved-server/manual-address picker inside X4,
persists Player name, displays status and supports cancellation before loading.
Original Host behavior remains in-game. See [design decisions](MULTIPLAYER_DESIGN.md).

Build the optional bridge from [examples/x4mp_join](../examples/x4mp_join/README.md).
Its platform-neutral API-v1 code exchanges bounded local messages with a configured
background coordinator, emits Lua status on UI polls and does no worker-thread
game mutation. A fresh owner-private bridge directory identifies a new process;
UI rebuilds retain the current request while a new game process clears stale intent.

The bridge README defines catalog/request/cancel/status formats and deployment.
The example [tools/join_service.py](../tools/join_service.py) implements owner-authenticated
SSH metadata/snapshot transfer. It is portable Python stdlib code with configurable
command arrays; no shell command is supplied by the player. The address field must
match an operator-configured profile. Its JSON configuration includes:

| Key | Meaning |
| --- | --- |
| `bridge_directory` | Same fresh private directory as native `X4MP_JOIN_DIRECTORY` |
| `save_directory`, `native_log` | Selected game profile's save directory and framework log |
| `game_version`, `dlc` | Expected exact version and DLC-ID set |
| `connect_ip`, `connect_port` | Native transport endpoint already provisioned by the operator |
| `servers` | Profile list: display `name`, selected `ip`, authenticated `ssh_command` array, `status_command`, allowed `snapshot_root` |
| `maximum_seconds` | Bounded coordinator lifetime during development |

The status provider returns `phase`, game/DLC metadata, a consistent snapshot's
absolute path/byte count/SHA-256, world progress and current client count. Paths
must stay under the configured snapshot root. A production coordinator needs
authenticated admission, secure snapshot delivery and automatic lifecycle/service
packaging; a configured SSH adapter is not that implementation.

Before download, the adapter requires a native WELCOME through the real route,
without sending READY or actions. It checks download bounds, checksum and bounded
gzip integrity, preserves the previous save, then promotes the new file atomically.
Only a current verified response lets Lua refresh the game's save list, check the
slot and invoke native joining. Real native universe readiness plus host presence
are required for a connected status. Windows nonblocking-connect pending results
are handled explicitly and reconnects back off rather than looping each frame.

## Open work

- Package and supervise the coordinator without an external player workflow.
- Reliable delivery/replay of the definitive MD event across UI re-discovery.
- General server selection/discovery, capability/mod negotiation and secure admission.
- Verified identity, admin roles/queue, collision/rename policy and ownership mapping.
- Full cancel/disconnect, reconnect/sector mapping and action-authority acceptance.
- Interactive Linux client coverage; Windows/Linux builds are a separate evidence layer.

The client user can load a valid host snapshot while transport is broken. Report
that as a partial flow, never connected gameplay. Keep the feature as a draft
until these dependencies and acceptance cases are addressed.
