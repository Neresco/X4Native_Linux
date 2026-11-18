# Research informing the multiplayer prototype

These are reference patterns, not imported implementations or proof that X4
supports another engine's server mode. No external example's setup instructions
are automatically adopted. The implemented decisions are in
[MULTIPLAYER_DESIGN.md](MULTIPLAYER_DESIGN.md); measured limits are in
[VALIDATION.md](VALIDATION.md).

| Primary reference | Useful lesson | Boundary for X4 |
| --- | --- | --- |
| [Nebula headless guide](https://github.com/NebulaModTeam/nebula/wiki/Setup-Headless-Server) and [mod API](https://github.com/NebulaModTeam/nebula/wiki/Nebula-mod-API) | Preserve simulation while using an engine-supported graphics boundary; explicit role/compatibility/region-scoped packets | Unity batch/no-graphics flags do not implement an X4 renderer bypass |
| [RimWorld Multiplayer source](https://github.com/rwmt/Multiplayer) and [developer notes](https://hackmd.io/@rimworldmultiplayer/dev-general) | Determinism, compatible command replay and divergence diagnostics need deliberate engineering | X4 determinism/hook coverage across platforms is unproven; identical saves alone do not establish lockstep |
| [Skyrim Together source](https://github.com/tiltedphoques/TiltedEvolution) and [build architecture](https://wiki.tiltedphoques.com/tilted-online/technical-documentation/build-guide) | Separate protocol/coordination from engine integration | A small coordinator does not replace X4's AI/economy/world simulation |
| [TES3MP source](https://github.com/TES3MP/TES3MP) | Test authority changes, late packets, persistence and script/state coverage explicitly | An open-engine fork exposes different integration options from X4's extension surface |
| [Factorio determinism discussion](https://www.factorio.com/blog/post/fff-76) | Bandwidth/determinism tradeoffs and recovery failures deserve reproducible tests | Native engine design is not automatically available to a retrofit mod |

For transport/replication comparisons, Glenn Fiedler's
[deterministic lockstep](https://gafferongames.com/post/deterministic_lockstep/),
[snapshot interpolation](https://gafferongames.com/post/snapshot_interpolation/) and
[state synchronization](https://gafferongames.com/post/state_synchronization/)
describe different tradeoffs. Existing X4 reconciliation is closer to authoritative
state correction than proven deterministic lockstep. Treat buffering, priorities,
quantization and delta encoding as measured experiments rather than arbitrary defaults.
[GameNetworkingSockets](https://github.com/ValveSoftware/GameNetworkingSockets)
provides transport capabilities, not entity authority/serialization by itself.
Replacing TCP would not fix entity lifetime, ownership or divergence.

## Contracts still needing an audit

- Stable entity ID plus generation/lifetime; explicit spawn/bind/despawn/rebind,
  without relying only on same-macro/nearest-position matching.
- Server ownership of ships, cargo, money, stations, kills/captures and persistent
  script outcomes; validate client intentions and reject duplicate accepted actions.
- Session/action IDs, sequence/acknowledgement policy, bounded deduplication and queues.
- Distinguish simulation time, packet sequence and visual presentation time.
- Sector/region subscriptions with a consistent baseline and transition overlap.
- Snapshot/world revision provenance, version/DLC/mod negotiation and restart policy
  for transient proxies versus persistent authoritative state.
- Bounded per-frame work/catch-up bursts and observable stale updates, bytes,
  correction distance, snapshot age, duplicate bindings and queue depth.

Useful implementation failures already reproduced here: replacing an existing
host universe; losing menu-selected role during re-discovery; missing the second-pass
readiness event; Windows pending-connect mismatch; late UI wrappers bypassing sync;
and a local SSH listener masking a denied remote route. Each has either a focused
regression/correction or an explicit open acceptance item. A loaded snapshot must
never be used as a substitute for a verified live shared-world connection.
