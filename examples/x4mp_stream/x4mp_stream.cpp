// ==========================================================================
// x4mp_stream - Client-side server->client object stream handler for X4MP.
//
// This extension runs ONLY on the CLIENT. It owns a dedicated TCP listener
// (default port 7778) that the host connects to and streams the object/player
// data (OBJ and PLAYER messages) over as a RELIABLE byte stream (ordered,
// lossless — no UDP loss/reorder stalls). It renders it with SMOOTHING:
//   * interpolation between received positions (no teleporting / stutter)
//   * dead-object pruning (never touch destroyed components)
//   * batched, rate-limited rendering on the game's main thread
//
// The control channel (handshake, INPUT, SNAP camera, PING/PONG) remains in
// x4mp.so on port 7777. This module is purely the server->client render path.
// ==========================================================================
#include <x4native_extension.h>
#include <x4_game_func_table.h>

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cmath>
#include <thread>
#include <mutex>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <string>
#include <chrono>

#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <cerrno>

static X4NativeAPI*    g_api  = nullptr;
static X4GameFunctions* g_game = nullptr;

// The game loads saves in TWO passes. on_game_loaded fires after the 1st pass
// (entity IDs valid, but the universe is NOT fully built). Spawning/positioning
// objects during the 2nd pass races the game's background "Movement worker"
// thread and corrupts the object vectors -> SIGSEGV. So we only render once
// on_universe_ready fires (after the 2nd pass / event_universe_generated).
static volatile bool g_ready = false;   // set true on on_universe_ready

// ---- Config (env overrides) ----------------------------------------------
static int   g_stream_port   = 7778;   // X4MP_STREAM_PORT
static std::string g_transport = "tcp"; // X4MP_TRANSPORT: tcp (default) or udp
static bool  g_legacy_net    = false;  // X4MP_LEGACY_NET: 1=own socket/recv (legacy), 0=x4mp owns the connection (DEFAULT)
static int   g_render_interval = 3;    // frames between render passes (20Hz @60fps)
static float g_smooth_tau    = 0.12f;  // interpolation time constant (seconds)

// ---- Remote object state --------------------------------------------------
struct RemoteObj {
    unsigned long long host_id = 0;
    char macro[160] = {0};
    char faction[80] = {0};
    char sector_macro[160] = {0}; // host sector macro name -> client sector
    // interpolation state
    float tx=0, ty=0, tz=0, tyaw=0, tpitch=0, troll=0; // latest target
    float px=0, py=0, pz=0;                            // previous position
    std::chrono::steady_clock::time_point last_update;
    UniverseID client_id = 0;                          // spawned ghost (0 = none)
    bool has_target = false;
    bool spawned = false;
    bool is_player = false;                            // true for PLAYER ghosts (human ships)
};

static std::unordered_map<unsigned long long, RemoteObj> g_objs;
static std::mutex g_mutex;

// ---- Reconciliation: host<->local ship binding ---------------------------
// The client keeps its own locally simulated universe (the synced save). In
// the client's CURRENT sector (its rendering zone, fully simulated locally so
// ships behave normally), each host-streamed ship is BOUND to the matching
// local ship (same macro, nearest) and the local ship is pinned to the
// interpolated host trajectory every render pass — all clients + host show
// the same ship state at any speed. Local ships
// the host no longer reports (died on the host) are removed after a grace
// period. This keeps every client — and the host — showing the same ship
// positions, while the local simulation keeps the ships alive and natural.
static UniverseID g_index_sector = 0;   // sector the local index covers
static std::vector<UniverseID> g_index_ships;  // local ships in that sector
static std::unordered_map<std::string, std::vector<size_t>> g_index_by_macro; // macro -> indices
static std::unordered_map<unsigned long long, UniverseID> g_bindings;  // host id -> local id
static std::unordered_set<UniverseID> g_bound_locals;                  // local ids bound
static std::unordered_map<UniverseID, std::chrono::steady_clock::time_point> g_missing_since; // not seen from host
static bool g_full_received = false;   // full snapshot for the current sector received
static std::chrono::steady_clock::time_point g_last_recv_any; // last stream data (link alive)
static float g_bind_radius = 1000.0f;  // X4MP_BIND_RADIUS: re-match distance after a local death (mid-sector)
static float g_converge_radius = 20000.0f; // X4MP_CONVERGE_RADIUS: match distance on sector entry
static float g_max_lag_m = 300.0f;     // X4MP_MAX_LAG_M: max interpolation lag behind the host
static bool g_converged = false;        // entry convergence done for the current sector
// X4MP_CONVERGE_GREEDY (default 1): on sector entry, bind each host ship to the
// NEAREST same-macro local ship regardless of distance. Inactive sectors diverge
// from the host while the client is elsewhere, so a fixed converge radius ghosts
// the diverged ships (the transition flicker). Greedy binding snaps them to the
// host position instead (a one-time snap, masked by the sector change).
static bool g_converge_greedy = true;
static const int g_missing_prune_ms = 10000; // remove a local ship missing from the host for this long
static const int g_link_alive_ms = 15000;    // no stream data for this long = link dead (no pruning)
static bool g_debug = false;             // X4MP_DEBUG=1
// X4MP_INERT (default 1): stop the local simulation of host-driven ships so the
// client's own AI cannot dock/move/destroy them and diverge from the host (the
// source of the prune/respawn flicker). They are still rendered and targetable;
// their position is driven by the host pin. X4MP_INERT=0 restores local AI.
static bool g_inert = true;
// Convergence diagnostics (reset per sector entry, logged once under X4MP_DEBUG).
static unsigned g_cv_host = 0, g_cv_bound = 0, g_cv_ghost = 0;
static float g_cv_maxd = 0.0f;
static bool g_cv_logged = false;
// Auto-fly test hook (X4MP_AUTOFLY=1): periodically move the client's player to
// a different sector to exercise the sector-transition logic (index rebuild,
// convergence, binding) without manual driving. Testing only — leave off in use.
static bool g_autofly = false;
static int g_autofly_interval_s = 10;
static int g_autofly_idx = 0;
static std::vector<UniverseID> g_autofly_sectors;
static std::chrono::steady_clock::time_point g_autofly_last{};
// Flicker diagnostics: count each mechanism that removes/replaces a ship so we
// can see WHICH one is churning (reported every ~15s under X4MP_DEBUG).
static unsigned g_flk_stale = 0;     // host object aged out (not seen 30s)
static unsigned g_flk_zone  = 0;     // ghost dropped: obj sector != client sector
static unsigned g_flk_bindrel = 0;   // binding released: local ship became invalid
static unsigned g_flk_death = 0;     // ghost died locally (respawned)
static unsigned g_flk_spawn = 0;     // ghost spawned
static unsigned g_flk_prune = 0;     // local ship pruned (not reported by host)

