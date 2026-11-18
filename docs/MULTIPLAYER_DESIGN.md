# Multiplayer design decisions

These choices describe the experimental host/join work, not a claim that a full
multiplayer conversion is complete. Existing SDK/event architecture remains in
[ARCHITECTURE.md](ARCHITECTURE.md).

## 1. Keep the game engine authoritative

Use the existing X4 engine to simulate one host-owned universe. The host owns
clock, persistent world state and accepted gameplay outcomes. Clients render,
submit controls and may reconcile provisional local behavior. Disconnecting a
client must not replace the universe or transfer ownership. Preserve X4's normal
attention/detail optimizations; forcing every object into high-detail simulation
would waste resources and is not a requirement.

This avoids a speculative engine rewrite or deterministic lockstep assumption.
Existing hybrid combat/action replication still needs authority audits. A working
zero-client engine does not prove shared economy, player capacity or persistence.

## 2. Preserve existing worlds and definitive readiness

Calling NewMultiplayerGame when a valid universe is already loaded replaces it
and reloads the UI, losing menu-selected intent. The focused lifecycle fix waits
for definitive universe readiness, activates transport/streams and retains the
world. No-world startup retains its original behavior; duplicate activation is
still guarded. The regression exercises the actual callbacks, not a rewritten model.

X4 loads saves in multiple passes. A player ID can exist before background world
construction finishes. Neither a timeout, valid player nor a restored session
may become proof of the second pass. Native menu role/address/name and load intent
survive re-discovery in the process stash; world readiness still needs the real
MD universe-generated event. Delivery/replay across UI rebuild remains open.

## 3. Keep all player-facing interaction inside X4

Server selection/address entry, join requests, save-sync progress, cancellation,
errors/retry, graphical-client hosting and future admin notifications/controls
belong inside X4 on both platforms. Background services may perform networking
and transfer but must not impose a separate player-facing launcher or dialog.
Initial server deployment/authentication and developer diagnostics are operational
tasks, not ordinary player interaction.

Menu definitions are rebuilt after resizing/reloading. Reapply entries idempotently
and preserve existing handlers. The old injector can install late, so its Join
callback must delegate to the new controller rather than bypassing sync.

## 4. Request, authorize, sync, load, admit

The intended sequence is request entry, validate identity/capabilities, obtain
server-owned approval where required, sync a consistent compatible snapshot,
validate/promote it atomically, load it, then complete native readiness/admission.
Transfer workers never mutate game state; verified work is handed to the UI lifecycle.
Request IDs prevent stale, duplicate or cancelled replies from starting a load.

The current authenticated SSH adapter is an owner-only development transport,
not a general admission service. A local forwarding listener can exist while its
remote channel is rejected. Require an actual native greeting before sync/load;
never treat a loaded snapshot as proof that the client sees a live shared world.
Preflight sends no READY or gameplay actions. Reconnect attempts are bounded to
avoid opening a channel every frame when a route is unavailable.

## 5. Separate visible names, identity and permission

Player name is a persisted in-game display setting. The current token-delimited
packet format supports 1-32 ASCII letters/digits/underscore/hyphen. Richer names
need explicit compatible encoding. Names can change or collide; they cannot
serve as credentials or transfer ship ownership/admin rights. Stable verified
identity and persistent ownership mapping remain server protocol work.

A headless host needs owner-designated admin clients to receive pending join
requests and approve/deny them in X4. The server verifies roles and owns final
decisions. The first connection never implicitly becomes admin. Requests need
expiry, replay/revocation behavior and recovery when all admins are offline.
These are requirements, not cosmetic fields or implemented authorization.

## 6. Portable, isolated packaging

Use the same Lua/join-state protocol on Windows/Linux and preserve native socket,
ABI and lifecycle boundaries. Build native sources in a Docker build stage;
acquire licensed assets/auth state only at runtime. Keep game data, profiles,
cache/secrets and ephemeral runtime separate. Configure the host with environment
variables, validate them before launch and preserve failed evidence.

CPU Vulkan plus optional draw suppression is a measured feasibility route, not
a renderer-free server. Test with explicit CPU/memory/time budgets and distinguish
installer, initialization, simulation, transport and actual connected gameplay.
See [HEADLESS_HOST.md](HEADLESS_HOST.md) and [VALIDATION.md](VALIDATION.md).