// Host sector MACRO name -> client sector UniverseID. Built once on
// universe_ready (the client loads the same save, so sector macros match).
static std::unordered_map<std::string, UniverseID> g_sector_map;

// Resolve an object's MACRO name (for building the sector map).
// GetComponentName returns the DISPLAY name ("Argon Prime"), which does NOT
// match the host's streamed sector macro ("cluster_113_sector001_macro"). We
// must query the "macro" property via the Lua API, exactly as the host does,
// so the client's sector-map keys match the host's streamed macros.
static void get_macro(UniverseID id, char* out, size_t outsize) {
    out[0] = 0;
    if (g_api && g_api->get_lua_property_str) {
        X4nLuaKey k{};
        k.type = X4N_KEY_UINT64;
        k.v.u = id;
        if (g_api->get_lua_property_str("GetComponentData", k, "macro", out, outsize))
            return;
    }
    if (g_game && g_game->GetComponentName) {
        const char* n = g_game->GetComponentName(id);
        if (n) snprintf(out, outsize, "%s", n);
    }
}

// Enumerate the client's own sectors (same save as host) and build a
// macro-name -> sector-id map so host-streamed objects can be placed in the
// correct client sector.
//
// Enumerate ALL ships/stations of a faction with no arbitrary buffer cap.
// A fixed 2048-entry stack buffer silently dropped the overflow for large
// factions, so the local index / sector map were incomplete and ships beyond
// the cap were never bound (and got pruned as "missing"). Sizing from
// GetNumAllFactionShips/Stations and heap-allocating removes the cap.
static uint32_t enum_faction_ships(const char* faction, std::vector<UniverseID>& out) {
    if (!g_game || !g_game->GetAllFactionShips) return 0;
    uint32_t cap = 4096;
    if (g_game->GetNumAllFactionShips) cap = std::max<uint32_t>(cap, g_game->GetNumAllFactionShips(faction));
    if (cap == 0) return 0;
    out.resize(cap);
    return g_game->GetAllFactionShips(out.data(), cap, faction);
}
static uint32_t enum_faction_stations(const char* faction, std::vector<UniverseID>& out) {
    if (!g_game || !g_game->GetAllFactionStations) return 0;
    uint32_t cap = 1024;
    if (g_game->GetNumAllFactionStations) cap = std::max<uint32_t>(cap, g_game->GetNumAllFactionStations(faction));
    if (cap == 0) return 0;
    out.resize(cap);
    return g_game->GetAllFactionStations(out.data(), cap, faction);
}

// GetSectorsByOwner is unreliable on a paused thin client (ownership data is
// not computed). Instead we derive the sector set from the client's own loaded
// objects (ships + stations) via GetContextByClass, which works regardless of
// pause state. Called on universe_ready, before the ship cleanup runs.
static void build_sector_map() {
    g_sector_map.clear();
    if (!g_game || !g_game->GetAllFactions || !g_game->GetAllFactionShips) return;
    const char* factions[64];
    uint32_t nf = g_game->GetAllFactions(factions, 64, true);
    auto add_sector = [&](UniverseID obj) {
        if (!obj) return;
        UniverseID sid = g_game->GetContextByClass ? g_game->GetContextByClass(obj, "sector", false) : 0;
        if (!sid) return;
        char m[160]; get_macro(sid, m, sizeof(m));
        if (m[0]) g_sector_map[m] = sid;
    };
    uint32_t iterated = 0, found = 0;
    std::vector<UniverseID> objs;
    for (uint32_t f = 0; f < nf; f++) {
        uint32_t ns = enum_faction_ships(factions[f], objs);
        for (uint32_t i = 0; i < ns; i++) { iterated++; add_sector(objs[i]); }
        if (g_game->GetAllFactionStations) {
            uint32_t nst = enum_faction_stations(factions[f], objs);
            for (uint32_t i = 0; i < nst; i++) { iterated++; add_sector(objs[i]); }
        }
    }
    if (g_api) {
        char m[256];
        snprintf(m, sizeof(m), "x4mp_stream: sector map built: %zu sectors (iterated %u objs)", g_sector_map.size(), iterated);
        g_api->log(X4NATIVE_LOG_INFO, m);
        // Sample a few mapped sectors to verify macro names match the host.
        uint32_t shown = 0;
        for (auto& kv : g_sector_map) {
            if (shown++ >= 5) break;
            char s[256];
            snprintf(s, sizeof(s), "x4mp_stream: map sample: %s -> %llu", kv.first.c_str(), (unsigned long long)kv.second);
            g_api->log(X4NATIVE_LOG_INFO, s);
        }
    }
}

// All known IDs of the player's own ship(s) in ANY state (flying, docked,
// in a hangar). Binding, convergence and pruning must NEVER touch these:
// removing the player ship triggers "Game Over (killmethod=removed)" and the
// game aborts to the main menu (the client save-reload loop).
struct PlayerShipIDs {
    UniverseID ids[5] = {0, 0, 0, 0, 0};
    PlayerShipIDs() {
        if (!g_game) return;
        ids[0] = g_game->GetPlayerControlledShipID ? g_game->GetPlayerControlledShipID() : 0;
        ids[1] = g_game->GetPlayerShipID ? g_game->GetPlayerShipID() : 0;
        ids[2] = g_game->GetPlayerOccupiedShipID ? g_game->GetPlayerOccupiedShipID() : 0;
        UniverseID po = g_game->GetPlayerObjectID ? g_game->GetPlayerObjectID() : 0;
        ids[3] = po;
        ids[4] = (po && g_game->GetContextByClass)
                     ? g_game->GetContextByClass(po, "ship", false) : 0;
    }
    bool contains(UniverseID s) const {
        for (int i = 0; i < 5; i++)
            if (ids[i] && ids[i] == s) return true;
        return false;
    }
};

// Enumerate the client's local ships in ONE sector and build the binding
// index (macro -> local ship ids). Called on the main thread when the client
// enters a new sector. The enumeration is C-level (GetContextByClass) for all
// ~85k ships (fast); the Lua macro lookup is only needed for the ships that
// are actually in the target sector (~hundreds).
static void rebuild_local_index(UniverseID sector) {
    g_index_ships.clear();
    g_index_by_macro.clear();
    g_bindings.clear();
    g_bound_locals.clear();
    g_missing_since.clear();
    g_index_sector = sector;
    if (!g_game || !g_game->GetAllFactions || !g_game->GetAllFactionShips ||
        !g_game->GetContextByClass) return;
    // Never bind/correct/remove the player's own ship(s) — in every state
    // (a DOCKED player returns 0 from GetPlayerControlledShipID, so the old
    // two-ID exclusion failed and pruning deleted the docked player ship).
    PlayerShipIDs pids;
    const char* factions[64];
    uint32_t nf = g_game->GetAllFactions(factions, 64, true);
    std::vector<UniverseID> ships;
    // Dedupe: the same ship can be returned by multiple factions' enumerations
    // (includehidden aliases), which would fill the index with duplicates.
    std::unordered_set<UniverseID> indexed;
    uint32_t dupes = 0;
    for (uint32_t f = 0; f < nf; f++) {
        uint32_t ns = enum_faction_ships(factions[f], ships);
        for (uint32_t i = 0; i < ns; i++) {
            UniverseID s = ships[i];
            if (pids.contains(s)) continue;
            if (g_game->IsValidComponent && !g_game->IsValidComponent(s)) continue;
            if (g_game->GetContextByClass(s, "sector", false) != sector) continue;
            char macro[160]; get_macro(s, macro, sizeof(macro));
            if (!macro[0] || macro[0] == '?') continue;
            if (!indexed.insert(s).second) { dupes++; continue; }
            size_t idx = g_index_ships.size();
            g_index_ships.push_back(s);
            g_index_by_macro[std::string(macro)].push_back(idx);
        }
    }
    // Phase 1 (persistent AI suppression): freeze the local simulation of every
    // ship in this sector so the client's own AI cannot dock/move/destroy them
    // and diverge from the host. They stay put for binding; their position is
    // driven by the host pin. The player's own ships are already excluded from
    // the index. Re-done on every sector entry (the game may re-activate ships
    // when the sector becomes live again).
    if (g_inert && g_game->ActivateObject) {
        for (UniverseID s : g_index_ships)
            g_game->ActivateObject(s, false);
    }
    if (g_api && g_api->log) {
        char m[256];
        snprintf(m, sizeof(m), "x4mp_stream: local index rebuilt for sector %llu: %zu ships, %u dupes dropped (inert=%d)",
                 (unsigned long long)sector, g_index_ships.size(), dupes, (int)g_inert);
        g_api->log(X4NATIVE_LOG_INFO, m);
    }
}

// ---- Network --------------------------------------------------------------
// TCP stream: we LISTEN on g_stream_port; the host CONNECTS to us and streams
// OBJ/PLAYER lines. g_listen_sock is the listening socket; g_data_sock is the
// accepted host connection (re-accepted automatically if the host reconnects).
static int g_listen_sock = -1;
static int g_data_sock = -1;
static std::thread g_recv_thread;
static volatile bool g_running = false;

static void net_log(const char* msg) {
    if (g_api && g_api->log) g_api->log(X4NATIVE_LOG_INFO, msg);
}

// ---- Receive thread: parse packets, store target positions ----------------
static void parse_line(char* line) {
    if (strncmp(line, "FULL", 4) == 0) {
        // Host marked the previous batch as a FULL sector snapshot: the
        // client's authoritative set for the current sector is complete, so
        // missing-ship pruning may start.
        g_full_received = true;
    } else if (strncmp(line, "OBJ", 3) == 0) {
        // OBJ <id> <sectormacro> <x> <y> <z> <yaw> <pitch> <roll> <faction> <macro>
        unsigned long long id;
        float x, y, z, yaw, pitch, roll;
        char sectormacro[160], faction[80], macro[160];
        if (sscanf(line, "OBJ %llu %159s %f %f %f %f %f %f %79s %159s",
                   &id, sectormacro, &x, &y, &z, &yaw, &pitch, &roll, faction, macro) == 10) {
            std::lock_guard<std::mutex> lk(g_mutex);
            RemoteObj& o = g_objs[id];
            if (o.has_target) { o.px = o.tx; o.py = o.ty; o.pz = o.tz; }
            o.host_id = id;
            snprintf(o.macro, sizeof(o.macro), "%s", macro[0] ? macro : "?");
            snprintf(o.faction, sizeof(o.faction), "%s", faction[0] ? faction : "player");
            snprintf(o.sector_macro, sizeof(o.sector_macro), "%s", sectormacro[0] ? sectormacro : "?");
            o.is_player = false;
            o.tx = x; o.ty = y; o.tz = z; o.tyaw = yaw; o.tpitch = pitch; o.troll = roll;
            o.last_update = std::chrono::steady_clock::now();
            o.has_target = true;
        }
    } else if (strncmp(line, "PLAYER", 6) == 0) {
        // PLAYER <cid> <x> <y> <z> <yaw> <pitch> <roll> <macro> <faction> [sectormacro]
        // faction is the player's UNIQUE faction (x4mp_host / x4mp_client_N),
        // so each player's ghost is owned by a distinct faction and can never
        // be confused with another player. The optional sector macro tells the
        // client WHICH sector the player is actually in (host and client load
        // the same save, so sector macros match).
        unsigned long long cid;
        float x, y, z, yaw, pitch, roll;
        char macro[160], faction[80], sectormacro[160] = {0};
        int nf = sscanf(line, "PLAYER %llu %f %f %f %f %f %f %159s %79s %159s",
                        &cid, &x, &y, &z, &yaw, &pitch, &roll, macro, faction, sectormacro);
        if (nf >= 9) {
            std::lock_guard<std::mutex> lk(g_mutex);
            RemoteObj& o = g_objs[cid];
            if (o.has_target) { o.px = o.tx; o.py = o.ty; o.pz = o.tz; }
            o.host_id = cid;
            snprintf(o.macro, sizeof(o.macro), "%s", macro[0] ? macro : "?");
            snprintf(o.faction, sizeof(o.faction), "%s", faction[0] ? faction : "player");
            if (nf >= 10 && sectormacro[0] && sectormacro[0] != '?')
                snprintf(o.sector_macro, sizeof(o.sector_macro), "%s", sectormacro);
            else
                o.sector_macro[0] = 0;
            o.is_player = true;
            o.tx = x; o.ty = y; o.tz = z; o.tyaw = yaw; o.tpitch = pitch; o.troll = roll;
            o.last_update = std::chrono::steady_clock::now();
            o.has_target = true;
        }
    }
}

// Event bridge (consolidated one-port-per-transport design): x4mp owns the
// network connection and raises "x4mp_stream.data" with each OBJ/PLAYER line it
// receives from the host. This feeds the line into the SAME parse_line() the old
// dedicated recv thread used, so reconciliation (render_pass) is unchanged.
// Synchronous: the line is valid for the duration of the raise (it lives on
// x4mp's recv stack). Only used when X4MP_LEGACY_NET=0 (legacy keeps recv_loop).
static void on_stream_data(const char* /*event_name*/, void* data, void* /*userdata*/) {
    if (data) {
        // Consolidated mode: data arrives via the event bridge, not recv_loop,
        // so refresh the link-alive timestamp here (legacy recv_loop does it
        // itself). Without this, link_alive would stay false forever and the
        // missing-ship prune (and its safety gate) would never run.
        g_last_recv_any = std::chrono::steady_clock::now();
        parse_line((char*)data);
    }
}

// Receive thread: accept the host's TCP connection and read the reliable byte
// stream, splitting it into newline-delimited lines. TCP may deliver a line
// split across reads or several lines in one read, so we accumulate in a
// buffer and only parse complete lines. poll() timeouts keep the loop
// responsive to shutdown and to (re)connecting.
static void recv_loop() {
    std::string acc;   // accumulates partial lines across reads
    char buf[65536];
    while (g_running) {
        // UDP: no connection — recvfrom the bound socket directly (datagrams
        // can be dropped/reordered; the host resends the full state each tick).
        if (g_transport == "udp") {
            struct pollfd pfd; pfd.fd = g_listen_sock; pfd.events = POLLIN; pfd.revents = 0;
            int pr = poll(&pfd, 1, 100);
            if (pr < 0) { if (errno == EINTR) continue; continue; }
            if (pr == 0) continue;
            struct sockaddr_in from; socklen_t fl = sizeof(from);
            ssize_t n = recvfrom(g_listen_sock, buf, sizeof(buf), 0, (struct sockaddr*)&from, &fl);
            if (n <= 0) { if (errno == EINTR) continue; continue; }
            acc.append(buf, (size_t)n);
            g_last_recv_any = std::chrono::steady_clock::now();
            size_t start = 0;
            for (;;) {
                size_t nl = acc.find('\n', start);
                if (nl == std::string::npos) break;
                acc[nl] = 0;
                if (nl > start) parse_line(acc.data() + start);
                start = nl + 1;
            }
            if (start > 0) acc.erase(0, start);
            if (acc.size() > 2000000) acc.clear();
            continue;
        }
        // TCP: (re)accept the host's connection, then recv.
        if (g_data_sock < 0) {
            // (Re)accept the host's TCP connection.
            if (g_listen_sock < 0) { usleep(2000); continue; }
            struct pollfd pfd; pfd.fd = g_listen_sock; pfd.events = POLLIN; pfd.revents = 0;
            if (poll(&pfd, 1, 100) <= 0) continue; // timeout / error: re-check g_running
            struct sockaddr_in from; socklen_t fl = sizeof(from);
            int fd = accept(g_listen_sock, (struct sockaddr*)&from, &fl);
            if (fd < 0) { if (errno == EINTR) continue; continue; }
            int one = 1;
            setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
            g_data_sock = fd;
            acc.clear();
            g_last_recv_any = std::chrono::steady_clock::now();
            net_log("x4mp_stream: TCP stream connected (host)");
            continue;
        }
        struct pollfd pfd; pfd.fd = g_data_sock; pfd.events = POLLIN; pfd.revents = 0;
        int pr = poll(&pfd, 1, 100);
        if (pr < 0) { if (errno == EINTR) continue; close(g_data_sock); g_data_sock = -1; acc.clear(); continue; }
        if (pr == 0) continue; // timeout: re-check g_running
        if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
            close(g_data_sock); g_data_sock = -1; acc.clear();
            net_log("x4mp_stream: TCP stream closed by host; waiting for reconnect");
            continue;
        }
        ssize_t n = recv(g_data_sock, buf, sizeof(buf), 0);
        if (n < 0) { if (errno == EINTR) continue; close(g_data_sock); g_data_sock = -1; acc.clear(); continue; }
        if (n == 0) { close(g_data_sock); g_data_sock = -1; acc.clear(); continue; }
        acc.append(buf, (size_t)n);
        g_last_recv_any = std::chrono::steady_clock::now();
        // Parse every complete line; keep the trailing partial line for next read.
        size_t start = 0;
        for (;;) {
            size_t nl = acc.find('\n', start);
            if (nl == std::string::npos) break;
            acc[nl] = 0;
            if (nl > start) parse_line(acc.data() + start);
            start = nl + 1;
        }
        if (start > 0) acc.erase(0, start);
        if (acc.size() > 2000000) acc.clear(); // safety valve (runaway / desync)
    }
}

// ---- Main-thread render pass ----------------------------------------------
// Interpolate each ghost toward its target and position it. Only touches the
// game API from the main thread (called from on_frame_update).
static void render_pass() {
    if (!g_game) return;
    // Do NOT touch the object system until the universe is fully built (2nd
    // load pass done). Spawning earlier races the Movement worker -> SIGSEGV.
    if (!g_ready) return;
    if (!g_game->SpawnObjectAtPos2 || !g_game->SetObjectSectorPos) return;
    if (!g_game->GetObjectPositionInSector || !g_game->IsValidComponent) return;
    if (!g_game->GetPlayerZoneID || !g_game->GetContextByClass) return;

    UniverseID player_zone = g_game->GetPlayerZoneID();
    if (player_zone == 0) return;
    UniverseID client_sector = g_game->GetContextByClass(player_zone, "sector", false);
    if (!client_sector) return;

    // Sector change: rebuild the local-ship index for the new rendering zone
    // (binding + correction operate only within the current sector) and wait
    // for a fresh FULL snapshot before any missing-ship pruning.
    if (client_sector != g_index_sector) {
        rebuild_local_index(client_sector);
        g_full_received = false;
        g_converged = false;
        g_cv_host = 0; g_cv_bound = 0; g_cv_ghost = 0; g_cv_maxd = 0.0f; g_cv_logged = false;
    }

    auto now = std::chrono::steady_clock::now();
    bool link_alive = (now - g_last_recv_any) < std::chrono::milliseconds(g_link_alive_ms);

    std::lock_guard<std::mutex> lk(g_mutex);
    std::vector<unsigned long long> dead;
    static unsigned dbg_tick = 0;
    static unsigned spawned_total = 0;
    unsigned spawned_now = 0;
    unsigned bound_now = 0, corrected = 0, pruned = 0;
    if ((++dbg_tick % 300) == 0) {
        char dbg[256];
        // Show the target of the first tracked object so we can see if the
        // host is sending changing positions (movement) or static ones.
        float sx = 0, sy = 0, sz = 0; unsigned long long sid = 0;
        for (auto& kv : g_objs) { sid = kv.first; sx = kv.second.tx; sy = kv.second.ty; sz = kv.second.tz; break; }
        snprintf(dbg, sizeof(dbg), "x4mp_stream: [DBG] objs=%zu ghosts=%u total=%u sector=%llu index=%zu bindings=%zu full=%d link=%d sample=%llu pos=(%.1f,%.1f,%.1f)",
                 g_objs.size(), spawned_now, spawned_total, (unsigned long long)client_sector,
                 g_index_ships.size(), g_bindings.size(), (int)g_full_received, (int)link_alive,
                 sid, sx, sy, sz);
        net_log(dbg);
        // Flicker rate over the last ~15s (300 ticks). High numbers in any
        // column = that mechanism is churning ships (the flicker source).
        static unsigned p_stale=0,p_zone=0,p_bindrel=0,p_death=0,p_spawn=0,p_prune=0;
        char flk[256];
        snprintf(flk, sizeof(flk), "x4mp_stream: [FLK] /15s: stale=%u zone=%u bindrel=%u death=%u spawn=%u prune=%u",
                 g_flk_stale-p_stale, g_flk_zone-p_zone, g_flk_bindrel-p_bindrel,
                 g_flk_death-p_death, g_flk_spawn-p_spawn, g_flk_prune-p_prune);
        p_stale=g_flk_stale; p_zone=g_flk_zone; p_bindrel=g_flk_bindrel;
        p_death=g_flk_death; p_spawn=g_flk_spawn; p_prune=g_flk_prune;
        net_log(flk);
    }
    for (auto& kv : g_objs) {
        RemoteObj& o = kv.second;
        if (!o.has_target) continue;
        // Prune objects not updated for a while (host stopped sending / destroyed).
        // Long timeout so stationary objects (not re-sent due to delta compression)
        // do not flicker. The host sends periodic full refreshes (every ~5s) to
        // keep the set authoritative; this only clears objects truly gone.
        auto age = std::chrono::duration_cast<std::chrono::milliseconds>(now - o.last_update).count();
        if (age > 30000) {
            // Release any binding so the local ship becomes a prune candidate,
            // and remove a spawned ghost of a ship that no longer exists.
            auto bit = g_bindings.find(kv.first);
            if (bit != g_bindings.end()) {
                g_bound_locals.erase(bit->second);
                g_bindings.erase(bit);
            }
            if (o.spawned && o.client_id && g_game->RemoveComponent &&
                g_game->IsValidComponent && g_game->IsValidComponent(o.client_id))
                g_game->RemoveComponent(o.client_id);
            g_flk_stale++;
            dead.push_back(kv.first);
            continue;
        }

        // Interpolate: exponential chase toward the latest target. This smooths
        // movement without teleporting, at any host update rate.
        // Speed-adaptive: a fixed tau lags the host by v*tau (1.8 km behind at
        // highway speed 15 km/s with tau=0.12) — cap the steady-state lag at
        // g_max_lag_m by shrinking tau for fast objects.
        float dt = std::chrono::duration_cast<std::chrono::duration<float>>(now - o.last_update).count();
        float ddx = o.tx - o.px, ddy = o.ty - o.py, ddz = o.tz - o.pz;
        float dist = sqrtf(ddx*ddx + ddy*ddy + ddz*ddz);
        float v_est = dist / fmaxf(dt, 0.001f); // upper bound on speed (includes catch-up)
        float tau = g_smooth_tau > 0 ? g_smooth_tau : 0.12f;
        if (v_est > 1.0f) {
            float tau_fast = g_max_lag_m / v_est;
            if (tau_fast < tau) tau = tau_fast;
        }
        float alpha = 1.0f - expf(-dt / fmaxf(tau, 0.005f));
        if (alpha > 1.0f) alpha = 1.0f;
        float ix = o.px + (o.tx - o.px) * alpha;
        float iy = o.py + (o.ty - o.py) * alpha;
        float iz = o.pz + (o.tz - o.pz) * alpha;

        UIPosRot pos; pos.x = ix; pos.y = iy; pos.z = iz;
        pos.yaw = o.tyaw; pos.pitch = o.tpitch; pos.roll = o.troll;

        // Place the object in the client sector matching the host's sector
        // (same save => same sector macros). Fall back to the player sector if
        // the macro is unknown.
        UniverseID target_sector = client_sector;
        if (o.sector_macro[0]) {
            auto it = g_sector_map.find(o.sector_macro);
            if (it != g_sector_map.end()) target_sector = it->second;
        }

        // Zone-limited rendering: only render objects in the client's CURRENT
        // sector — its rendering zone, which the client's own game fully
        // simulates locally (ships behave normally). Objects in other sectors
        // are not rendered. OBJ ghosts from other sectors are dropped entirely:
        // spawning the whole universe (~85k ships x ~30 sub-objects) exhausted
        // the game's ID map ("AutoIDMap::Insert(): ID map is full" -> FATAL).
        // PLAYER ghosts are kept (few of them) so they reappear when the
        // client flies into that sector.
        if (target_sector != client_sector) {
            if (!o.is_player) {
                // Release any binding first: dropping the RemoteObj without
                // clearing g_bindings/g_bound_locals leaks the local ship into
                // g_bound_locals forever, where the prune loop skips it — the
                // ship then stays frozen in place indefinitely.
                auto bit = g_bindings.find(kv.first);
                if (bit != g_bindings.end()) {
                    g_bound_locals.erase(bit->second);
                    g_bindings.erase(bit);
                }
                // Remove a spawned ghost ship so it does not linger as an
                // orphan in the old sector (each ship ~30 IDs in the game's
                // ID map; orphans from many sector crossings would refill it).
                if (o.spawned && o.client_id && g_game->RemoveComponent &&
                    g_game->IsValidComponent && g_game->IsValidComponent(o.client_id))
                    g_game->RemoveComponent(o.client_id);
                g_flk_zone++;
                dead.push_back(kv.first);
            }
            continue;
        }

        // ---- Reconciliation (non-player objects only) ---------------------
        // Bind this host ship to the matching local ship and keep the local
        // ship on the host's position. PLAYER ghosts never bind (they are
        // human ships rendered as ghosts on top of the local universe).
        if (!o.is_player) {
            if (g_full_received && !g_converged) g_cv_host++;
            UniverseID local = 0;
            auto bit = g_bindings.find(kv.first);
            if (bit != g_bindings.end()) {
                local = bit->second;
                if (g_game->IsValidComponent && !g_game->IsValidComponent(local)) {
                    // Local ship died: unbind so it is re-bound or ghosted.
                    g_flk_bindrel++;
                    g_bindings.erase(bit);
                    g_bound_locals.erase(local);
                    local = 0;
                }
            }
            if (!local) {
                // Nearest unbound local ship with the same macro. On sector
                // ENTRY (before convergence) the local universe has been
                // simulated independently of the host's for a while, so
                // positions can differ by kilometres: use the generous
                // convergence radius and snap the local ship to the host's
                // state — the local ship then becomes the LIVE version of the
                // host's ship (natural behaviour, host-truth position).
                // Mid-sector (after convergence) only re-match near the host
                // position (a local death freed the binding).
                bool greedy = (g_full_received && !g_converged && g_converge_greedy);
                float radius = greedy ? 0.0f : ((g_full_received && !g_converged) ? g_converge_radius : g_bind_radius);
                float best_d2 = 0.0f;
                auto mit = g_index_by_macro.find(o.macro);
                if (mit != g_index_by_macro.end()) {
                    float best = greedy ? 1e30f : radius * radius;
                    for (size_t idx : mit->second) {
                        UniverseID cand = g_index_ships[idx];
                        if (g_bound_locals.count(cand)) continue;
                        if (g_game->IsValidComponent && !g_game->IsValidComponent(cand)) continue;
                        UIPosRot lp = g_game->GetObjectPositionInSector(cand);
                        float dx = lp.x - o.tx, dy = lp.y - o.ty, dz = lp.z - o.tz;
                        float d2 = dx*dx + dy*dy + dz*dz;
                        if (d2 < best) { best = d2; local = cand; }
                    }
                    best_d2 = best;
                }
                if (local) {
                    g_bindings[kv.first] = local;
                    g_bound_locals.insert(local);
                    g_missing_since.erase(local);
                    if (greedy) { g_cv_bound++; g_cv_maxd = fmaxf(g_cv_maxd, sqrtf(best_d2)); }
                    // Replace a previously spawned ghost of this host ship.
                    if (o.spawned && o.client_id && g_game->RemoveComponent)
                        g_game->RemoveComponent(o.client_id);
                    o.spawned = false;
                    o.client_id = 0;
                    // Snap the local ship to the host's current state.
                    UIPosRot snap; snap.x = o.tx; snap.y = o.ty; snap.z = o.tz;
                    snap.yaw = o.tyaw; snap.pitch = o.tpitch; snap.roll = o.troll;
                    g_game->SetObjectSectorPos(local, client_sector, snap);
                    bound_now++;
                }
            } else {
                // Bound: pin the local ship to the interpolated host
                // trajectory every pass. The local simulation still drives
                // AI/visuals, but position follows the host's truth — so all
                // clients + host show the same ship state at any speed
                // (a drift threshold can't keep up at 15 km/s).
                if (g_inert && g_game->ActivateObject) g_game->ActivateObject(local, false);
                g_game->SetObjectSectorPos(local, client_sector, pos);
                corrected++;
            }
            if (local) continue; // driven by the host; no ghost for this one
        }

        // ---- Ghost path (PLAYER ghosts + host-only ships) -----------------
        if (!o.spawned) {
            UniverseID newid = g_game->SpawnObjectAtPos2(o.macro, target_sector, pos,
                                                         (o.faction[0]) ? o.faction : "player");
            if (newid != 0) {
                o.client_id = newid;
                o.spawned = true;
                spawned_total++;
                g_flk_spawn++;
                if (g_full_received && !g_converged) g_cv_ghost++;
                if (spawned_total < 60 || (spawned_total % 30) == 0) {
                    char dbg[192];
                    snprintf(dbg, sizeof(dbg), "x4mp_stream: [DBG] SPAWN id=%llu macro=%s at (%.0f,%.0f,%.0f) sector=%s total=%u",
                             (unsigned long long)kv.first, o.macro, ix, iy, iz, o.sector_macro, spawned_total);
                    net_log(dbg);
                }
            }
        } else {
            spawned_now++;
            if (g_game->IsValidComponent && !g_game->IsValidComponent(o.client_id)) {
                g_flk_death++;
                if ((dbg_tick % 300) == 0) {
                    char dbg[160];
                    snprintf(dbg, sizeof(dbg), "x4mp_stream: [DBG] ghost id=%llu became invalid; respawning", (unsigned long long)o.client_id);
                    net_log(dbg);
                }
                o.spawned = false; // ghost was destroyed; respawn next pass
                continue;
            }
            if (g_inert && g_game->ActivateObject) g_game->ActivateObject(o.client_id, false);
            g_game->SetObjectSectorPos(o.client_id, target_sector, pos);
        }
    }
    for (auto id : dead) g_objs.erase(id);

    // ---- Prune local ships the host no longer reports (died on the host) --
    // Only after a FULL snapshot of this sector was received and the link is
    // alive, so a broken/slow stream can never wipe the local universe.
    if (g_full_received && link_alive && g_game->RemoveComponent && g_game->IsValidComponent) {
        // Belt-and-suspenders: re-check the live player-ship IDs every pass so
        // a docking/undocking state change can never expose the player ship to
        // pruning (removing it = Game Over = the client save-reload loop).
        PlayerShipIDs pids;
        // Spawned ghosts (host ships with no local counterpart) must never be
        // pruned even though they are unbound: after a sector entry they get
        // indexed like any local ship, and the prune would kill + respawn them
        // every ~10 s (ghost flicker) while the host keeps reporting them.
        std::unordered_set<UniverseID> ghosts;
        for (auto& kv : g_objs)
            if (kv.second.spawned && kv.second.client_id) ghosts.insert(kv.second.client_id);
        unsigned sk_pid = 0, sk_bound = 0, sk_ghost = 0, sk_invalid = 0, sk_young = 0;
        for (size_t idx = 0; idx < g_index_ships.size(); idx++) {
            UniverseID s = g_index_ships[idx];
            if (pids.contains(s)) { sk_pid++; continue; }
            if (g_bound_locals.count(s)) { sk_bound++; continue; }
            if (ghosts.count(s)) { sk_ghost++; continue; }
            if (!g_game->IsValidComponent(s)) { sk_invalid++; continue; } // already gone
            auto ms = g_missing_since.find(s);
            if (ms == g_missing_since.end()) {
                g_missing_since[s] = now;
            } else if ((now - ms->second) <= std::chrono::milliseconds(g_missing_prune_ms)) {
                sk_young++;
            } else {
                g_game->RemoveComponent(s);
                g_missing_since.erase(ms);
                pruned++;
                g_flk_prune++;
                if (g_debug) {
                    char dbg[160];
                    snprintf(dbg, sizeof(dbg), "x4mp_stream: [DBG] pruned local ship %llu (not reported by host)", (unsigned long long)s);
                    net_log(dbg);
                }
            }
        }
        if (g_debug && (dbg_tick % 300) == 0 && (sk_invalid || sk_young || sk_pid || sk_bound || sk_ghost)) {
            char dbg[220];
            snprintf(dbg, sizeof(dbg), "x4mp_stream: [DBG] prune-skip: index=%zu pid=%u bound=%u ghost=%u invalid=%u young=%u (bound_locals=%zu bindings=%zu)",
                     g_index_ships.size(), sk_pid, sk_bound, sk_ghost, sk_invalid, sk_young,
                     g_bound_locals.size(), g_bindings.size());
            net_log(dbg);
        }
    }
    if (g_debug && (dbg_tick % 300) == 0 && (bound_now || corrected || pruned)) {
        char dbg[192];
        snprintf(dbg, sizeof(dbg), "x4mp_stream: [DBG] sync: bound=%u pinned=%u pruned=%u (max_lag=%.0f bind_r=%.0f conv_r=%.0f)",
                 bound_now, corrected, pruned, g_max_lag_m, g_bind_radius, g_converge_radius);
        net_log(dbg);
    }

    // Entry convergence complete: the full snapshot was received and every
    // host ship in the sector either bound (and snapped) or ghosted. From
    // now on only tight re-matching and drift correction apply.
    if (g_full_received && !g_converged) {
        g_converged = true;
        if (g_debug && !g_cv_logged) {
            g_cv_logged = true;
            char cv[256];
            snprintf(cv, sizeof(cv), "x4mp_stream: [CONVERGE] sector=%llu index=%zu host=%u bound=%u ghost=%u maxbind=%.0fm",
                     (unsigned long long)client_sector, g_index_ships.size(), g_cv_host, g_cv_bound, g_cv_ghost, g_cv_maxd);
            net_log(cv);
        }
    }
}

// ---- Frame update (main thread) -------------------------------------------
// Auto-fly test hook: teleport the player to the next sector every interval so
// the sector-transition logic (rebuild_local_index + greedy convergence) is
// exercised repeatedly. Testing only.
static void autofly_tick() {
    if (!g_autofly || !g_ready || g_autofly_sectors.empty()) return;
    if (!g_game || !g_game->MovePlayerToSectorPos) return;
    auto now = std::chrono::steady_clock::now();
    if ((now - g_autofly_last) < std::chrono::seconds(g_autofly_interval_s)) return;
    g_autofly_last = now;
    g_autofly_idx = (g_autofly_idx + 1) % (int)g_autofly_sectors.size();
    UniverseID sec = g_autofly_sectors[g_autofly_idx];
    // Use a position taken from a real ship in the target sector (a bare
    // (0,0,0) can land inside a planet and fail to update the player zone).
    UIPosRot pos; pos.x = 100000.0f; pos.y = 0.0f; pos.z = 100000.0f; pos.yaw = 0; pos.pitch = 0; pos.roll = 0;
    if (g_game->GetAllFactions && g_game->GetAllFactionShips && g_game->GetContextByClass &&
        g_game->GetObjectPositionInSector && g_game->IsValidComponent) {
        const char* factions[64];
        uint32_t nf = g_game->GetAllFactions(factions, 64, true);
        for (uint32_t f = 0; f < nf && pos.x == 100000.0f; f++) {
            UniverseID ships[512];
            uint32_t ns = g_game->GetAllFactionShips(ships, 512, factions[f]);
            for (uint32_t i = 0; i < ns; i++) {
                UniverseID s = ships[i];
                if (g_game->IsValidComponent(s) && g_game->GetContextByClass(s, "sector", false) == sec) {
                    pos = g_game->GetObjectPositionInSector(s);
                    break;
                }
            }
        }
    }
    g_game->MovePlayerToSectorPos(sec, pos);
    if (g_api && g_api->log) {
        UniverseID pz = g_game->GetPlayerZoneID ? g_game->GetPlayerZoneID() : 0;
        UniverseID actual = (pz && g_game->GetContextByClass) ? g_game->GetContextByClass(pz, "sector", false) : 0;
        char m[224];
        snprintf(m, sizeof(m), "x4mp_stream: [AUTOFLY] -> sector %llu pos=(%.0f,%.0f,%.0f) actual=%llu zone=%llu (idx %d/%zu)",
                 (unsigned long long)sec, pos.x, pos.y, pos.z, (unsigned long long)actual, (unsigned long long)pz,
                 g_autofly_idx, g_autofly_sectors.size());
        g_api->log(X4NATIVE_LOG_INFO, m);
    }
}

static void on_frame_update(const char* /*name*/, void* /*data*/, void* /*ud*/) {
    autofly_tick();
    static unsigned tick = 0;
    if ((++tick % (unsigned)(g_render_interval > 0 ? g_render_interval : 3)) != 0) return;
    render_pass();
}

// Universe fully built (2nd load pass complete). This is the definitive
// "world ready" signal. Only now is it safe to start spawning host objects.
static void on_universe_ready(const char* /*name*/, void* /*data*/, void* /*ud*/) {
    g_ready = true;
    build_sector_map();
    if (g_autofly) {
        for (auto& kv : g_sector_map) g_autofly_sectors.push_back(kv.second);
        g_autofly_last = std::chrono::steady_clock::now();
        if (g_api) {
            char m[128];
            snprintf(m, sizeof(m), "x4mp_stream: autofly armed with %zu sectors", g_autofly_sectors.size());
            g_api->log(X4NATIVE_LOG_INFO, m);
        }
    }
    if (g_api) g_api->log(X4NATIVE_LOG_INFO, "x4mp_stream: universe ready — rendering enabled");
}

// ---- Init / shutdown -------------------------------------------------------
static int g_sub_tick = -1;
static int g_sub_universe = -1;
static int g_sub_data = -1;

X4NATIVE_EXPORT int x4native_api_version(void) { return 1; }

X4NATIVE_EXPORT int x4native_init(X4NativeAPI* api) {
    g_api = api;
    g_game = api->game;

    const char* sp = std::getenv("X4MP_STREAM_PORT");
    if (sp) { int v = atoi(sp); if (v > 0 && v < 65535) g_stream_port = v; }
    const char* tr = std::getenv("X4MP_TRANSPORT");
    if (tr && *tr) {
        std::string t = tr;
        if (t == "udp" || t == "UDP") g_transport = "udp";
        else g_transport = "tcp";
    }
    const char* ln = std::getenv("X4MP_LEGACY_NET");
    if (ln) g_legacy_net = (*ln != '0');   // default OFF (consolidated); X4MP_LEGACY_NET=1 -> legacy own-socket mode
    const char* ri = std::getenv("X4MP_RENDER_INTERVAL");
    if (ri) { int v = atoi(ri); if (v >= 1 && v <= 60) g_render_interval = v; }
    const char* tau = std::getenv("X4MP_SMOOTH_TAU");
    if (tau) { float v = (float)atof(tau); if (v >= 0.001f && v <= 2.0f) g_smooth_tau = v; }
    const char* ml = std::getenv("X4MP_MAX_LAG_M");
    if (ml) { float v = (float)atof(ml); if (v >= 10.0f && v <= 5000.0f) g_max_lag_m = v; }
    const char* br = std::getenv("X4MP_BIND_RADIUS");
    if (br) { float v = (float)atof(br); if (v >= 10.0f && v <= 100000.0f) g_bind_radius = v; }
    const char* cr = std::getenv("X4MP_CONVERGE_RADIUS");
    if (cr) { float v = (float)atof(cr); if (v >= 100.0f && v <= 1000000.0f) g_converge_radius = v; }
    const char* dbg = std::getenv("X4MP_DEBUG");
    if (dbg && *dbg == '1') g_debug = true;
    const char* in = std::getenv("X4MP_INERT");
    if (in) g_inert = (*in != '0');            // default ON; X4MP_INERT=0 disables
    const char* cg = std::getenv("X4MP_CONVERGE_GREEDY");
    if (cg) g_converge_greedy = (*cg != '0');  // default ON; X4MP_CONVERGE_GREEDY=0 disables
    const char* af = std::getenv("X4MP_AUTOFLY");
    if (af && *af == '1') g_autofly = true;
    const char* afi = std::getenv("X4MP_AUTOFLY_INTERVAL");
    if (afi) { int v = atoi(afi); if (v >= 2 && v <= 300) g_autofly_interval_s = v; }

    // Legacy mode (X4MP_LEGACY_NET=1, default): open the data-stream listener
    // (TCP or UDP) on g_stream_port; the host streams OBJ/PLAYER lines here and
    // recv_loop() parses them. In the consolidated mode (X4MP_LEGACY_NET=0),
    // x4mp owns the connection and feeds us lines via the "x4mp_stream.data"
    // event, so we skip the socket/recv entirely.
    if (g_legacy_net) {
        int sock_type = (g_transport == "udp") ? SOCK_DGRAM : SOCK_STREAM;
        g_listen_sock = socket(AF_INET, sock_type, 0);
        if (g_listen_sock < 0) { net_log("x4mp_stream: failed to create socket"); return X4NATIVE_OK; }
        int one = 1;
        setsockopt(g_listen_sock, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        struct sockaddr_in sa; memset(&sa, 0, sizeof(sa));
        sa.sin_family = AF_INET;
        sa.sin_addr.s_addr = htonl(INADDR_ANY);
        sa.sin_port = htons((uint16_t)g_stream_port);
        if (bind(g_listen_sock, (struct sockaddr*)&sa, sizeof(sa)) < 0) {
            char m[128]; snprintf(m, sizeof(m), "x4mp_stream: bind port %d failed", g_stream_port);
            net_log(m);
            close(g_listen_sock); g_listen_sock = -1;
            return X4NATIVE_OK;
        }
        if (g_transport == "tcp" && listen(g_listen_sock, 4) < 0) {
            net_log("x4mp_stream: listen failed");
            close(g_listen_sock); g_listen_sock = -1;
            return X4NATIVE_OK;
        }
        g_running = true;
        g_recv_thread = std::thread(recv_loop);
    } else {
        net_log("x4mp_stream: consolidated mode — x4mp owns the connection (event-fed)");
    }

    g_sub_tick = api->subscribe("on_frame_update", on_frame_update, nullptr, api);
    g_sub_universe = api->subscribe("on_universe_ready", on_universe_ready, nullptr, api);
    g_sub_data = api->subscribe("x4mp_stream.data", on_stream_data, nullptr, api);

    char m[200];
    if (g_legacy_net) {
        snprintf(m, sizeof(m), "x4mp_stream: listening on %s %d; render interval %d frames; INERT=%d",
                 g_transport.c_str(), g_stream_port, g_render_interval, (int)g_inert);
    } else {
        snprintf(m, sizeof(m), "x4mp_stream: consolidated (event-fed); render interval %d frames; INERT=%d",
                 g_render_interval, (int)g_inert);
    }
    net_log(m);
    return X4NATIVE_OK;
}

X4NATIVE_EXPORT void x4native_shutdown(void) {
    g_running = false;
    if (g_data_sock >= 0) { close(g_data_sock); g_data_sock = -1; }
    if (g_listen_sock >= 0) { close(g_listen_sock); g_listen_sock = -1; }
    if (g_recv_thread.joinable()) g_recv_thread.join();
    if (g_api && g_api->unsubscribe && g_sub_tick >= 0) g_api->unsubscribe(g_sub_tick);
    if (g_api && g_api->unsubscribe && g_sub_universe >= 0) g_api->unsubscribe(g_sub_universe);
    if (g_api && g_api->unsubscribe && g_sub_data >= 0) g_api->unsubscribe(g_sub_data);
    net_log("x4mp_stream: shutting down");
}
