// ---------------------------------------------------------------------------
// x4mp — X4 multiplayer host/client driver (no-Steam, LAN direct-IP)
//
// X4's multiplayer transport is SLikeNet (RakNet). Steam is only used for
// lobby *discovery*; the actual game sync goes over the RakNet socket.
// X4 exports two stable functions that let us host / join directly by IP,
// completely bypassing the Steam lobby:
//
//   void NewMultiplayerGame(const char* modulename, const char* difficulty);
//   void ConnectToMultiplayerGame(const char* serverip);
//
// DESIGN (menu-driven, both sides render a window):
//   This extension does NOT auto-host or auto-join on startup by default.
//   Instead it registers two Lua->C++ bridges that the in-game menu (added by
//   the x4mp Lua/MD files) triggers via <raise_lua_event>:
//       lua event "x4mp.host"  -> C++ event "x4mp_host_request"
//       lua event "x4mp.join"  -> C++ event "x4mp_join_request" (param = IP)
//   Both the server (Machine 2) and the client (Machine 1) run a normal,
//   rendering X4 window. The user chooses the role from the menu.
//
//   OPTIONAL env-driven fallback (for headless / scripting): if X4MP_AUTO is
//   set to "host" or "client", the extension performs the action automatically
//   (host once the universe is loaded; client immediately). OFF by default.
//
//   NOTE on the old black-screen bug: the previous build auto-called
//   NewMultiplayerGame() from init() on every host launch, and the Vulkan
//   shim (vkshim) forced a no-op present whenever X4MP_MODE=host. That no-op
//   also hit the interactive CLIENT when the env was shared -> black screen.
//   Now nothing is forced: present/surface are only no-op'd when X4_HEADLESS=1
//   is set explicitly (see libshim/vkshim.c). Both roles render normally.
//
// Config (environment variables):
//   X4MP_AUTO       "host" | "client" | unset (default: menu-driven)
//   X4MP_SERVER_IP  host LAN IP (default 192.168.1.16) — used by join()
//   X4MP_MODULE     default gamestart id for host() (default
//                    "x4ep1_gamestart_boron1")
//   X4MP_DIFFICULTY default difficulty for host() (default "easy")
//
// Simulation model ("high simulation" per rendering zone + reconciliation):
//   Every participant's rendering zone is fully simulated AND synced:
//     * HOST  : the server player's sector is fully simulated (X4 default),
//               plus every connected client's current sector (ships there are
//               ActivateObject()'d, same treatment as the server player's
//               zone). The host streams each client the ships in THAT client's
//               current sector only (full snapshot on join/sector change, then
//               deltas) — the reconciliation feed.
//     * CLIENT: the client loads the SAME (synced) save and its own game
//               fully simulates the client's current sector natively (ships
//               behave normally). x4mp_stream binds each host-streamed ship to
//               the matching local ship (same macro, nearest) and corrects the
//               local ship to the host position when drift exceeds X4MP_SYNC_M;
//               local ships the host no longer reports are removed. Result: all
//               clients + host show the same ship positions.
//   Opt-ins (testing / legacy):
//     * X4MP_STREAMSHIPS=1  legacy full-universe OBJ broadcast to all clients
//     * X4MP_FULLSIM=1      host ActivateObject()s ALL ships (CPU-heavy)
//     * X4MP_TELEPORT=1     host teleports the server player into a client sector
//     * X4MP_CLEANUP=1      client removes its own save ships (thin client)
//     * X4MP_PAUSE=1        client pauses its local simulation (thin client)
// ---------------------------------------------------------------------------
#include <cstdlib>
#include <cstring>
#include <string>
#include <cstdio>
#include <chrono>
#include <vector>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <cerrno>
#include <dirent.h>
#include <unordered_map>
#include <unordered_set>
#include <thread>
#include <mutex>
#include <atomic>
#include <algorithm>

#include <x4native_extension.h>
#include <x4_game_func_table.h>

static X4NativeAPI*   g_api       = nullptr;
static X4GameFunctions* g_game    = nullptr;
static int             g_sub_tick  = 0;
static int             g_sub_loaded = 0;
static int             g_sub_host  = 0;
static int             g_sub_join  = 0;
static int             g_sub_universe = 0; // on_universe_ready (world fully built)
static int             g_sub_send_act = 0; // x4mp_stream combat ACT lines (client)
static bool            g_host_done = false;
static bool            g_join_done = false;
static bool            g_host_pending = false;
static unsigned        g_heartbeat = 0;
static std::string     g_auto;       // "host" | "client" | ""
static std::string     g_server_ip  = "192.168.1.16";
static std::string     g_module     = "x4ep1_gamestart_boron1";
static std::string     g_difficulty = "easy";
static std::string     g_save;       // X4MP_SAVE - save file to LOAD before hosting ("" = new game)
static bool            g_save_pending = false; // waiting for save list, then will raise loadSave
static bool            g_save_loading  = false; // loadSave raised, awaiting on_game_loaded
static bool            g_join_pending = false;  // client: waiting for menu before first join
static bool            g_join_success = false;  // client: confirmed in-game (joined host)
static int             g_join_wait_frames = 0;  // client: frames waited for menu
static int             g_join_retry_frames = 0; // client: frames since last retry
static int             g_join_attempts = 0;     // client: total join attempts
static bool            g_client_ready = false;  // thin client: in universe, ready to render host state
// Follow-up (player-action replication): X4MP_TEST_ACTION=1 makes the client
// send a periodic ACT message to the host to validate the action transport.
static bool            g_test_action = false;
// Test hook: X4MP_TEST_MENU=host|client simulates the in-game menu click after
// a delay, so the menu-driven host/join flow can be verified without clicking.
static bool            g_test_menu = false;
static std::string     g_test_menu_role;   // "host" or "client"
static std::chrono::steady_clock::time_point g_test_menu_time;
static bool            g_test_menu_done = false;
// PERF logging: write FPS + per-tick net_update cost to a file (X4MP_PERF_LOG,
// default /tmp/x4mp_perf.log) every 2s, so the host's frame cost can be
// diagnosed after a test run.
static int             g_perf_frames = 0;
static long long       g_perf_net_us = 0;     // accumulated net_update cost
static int             g_perf_net_count = 0;  // number of net_updates measured
static std::string     g_perf_log_path = "/tmp/x4mp_perf.log";
static std::chrono::steady_clock::time_point g_perf_log_time;
static bool            g_universe_ready = false; // universe fully built (2nd load pass done)
static bool            g_game_loaded_fired = false; // client: on_game_loaded has fired (1st pass done)
static std::chrono::steady_clock::time_point g_game_loaded_time; // when client's 1st pass completed
static long long       g_universe_timeout_ms = 180000; // X4MP_UNIVERSE_TIMEOUT fallback (0=strict)
static bool            g_newgame_pending = false; // thin client: waiting to start a new game
static bool            g_newgame_done = false;    // thin client: new game started
static std::chrono::steady_clock::time_point g_start;

// -------------------------------------------------------------------------
// CUSTOM NETWORK — our own UDP transport, completely independent of SLNet.
//
// Architecture (authoritative server -> thin client):
//   HOST  runs the ENTIRE universe simulation and streams snapshots to clients.
//   CLIENT does NOT simulate the universe; it only syncs with the host and
//   renders what it needs.
//
// Layer 1 (this file): a raw UDP socket. Host binds and listens for JOINs;
// client connects and does a handshake. Layer 2+ will carry universe-state
// snapshots over this socket.
// -------------------------------------------------------------------------
static int             g_sock = -1;
static bool            g_net_host = false;
static bool            g_net_client = false;
static bool            g_net_connected = false; // client: handshake done
static bool            g_stream_reported = false; // client: told host our stream port
static int             g_stream_port = 7778;   // client's dedicated stream port (x4mp_stream)
// Consolidated TCP mode (client): g_sock is the TCP connection to the host.
static bool            g_client_tcp_connecting = false; // non-blocking connect in progress
static std::string     g_client_tcp_buf;                // accumulates partial lines from the host
static uint16_t        g_net_port = 7777;
static std::string     g_net_host_ip = "192.168.1.16";
static struct sockaddr_in g_host_sa;
// Data-stream transport: "tcp" (default) or "udp" (X4MP_TRANSPORT). The control
// channel (7777) is always UDP; only the object data stream (7778) switches.
static std::string     g_transport = "tcp";
static int             g_stream_udp_sock = -1; // host: UDP socket for the data stream (legacy)
// Consolidated one-port-per-transport mode (X4MP_LEGACY_NET=0): control + data
// share a SINGLE connection (TCP 7778 or UDP 7777). Legacy (default) keeps the
// current working setup (UDP 7777 control + TCP/UDP 7778 data).
static bool            g_legacy_net = false; // DEFAULT: consolidated one-port mode (X4MP_LEGACY_NET=1 for the old split-port setup)
static int             g_listen_sock = -1;     // host: TCP listening socket (new mode, 7778)

// Host-side client tracking with liveness (for dead-client pruning).
// Last position sent to one client (per-client delta compression).
struct Float3 { float x = 0, y = 0, z = 0; };

struct NetClient {
    struct sockaddr_in addr;
    int id = 0;                 // unique, monotonic client id (never reused)
    std::chrono::steady_clock::time_point last_seen;
    bool loading = false; // client is loading its save (don't prune during load)
    uint16_t stream_port = 7778; // client's dedicated stream port (x4mp_stream)
    char faction[80] = {0};      // unique player faction assigned by the host
    // Per-client sync-stream state (zone-limited OBJ stream):
    UniverseID cur_sector = 0;  // this client's current sector (host ids)
    bool needs_full = true;     // send a full sector snapshot on the next stream
    std::chrono::steady_clock::time_point cur_sector_set_time{}; // when cur_sector last changed
    std::unordered_map<unsigned long long, Float3> last_sent; // per-client deltas
    // TCP stream connection to the client's x4mp_stream (port stream_port):
    int  tcp_fd = -1;           // connected TCP fd (-1 = not connected)
    int  tcp_backoff = 0;       // ticks to wait before retrying the connect
    bool tcp_connecting = false;// non-blocking connect in progress
    std::string tcp_recv_buf;   // (new mode) accumulates partial lines from c.tcp_fd
    // NOTE: NO destructor that closes tcp_fd. NetClient is copied/moved inside
    // g_clients (push_back + remove_if reallocation); a destructor close would
    // free the fd while the vector's copy still references it -> EBADF on the
    // next recv (the accept->drop->reconnect loop). tcp_fd is closed explicitly
    // at the connection-error, prune, and shutdown sites instead.
};

static std::vector<NetClient> g_clients;
// Monotonic client-id counter: ids are never reused, so a client that joins
// after another is pruned cannot collide with the old client's faction/ghost.
static int g_next_client_id = 1;

// Host-side representation of each connected client's ship (so the host can
// SEE the client's ship). Key = hash of client sockaddr; value = spawned ship id.
static std::unordered_map<uint64_t, UniverseID> g_client_ships;

// Host-side ghost STATIONS built by each client after load (issue 3). The
// save's stations already exist on both sides (same save); only post-load
// builds are new. Key = hash of client sockaddr; value = spawned station ids
// indexed by the client's ACT BUILD sequence number.
static std::unordered_map<uint64_t, std::vector<UniverseID>> g_client_stations;
// CLIENT: stations already reported to the host (baseline = the save's
// stations at ready time, so only NEW builds are reported).
static std::unordered_map<UniverseID, uint32_t> g_client_station_seq_map; // station -> stable seq
static std::unordered_set<UniverseID> g_client_station_baseline; // the save's stations (never reported)
static uint32_t g_client_station_seq = 0;
static bool g_fleet_reassigned = false; // issue 4: reassignment completed
static bool client_reassign_player_fleet(); // fwd (defined after net_update)

// HOST side of station replication (issue 3, symmetric to ACT BUILD):
// the host player's NEW builds (PF stations created after load) are streamed
// to clients as STA lines so they appear in the clients' universes.
static std::unordered_set<UniverseID> g_host_station_baseline;
static bool g_host_station_baseline_done = false;
static std::unordered_set<UniverseID> g_host_station_sent; // logged-once (still re-sent)

// Host-side cached ship state per client, used to relay to other clients so
// that clients can see each other. Key = hash of client sockaddr.
struct ClientShipState {
    float x=0, y=0, z=0, yaw=0, pitch=0, roll=0;
    char macro[160] = {0};
    char sector_macro[160] = {0}; // client's sector macro (to place ghost + move host player)
    UniverseID host_sector = 0;   // host's version of the client's sector (0 = unknown)
};
static std::unordered_map<uint64_t, ClientShipState> g_client_ship_state;

// Client-side ghosts of OTHER clients' ships (so clients see each other).
// Key = remote client id (as sent by the host relay); value = spawned ship id.
static std::unordered_map<unsigned long long, UniverseID> g_remote_ships;
static unsigned        g_net_tick = 0;

// Connection-stability timeouts (ms).
// Host: prune a client that has sent nothing (JOIN/INPUT/PONG) for this long.
static long long       g_host_client_timeout_ms = 30000; // X4MP_HOST_TIMEOUT
// Client: if no data received from the host for this long, assume the link is
// dead and re-send JOIN to re-establish the handshake (handles host restart).
static long long       g_client_link_timeout_ms = 15000; // X4MP_CLIENT_TIMEOUT
static std::chrono::steady_clock::time_point g_client_last_recv; // client: last host data
static bool            g_debug = false;   // X4MP_DEBUG=1 -> continuous streamed-data display
static std::string     g_objmode = "cache"; // X4MP_OBJMODE=cache|full (object streaming)
static bool            g_cleanup_enabled = false; // X4MP_CLEANUP=1 enables client own-object removal (thin client)
static bool            g_pause_enabled = false;  // X4MP_PAUSE=1 pauses client (thin-client offload)
// X4MP_FULLSIM=1 forces the host to fully simulate EVERY ship in the universe
// (ActivateObject on all ~85k). CPU-heavy; testing only. Default 0: the host
// fully simulates just the high-simulation set — the server player's sector
// plus every connected client's current sector — so each participant has a
// fully simulated rendering zone without simulating the whole universe.
static bool            g_fullsim = false;         // X4MP_FULLSIM=1 enables full-universe simulation
// X4MP_TELEPORT=1: legacy behaviour — teleport the host (server) player ship
// into a client's sector to force simulation of that sector. Off by default:
// the per-sector ActivateObject() (high-simulation set) achieves the same
// without moving the server player around.
static bool            g_teleport_enabled = false;
// Rate-limit for moving the host player into a client's sector (only used when
// X4MP_TELEPORT=1). Moving the host player is a teleport that stresses the
// engine; if the client flies fast through many sectors, teleporting every
// update can crash the game. Only move at most once per this interval (ms).
static std::chrono::steady_clock::time_point g_last_player_move;
static int             g_player_move_interval_ms = 2000; // X4MP_PLAYER_MOVE_INTERVAL
// X4MP_STREAMSHIPS=1: host streams the full ship universe (OBJ messages) to
// clients (thin-client / testing mode). Default 0: clients keep their own
// locally simulated universe (the synced save) and only receive human PLAYER
// ghosts — each client's rendering zone is fully simulated by its own game.
static bool            g_stream_ships = false;
// Sectors that already have a player-owned simulation satellite. X4 keeps
// sectors where the player has assets active. We place one in every
// high-simulation sector (server player + connected clients).
static std::unordered_set<UniverseID> g_sat_sectors;
// Host: map from sector macro name -> host sector UniverseID. Built during the
// simulation maintenance pass. Used to place client ghosts in the correct
// sector and to compute the high-simulation sector set.
static std::unordered_map<std::string, UniverseID> g_host_sector_by_macro;
// High-simulation sector set: the server player's sector plus every connected
// client's current sector. Ships in these sectors are ActivateObject()'d so
// they are fully simulated (high attention) even though the local player is
// not there. Rebuilt on every maintenance pass.
static std::unordered_set<UniverseID> g_highsim_sectors;

static void net_log(const char* msg) {
    if (g_api) g_api->log(X4NATIVE_LOG_INFO, msg);
}

// Debug display: writes to the x4mp log AND to stdout (so it appears in the
// terminal where the game is launched / via tee to the log). Continuous stream
// of what data is being sent/received, to verify the sync is real.
static void net_debug(const char* msg) {
    if (!g_debug) return;
    if (g_api) g_api->log(X4NATIVE_LOG_INFO, msg);
    fprintf(stdout, "%s\n", msg);
    fflush(stdout);
}

// ---- Ghost factions ------------------------------------------------------------
// Ghost ships (a client's player ship as seen on the host / other clients, and
// the host's player ship as seen on clients) are spawned with SpawnObjectAtPos2,
// which requires the owner faction to EXIST in the universe. The old design
// invented fake factions ("x4mp_host", "x4mp_client_N") that were never
// registered, so every ghost spawn failed with "Failed to retrieve owner
// faction" and no player could ever see another player's ship.
//
// Both host and client load the SAME save, so every REAL faction exists on both
// sides. We therefore assign each ghost a real faction, chosen deterministically
// from the sorted real-faction list (excluding the vanilla "player" faction).
// The sorted order is identical on every machine for the same save, so the host
// and all clients agree on which faction a given ghost uses without extra
// negotiation.
static std::vector<std::string> g_real_factions; // sorted, "player" excluded
static bool g_real_factions_built = false;

static void ensure_real_factions() {
    if (g_real_factions_built) return;
    if (!g_game || !g_game->GetAllFactions) return;   // retry next call once loaded
    g_real_factions_built = true;
    const char* factions[128];
    uint32_t nf = g_game->GetAllFactions(factions, 128, true);
    for (uint32_t f = 0; f < nf; f++) {
        if (!factions[f] || !factions[f][0]) continue;
        if (strcmp(factions[f], "player") == 0) continue;
        g_real_factions.push_back(factions[f]);
    }
    std::sort(g_real_factions.begin(), g_real_factions.end());
    if (g_debug && !g_real_factions.empty()) {
        char dbg[220];
        snprintf(dbg, sizeof(dbg), "x4mp: [DBG] %zu real factions for ghosts; F_HOST=%s",
                 g_real_factions.size(), g_real_factions[0].c_str());
        net_debug(dbg);
    }
}

// The faction the local player is CURRENTLY piloting. In a campaign save the
// player boards foreign ships (e.g. the Boron1 "alliance" ship), so this can
// differ from "player". Ghost factions must never collide with it, or the
// host's ghost ship would be indistinguishable from the client's own ship.
static const char* player_piloted_faction() {
    if (!g_game || !g_game->GetPlayerObjectID || !g_game->GetOwnerDetails2) return nullptr;
    UniverseID p = g_game->GetPlayerObjectID();
    if (!p) return nullptr;
    auto od = g_game->GetOwnerDetails2(p);
    return od.factionID;
}

// Deterministic ghost-faction picker. `id` = 0 for the host's own ship,
// >= 1 for client slots. Skips the faction the player currently pilots so a
// ghost is always visually distinct from the local player's own ship. Every
// side computes the same choice (same save -> same faction list, same
// piloted faction).
static const std::string* pick_ghost_faction(int id) {
    ensure_real_factions();
    if (g_real_factions.empty()) return nullptr;
    const char* piloted = player_piloted_faction();
    int size = (int)g_real_factions.size();
    for (int attempt = 0; attempt < size; attempt++) {
        int idx = ((id + attempt) % size + size) % size;
        const std::string& f = g_real_factions[idx];
        if (piloted && strcmp(f.c_str(), piloted) == 0) continue;
        return &f;
    }
    return &g_real_factions[0];
}

// The host's OWN player-ship ghost faction (deterministic, exists on all
// sides). Returns "" while the universe (and thus the faction list) is not
// ready yet — callers must defer, NOT fall back to "player" (which would make
// a ghost appear as the local player's own ship).
static const char* host_display_faction() {
    const std::string* f = pick_ghost_faction(0);
    return f ? f->c_str() : "";
}

// Real faction assigned to client slot `id` (1-based) for its player-ship
// ghost. Same "" = universe not ready contract. MUST be called at
// ghost-spawn/relay time, not at JOIN time: JOINs arrive during the host's
// load passes, before the universe (and the faction list) exists.
static void client_faction_for(int id, char* out, size_t outsize) {
    const std::string* f = pick_ghost_faction(id);
    snprintf(out, outsize, "%s", f ? f->c_str() : "");
}

// Enumerate ALL ships/stations of a faction with no arbitrary buffer cap.
// The old code passed a fixed 2048-entry stack buffer; factions with more ships
// silently lost the overflow, so the host's FULL snapshots were incomplete and
// the client's missing-ship prune deleted ships that actually exist. We size
// the buffer from GetNumAllFactionShips/Stations and heap-allocate.
static uint32_t enumerate_faction_ships(const char* faction, std::vector<UniverseID>& out) {
    if (!g_game || !g_game->GetAllFactionShips) return 0;
    uint32_t cap = 4096;
    if (g_game->GetNumAllFactionShips) cap = std::max<uint32_t>(cap, g_game->GetNumAllFactionShips(faction));
    if (cap == 0) return 0;
    out.resize(cap);
    return g_game->GetAllFactionShips(out.data(), cap, faction);
}
static uint32_t enumerate_faction_stations(const char* faction, std::vector<UniverseID>& out) {
    if (!g_game || !g_game->GetAllFactionStations) return 0;
    uint32_t cap = 1024;
    if (g_game->GetNumAllFactionStations) cap = std::max<uint32_t>(cap, g_game->GetNumAllFactionStations(faction));
    if (cap == 0) return 0;
    out.resize(cap);
    return g_game->GetAllFactionStations(out.data(), cap, faction);
}

// DIAG: every close() of a socket we own goes through here so fd-table
// changes are traceable in the log (site = where the close happened).
static void net_close_fd(int fd, const char* site) {
    if (fd < 0) return;
    char m[128];
    snprintf(m, sizeof(m), "x4mp: net: CLOSE fd=%d site=%s", fd, site);
    net_log(m);
    close(fd);
}

// DIAG: dump the process fd table (/proc/self/fd) so a silent fd death can be
// correlated with what the fd number was/is.
static void net_dump_fds(const char* why) {
    DIR* d = opendir("/proc/self/fd");
    if (!d) return;
    std::string line;
    struct dirent* e;
    char path[64], tgt[512];
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        snprintf(path, sizeof(path), "/proc/self/fd/%s", e->d_name);
        ssize_t n = readlink(path, tgt, sizeof(tgt) - 1);
        if (n > 0) tgt[n] = 0; else tgt[0] = 0;
        line += std::string(e->d_name) + ":" + tgt + " ";
    }
    closedir(d);
    char m[2048];
    snprintf(m, sizeof(m), "x4mp: net: FD_TABLE (%s): %s", why, line.c_str());
    net_log(m);
}

static bool net_init_host(uint16_t port) {
    // Consolidated TCP mode (X4MP_LEGACY_NET=0, X4MP_TRANSPORT=tcp): bind a TCP
    // LISTENER on the data port (7778). Clients CONNECT to us; we accept one
    // connection per client and carry BOTH control and data on it. We do NOT
    // open the legacy UDP control socket in this mode.
    if (!g_legacy_net && g_transport == "tcp") {
        uint16_t tport = (uint16_t)g_stream_port;
        g_listen_sock = socket(AF_INET, SOCK_STREAM, 0);
        if (g_listen_sock < 0) { net_log("x4mp: net: socket() failed"); return false; }
        int one = 1;
        setsockopt(g_listen_sock, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        struct sockaddr_in sa; memset(&sa, 0, sizeof(sa));
        sa.sin_family = AF_INET;
        sa.sin_addr.s_addr = htonl(INADDR_ANY);
        sa.sin_port = htons(tport);
        if (bind(g_listen_sock, (struct sockaddr*)&sa, sizeof(sa)) < 0) {
            net_log("x4mp: net: bind() failed");
            net_close_fd(g_listen_sock, "init_host_bind"); g_listen_sock = -1;
            return false;
        }
        if (listen(g_listen_sock, 8) < 0) {
            net_log("x4mp: net: listen() failed");
            net_close_fd(g_listen_sock, "init_host_listen"); g_listen_sock = -1;
            return false;
        }
        int fl = fcntl(g_listen_sock, F_GETFL, 0);
        fcntl(g_listen_sock, F_SETFL, fl | O_NONBLOCK);
        g_net_host = true;
        char buf[160];
        snprintf(buf, sizeof(buf), "x4mp: net: HOST listening on TCP port %u (consolidated control+data)", (unsigned)tport);
        net_log(buf);
        return true;
    }
    g_sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (g_sock < 0) { net_log("x4mp: net: socket() failed"); return false; }
    int one = 1;
    setsockopt(g_sock, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in sa; memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_ANY);
    sa.sin_port = htons(port);
    if (bind(g_sock, (struct sockaddr*)&sa, sizeof(sa)) < 0) {
        net_log("x4mp: net: bind() failed");
        net_close_fd(g_sock, "init_host_udp_bind"); g_sock = -1;
        return false;
    }
    int flags = fcntl(g_sock, F_GETFL, 0);
    fcntl(g_sock, F_SETFL, flags | O_NONBLOCK);
    g_net_host = true;
    char buf[128];
    snprintf(buf, sizeof(buf), "x4mp: net: HOST listening on UDP port %u", (unsigned)port);
    net_log(buf);
    return true;
}

static bool net_init_client(const char* ip, uint16_t port) {
    // Consolidated TCP mode (X4MP_LEGACY_NET=0, tcp): g_sock is a TCP socket
    // that CONNECTS to the host's data port (7778). Control + data ride it.
    if (!g_legacy_net && g_transport == "tcp") {
        uint16_t tport = (uint16_t)g_stream_port;
        g_sock = socket(AF_INET, SOCK_STREAM, 0);
        if (g_sock < 0) { net_log("x4mp: net: socket() failed"); return false; }
        int fl = fcntl(g_sock, F_GETFL, 0);
        fcntl(g_sock, F_SETFL, fl | O_NONBLOCK);
        memset(&g_host_sa, 0, sizeof(g_host_sa));
        g_host_sa.sin_family = AF_INET;
        g_host_sa.sin_port = htons(tport);
        if (inet_pton(AF_INET, ip, &g_host_sa.sin_addr) != 1) {
            net_log("x4mp: net: invalid server IP");
            net_close_fd(g_sock, "init_client_tcp_badip"); g_sock = -1;
            return false;
        }
        int one = 1;
        setsockopt(g_sock, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
        int r = connect(g_sock, (struct sockaddr*)&g_host_sa, sizeof(g_host_sa));
        if (r == 0) g_client_tcp_connecting = false;
        else if (errno == EINPROGRESS) g_client_tcp_connecting = true;
        else { net_log("x4mp: net: connect() failed"); net_close_fd(g_sock, "init_client_tcp_connect"); g_sock = -1; return false; }
        g_net_client = true;
        g_client_last_recv = std::chrono::steady_clock::now();
        char buf[128];
        snprintf(buf, sizeof(buf), "x4mp: net: CLIENT connecting to %s:%u (consolidated TCP)", ip, (unsigned)tport);
        net_log(buf);
        return true;
    }
    g_sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (g_sock < 0) { net_log("x4mp: net: socket() failed"); return false; }
    memset(&g_host_sa, 0, sizeof(g_host_sa));
    g_host_sa.sin_family = AF_INET;
    g_host_sa.sin_port = htons(port);
    if (inet_pton(AF_INET, ip, &g_host_sa.sin_addr) != 1) {
        net_log("x4mp: net: invalid server IP");
        net_close_fd(g_sock, "init_client_udp_badip"); g_sock = -1;
        return false;
    }
    int flags = fcntl(g_sock, F_GETFL, 0);
    fcntl(g_sock, F_SETFL, flags | O_NONBLOCK);
    g_net_client = true;
    g_client_last_recv = std::chrono::steady_clock::now();
    char buf[128];
    snprintf(buf, sizeof(buf), "x4mp: net: CLIENT targeting %s:%u", ip, (unsigned)port);
    net_log(buf);
    return true;
}

static void net_send(const struct sockaddr_in* to, const char* msg) {
    if (g_sock < 0) return;
    sendto(g_sock, msg, (size_t)strlen(msg), 0, (struct sockaddr*)to, sizeof(*to));
}

static void net_send_to(int sock, const struct sockaddr_in* to, const char* msg) {
    if (sock < 0) return;
    sendto(sock, msg, (size_t)strlen(msg), 0, (struct sockaddr*)to, sizeof(*to));
}

static void net_send_host(const char* msg) {
    for (auto& c : g_clients) net_send(&c.addr, msg);
}

// CLIENT: send a CONTROL message to the host. In the consolidated TCP mode
// (X4MP_LEGACY_NET=0, tcp) it rides the TCP connection (g_sock); otherwise it
// uses the legacy UDP control socket (7777). A full send buffer drops the tick
// (the next tick resends) rather than stalling the game thread.
static void net_send_client(const char* msg) {
    if (!g_legacy_net && g_transport == "tcp") {
        if (g_sock < 0 || g_client_tcp_connecting) return;
        size_t len = strlen(msg);
        size_t off = 0;
        while (off < len) {
            ssize_t w = send(g_sock, msg + off, len - off, MSG_NOSIGNAL);
            if (w < 0) {
                if (errno == EINTR) continue;
                if (errno == EAGAIN || errno == EWOULDBLOCK) return;
                char m[200];
                snprintf(m, sizeof(m), "x4mp: net: CLIENT send error errno=%d (%s) — reconnecting", errno, strerror(errno));
                net_log(m);
                net_close_fd(g_sock, "client_send_error"); g_sock = -1; g_net_connected = false;
                return;
            }
            off += (size_t)w;
        }
    } else {
        net_send(&g_host_sa, msg);
    }
}

// HOST: send a batch over ONE client's TCP stream. Returns false on failure.
// A real error (EPIPE/ECONNRESET) drops the connection so net_client_tcp()
// reconnects next tick; a send-timeout just returns false (retry next tick).
static bool net_send_tcp(NetClient& c, const char* msg, size_t len) {
    if (c.tcp_fd < 0) return false;
    size_t off = 0;
    while (off < len) {
        ssize_t w = send(c.tcp_fd, msg + off, len - off, MSG_NOSIGNAL);
        if (w < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) return false; // send timeout
            char m[200];
            snprintf(m, sizeof(m), "x4mp: net: HOST send error id=%d fd=%d errno=%d (%s)", c.id, c.tcp_fd, errno, strerror(errno));
            net_log(m);
            if (errno == EBADF) net_dump_fds("send-EBADF");
            net_close_fd(c.tcp_fd, "host_send_error"); c.tcp_fd = -1; // real error (EPIPE/ECONNRESET)
            return false;
        }
        off += (size_t)w;
    }
    return true;
}

// HOST: send a CONTROL message (WELCOME/PING/SNAP) to ONE client. In the
// consolidated TCP mode (X4MP_LEGACY_NET=0, tcp) the control rides the same
// per-client connection as the data (c.tcp_fd); otherwise it uses the legacy
// UDP control socket (7777).
static void net_send_ctrl(NetClient* c, const char* msg) {
    if (!g_legacy_net && g_transport == "tcp") net_send_tcp(*c, msg, strlen(msg));
    else net_send(&c->addr, msg);
}
// HOST: send a CONTROL message to ALL clients.
static void net_send_ctrl_host(const char* msg) {
    for (auto& c : g_clients) net_send_ctrl(&c, msg);
}

// HOST: maintain the per-client TCP stream connection. The client's x4mp_stream
// listens on its stream port; we connect (non-blocking, short backoff) so a
// not-yet-ready client never stalls the game thread. On a trusted LAN the
// connect succeeds or is refused immediately.
static void net_client_tcp(NetClient& c) {
    if (c.tcp_connecting) {
        int err = 0; socklen_t el = sizeof(err);
        if (getsockopt(c.tcp_fd, SOL_SOCKET, SO_ERROR, &err, &el) == 0 && err == 0) {
            c.tcp_connecting = false;
            // Keep the stream fd NON-BLOCKING: a full send buffer must return
            // EAGAIN immediately (net_send_tcp drops the tick) rather than
            // stalling the game's main thread (the 5-FPS killer).
            int one = 1; setsockopt(c.tcp_fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
            char m[128];
            snprintf(m, sizeof(m), "x4mp: net: TCP stream connected to %s:%u",
                     inet_ntoa(c.addr.sin_addr), (unsigned)c.stream_port);
            net_log(m);
        } else {
            net_close_fd(c.tcp_fd, "net_client_tcp_verify_fail"); c.tcp_fd = -1; c.tcp_connecting = false;
            c.tcp_backoff = 15;
        }
        return;
    }
    if (c.tcp_fd >= 0) return; // already connected
    if (c.tcp_backoff > 0) { c.tcp_backoff--; return; }
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { c.tcp_backoff = 15; return; }
    int fl = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, fl | O_NONBLOCK);
    struct sockaddr_in sa = c.addr;
    sa.sin_port = htons(c.stream_port);
    int r = connect(fd, (struct sockaddr*)&sa, sizeof(sa));
    if (r == 0) {
        // Keep the stream fd NON-BLOCKING (see the tcp_connecting branch above).
        int one = 1; setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
        c.tcp_fd = fd;
        char m[128];
        snprintf(m, sizeof(m), "x4mp: net: TCP stream connected to %s:%u",
                 inet_ntoa(c.addr.sin_addr), (unsigned)c.stream_port);
        net_log(m);
    } else if (errno == EINPROGRESS) {
        c.tcp_fd = fd; c.tcp_connecting = true; // verify next tick
    } else {
        net_close_fd(fd, "net_client_tcp_connect_fail"); c.tcp_backoff = 15;
    }
}

// Forward decl (defined later): UDP data-stream send to one client.
static void net_send_udp_to(NetClient& c, const char* msg, size_t len);

// Send object/player RENDERING data to each client over its data stream
// (TCP or UDP; handled by the x4mp_stream extension on the client).
static void net_send_stream(const char* msg) {
    size_t len = strlen(msg);
    for (auto& c : g_clients) {
        if (g_transport == "udp") net_send_udp_to(c, msg, len);
        else net_send_tcp(c, msg, len);
        if (g_debug && (g_net_tick % 300) == 0) {
            char dbg[160];
            snprintf(dbg, sizeof(dbg), "x4mp: [DBG] HOST stream->%s:%u len=%zu",
                     inet_ntoa(c.addr.sin_addr), (unsigned)c.stream_port, len);
            net_debug(dbg);
        }
    }
}


// -------------------------------------------------------------------------
// Layer 2 — SYNC PROTOCOL.
//   HOST -> CLIENT : universe-state snapshots (start with the player object).
//   CLIENT -> HOST : input / commands.
//
// Snapshot format (text, one line):
//   SNAP <playerid> <zoneid> <x> <y> <z> <yaw> <pitch> <roll> <hosttick>\n
// Input format:
//   INPUT tick=<clienttick>\n
// Layer 3 will render the received state on the client instead of simulating.
// -------------------------------------------------------------------------
static float g_snap_x = 0, g_snap_y = 0, g_snap_z = 0;
static unsigned long long g_snap_id = 0;
static unsigned long long g_snap_zone = 0;
static unsigned g_snap_tick = 0;

// Client-side rendered objects (received from the host OBJ stream).
struct ClientObj {
    unsigned long long id;
    unsigned long long zone;
    unsigned long long sector;
    float x, y, z, yaw, pitch, roll;
    char faction[80];
    char macro[160];
};
static std::unordered_map<unsigned long long, ClientObj> g_client_objs;
static std::unordered_map<unsigned long long, unsigned long long> g_idmap; // host id -> client id
static bool g_cleanup_done = false;      // client's own save objects removed
static bool g_cleanup_pending = false;
static std::chrono::steady_clock::time_point g_ready_time;

// Host: cached list of objects in the player's zone (id/macro/faction), so we
// can stream their positions EVERY frame without re-enumerating all 85k ships.
struct ZoneObj {
    unsigned long long id;
    char faction[80];
    char macro[160];
    char sector_macro[160]; // sector MACRO name (client maps it to its own sector)
    UniverseID sector = 0;  // host sector id (per-sector index key)
    // Delta-compression state: last position sent to clients. We only re-send
    // an object if it moved beyond g_delta_m (or a periodic full refresh).
    float last_x = 0, last_y = 0, last_z = 0;
    bool sent = false;
};
static std::vector<ZoneObj> g_zone_objs;
static unsigned long long g_zone_objs_zone = 0;
// Host: per-sector index into g_zone_objs (rebuilt on every maintenance pass).
// Pointers are valid until the next maintenance pass.
static std::unordered_map<UniverseID, std::vector<ZoneObj*>> g_sector_ship_index;
// When the sector index was last rebuilt. Used to gate the empty-FULL send: an
// empty sector snapshot is only authoritative once the index has been rebuilt
// since the client's sector changed (otherwise a not-yet-indexed sector looks
// empty and the client would prune its whole local sector).
static std::chrono::steady_clock::time_point g_last_index_rebuild{};
// Host: persistent ship macro cache (id -> macro). A ship's macro never
// changes, so the expensive Lua lookup happens once per ship, not every 5s.
static std::unordered_map<UniverseID, std::string> g_ship_macro_cache;
// Host: host-only objects (simulation satellites, client ghost ships) that
// must never be streamed to clients — they do not exist in the clients' saves.
static std::unordered_set<UniverseID> g_host_only_objects;

// Network performance tuning.
static int    g_update_interval = 1;   // frames between state sends (1 = full tick rate ~20Hz;
                                       // at highway speeds 15km/s a slower rate means the
                                       // streamed positions are km stale)
static float  g_delta_m = 0.5f;        // min movement (meters) to re-send an object
static float  g_relevance_m = 20000.0f; // only stream objects within this radius of the host player
static int    g_full_refresh_interval = 120; // frames between forced full refreshes

// Multi-stream networking: port 7777 is the control/handshake channel; ports
// 7778+ are data channels, each streamed by its own thread.
static std::mutex g_data_mutex;                 // protects g_client_objs, g_snap
static std::mutex g_lua_mutex;                  // Lua is not thread-safe; serialize get_macro
static std::atomic<bool> g_streams_running{false};
static std::vector<std::thread> g_stream_threads;
static std::vector<int> g_data_socks;
static int g_num_streams = 0;                   // X4MP_STREAMS: ships, stations (0=off, single-threaded)

// Forward decl (defined below).
static void get_macro(UniverseID id, char* out, size_t outsize);

// Host: read the player's universe state and stream it to all clients.
static void net_send_snapshot_host() {
    if (!g_game || g_clients.empty()) return;
    if (!g_game->GetPlayerObjectID || !g_game->GetObjectPositionInSector) return;
    UniverseID player = g_game->GetPlayerObjectID();
    if (player == 0) return;
    UIPosRot pos = g_game->GetObjectPositionInSector(player);
    UniverseID zone = g_game->GetPlayerZoneID ? g_game->GetPlayerZoneID() : 0;
    char buf[256];
    snprintf(buf, sizeof(buf), "SNAP %llu %llu %.3f %.3f %.3f %.3f %.3f %.3f %u\n",
             (unsigned long long)player, (unsigned long long)zone,
             pos.x, pos.y, pos.z, pos.yaw, pos.pitch, pos.roll, g_net_tick);
    net_send_ctrl_host(buf);
    // Also relay the host's OWN ship to all clients (cid=0) so they can SEE
    // the server's player ship as a separate entity. (SNAP alone only moves the
    // client's camera when paused; this spawns a visible ghost of the host.)
    if (g_game && g_game->GetPlayerObjectID && g_game->GetObjectPositionInSector) {
        UniverseID pship = g_game->GetPlayerObjectID();
        if (pship) {
            UIPosRot ppos = g_game->GetObjectPositionInSector(pship);
            char pmacro[160]; get_macro(pship, pmacro, sizeof(pmacro));
            // Sector macro so the client renders the ghost in the correct
            // (mapped) sector and only when it is actually there.
            char psecmacro[160] = {0};
            if (g_game->GetContextByClass) {
                UniverseID psec = g_game->GetContextByClass(pship, "sector", false);
                if (psec) get_macro(psec, psecmacro, sizeof(psecmacro));
            }
            const char* hfaction = host_display_faction();
            if (!hfaction[0]) return; // universe not ready — defer this tick
            char relay[360];
            // The host's ghost is rendered on clients under a REAL faction
            // (host_display_faction) so SpawnObjectAtPos2 succeeds and the
            // client sees the host's ship as a foreign ship, NOT as its own
            // player-faction ship.
            snprintf(relay, sizeof(relay), "PLAYER 0 %.3f %.3f %.3f %.3f %.3f %.3f %s %s %s\n",
                     ppos.x, ppos.y, ppos.z, ppos.yaw, ppos.pitch, ppos.roll,
                     pmacro[0] ? pmacro : "?", hfaction, psecmacro[0] ? psecmacro : "?");
            net_send_stream(relay); // -> client's stream port (x4mp_stream)
        }
    }
    if (g_debug) {
        char dbg[320];
        snprintf(dbg, sizeof(dbg), "x4mp: [DBG] HOST streaming player id=%llu zone=%llu pos=(%.1f,%.1f,%.1f) rot=(%.1f,%.1f,%.1f) tick=%u",
                 (unsigned long long)player, (unsigned long long)zone,
                 pos.x, pos.y, pos.z, pos.yaw, pos.pitch, pos.roll, g_net_tick);
        net_debug(dbg);
    }
}

// Get an object's macro name via the Lua API (GetComponentData(id, "macro")).
// Falls back to the component name if unavailable.
static void get_macro(UniverseID id, char* out, size_t outsize) {
    out[0] = 0;
    std::lock_guard<std::mutex> lk(g_lua_mutex);
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

// HOST: a data-streaming thread. Binds a data socket (port 7777+1+i), accepts
// clients, and streams a category of objects (ships or stations) in the player's
// zone. Runs continuously off the main thread.
static void host_stream_thread(int port, bool ships) {
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) return;
    int one = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in sa; memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_ANY);
    sa.sin_port = htons(port);
    if (bind(sock, (struct sockaddr*)&sa, sizeof(sa)) < 0) { close(sock); return; }
    int flags = fcntl(sock, F_GETFL, 0);
    fcntl(sock, F_SETFL, flags | O_NONBLOCK);
    std::vector<struct sockaddr_in> clients;
    while (g_streams_running) {
        // Accept JOINs on this data port.
        char buf[4096];
        struct sockaddr_in from; socklen_t fl = sizeof(from);
        ssize_t n;
        while ((n = recvfrom(sock, buf, sizeof(buf) - 1, 0, (struct sockaddr*)&from, &fl)) > 0) {
            buf[n] = 0;
            if (strncmp(buf, "JOIN", 4) == 0) {
                bool found = false;
                for (auto& c : clients)
                    if (c.sin_port == from.sin_port && c.sin_addr.s_addr == from.sin_addr.s_addr) { found = true; break; }
                if (!found) clients.push_back(from);
            }
        }
        // Stream this category to connected clients. Only touch g_game once the
        // host is done (game loaded) — accessing it during init causes shutdown.
        if (g_host_done && !clients.empty() && g_game && g_game->GetPlayerZoneID && g_game->GetAllFactions) {
            UniverseID player_zone = g_game->GetPlayerZoneID();
            if (player_zone != 0) {
                const char* factions[64];
                uint32_t nf = g_game->GetAllFactions(factions, 64, true);
                char batch[60000]; size_t used = 0;
                for (uint32_t f = 0; f < nf; f++) {
                    if (ships) {
                        std::vector<UniverseID> objs;
                        uint32_t no = enumerate_faction_ships(factions[f], objs);
                        for (uint32_t i = 0; i < no; i++) {
                            UniverseID obj = objs[i];
                            UniverseID zone = g_game->GetContextByClass ? g_game->GetContextByClass(obj, "zone", false) : 0;
                            if (zone != player_zone) continue;
                            UIPosRot pos = g_game->GetObjectPositionInSector(obj);
                            UniverseID sector = g_game->GetContextByClass ? g_game->GetContextByClass(obj, "sector", false) : 0;
                            char macro[160]; get_macro(obj, macro, sizeof(macro));
                            char b2[600];
                            int nn = snprintf(b2, sizeof(b2), "OBJ %llu %llu %llu %.3f %.3f %.3f %.3f %.3f %.3f %s %s\n",
                                              (unsigned long long)obj, (unsigned long long)zone, (unsigned long long)sector,
                                              pos.x, pos.y, pos.z, pos.yaw, pos.pitch, pos.roll,
                                              factions[f] ? factions[f] : "player", macro[0] ? macro : "?");
                            if (used + (size_t)nn < sizeof(batch)) { memcpy(batch + used, b2, (size_t)nn); used += (size_t)nn; }
                        }
                    } else if (g_game->GetAllFactionStations) {
                        std::vector<UniverseID> objs;
                        uint32_t no = enumerate_faction_stations(factions[f], objs);
                        for (uint32_t i = 0; i < no; i++) {
                            UniverseID obj = objs[i];
                            UniverseID zone = g_game->GetContextByClass ? g_game->GetContextByClass(obj, "zone", false) : 0;
                            if (zone != player_zone) continue;
                            UIPosRot pos = g_game->GetObjectPositionInSector(obj);
                            UniverseID sector = g_game->GetContextByClass ? g_game->GetContextByClass(obj, "sector", false) : 0;
                            char macro[160]; get_macro(obj, macro, sizeof(macro));
                            char b2[600];
                            int nn = snprintf(b2, sizeof(b2), "OBJ %llu %llu %llu %.3f %.3f %.3f %.3f %.3f %.3f %s %s\n",
                                              (unsigned long long)obj, (unsigned long long)zone, (unsigned long long)sector,
                                              pos.x, pos.y, pos.z, pos.yaw, pos.pitch, pos.roll,
                                              factions[f] ? factions[f] : "player", macro[0] ? macro : "?");
                            if (used + (size_t)nn < sizeof(batch)) { memcpy(batch + used, b2, (size_t)nn); used += (size_t)nn; }
                        }
                    }
                }
                if (used > 0) {
                    batch[used] = 0;
                    for (auto& c : clients) net_send_to(sock, &c, batch);
                }
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }
    close(sock);
}

// CLIENT: a data-receive thread. Binds a data socket targeting host:port, sends
// JOIN, and stores received OBJ messages into g_client_objs (under the mutex).
static void client_receive_thread(int port) {
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) return;
    struct sockaddr_in sa; memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons(port);
    if (inet_pton(AF_INET, g_net_host_ip.c_str(), &sa.sin_addr) != 1) { close(sock); return; }
    int flags = fcntl(sock, F_GETFL, 0);
    fcntl(sock, F_SETFL, flags | O_NONBLOCK);
    // Send JOIN until the host accepts.
    char join[32];
    snprintf(join, sizeof(join), "JOIN x4mp\n");
    for (int i = 0; i < 10 && g_streams_running; i++) {
        sendto(sock, join, strlen(join), 0, (struct sockaddr*)&sa, sizeof(sa));
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    char buf[65536];
    while (g_streams_running) {
        struct sockaddr_in from; socklen_t fl = sizeof(from);
        ssize_t n = recvfrom(sock, buf, sizeof(buf) - 1, 0, (struct sockaddr*)&from, &fl);
        if (n <= 0) { std::this_thread::sleep_for(std::chrono::milliseconds(2)); continue; }
        buf[n] = 0;
        char* save = nullptr;
        char* line = strtok_r(buf, "\n", &save);
        while (line) {
            if (strncmp(line, "OBJ", 3) == 0) {
                unsigned long long id, zone, sector; float x, y, z, yaw, pitch, roll; char faction[80], macro[160];
                if (sscanf(line, "OBJ %llu %llu %llu %f %f %f %f %f %f %79s %159s",
                           &id, &zone, &sector, &x, &y, &z, &yaw, &pitch, &roll, faction, macro) == 11) {
                    std::lock_guard<std::mutex> lk(g_data_mutex);
                    ClientObj& o = g_client_objs[id];
                    o.id = id; o.zone = zone; o.sector = sector;
                    o.x = x; o.y = y; o.z = z; o.yaw = yaw; o.pitch = pitch; o.roll = roll;
                    snprintf(o.faction, sizeof(o.faction), "%s", faction);
                    snprintf(o.macro, sizeof(o.macro), "%s", macro);
                }
            }
            line = strtok_r(nullptr, "\n", &save);
        }
    }
    close(sock);
}


// HOST: stream the objects in the player's current zone (ships + stations).
// This mirrors how X4 single-player works: only what is near the player is
// fully rendered/simulated; everything far away is reduced. We enumerate all
// ships/stations at a low frequency and stream only those in the player's zone.
// HOST: refresh the cached list of ALL ships in the universe (across every
// sector), so clients receive the real, complete host state. Enumerating all
// ships is expensive (~85k), so we only do this at a low frequency; positions
// are then streamed every frame from the cache with delta compression.
//
// STATIONS are intentionally NOT streamed: the client loads the SAME (synced)
// save, so it keeps its own stations which preserve sector ownership/claims.
// Streaming stations too would duplicate them on the client.
// Find a host-side client entry by address, or return nullptr.
static NetClient* find_client(const struct sockaddr_in& from);

// HOST: simulation maintenance pass (runs every ~5s while hosting).
//
// 1. Rebuilds the HIGH-SIMULATION sector set: the server player's sector plus
//    every connected client's current sector. Ships in these sectors are
//    ActivateObject()'d so they are FULLY simulated (high attention) even
//    though the local player is not there — the same treatment the server
//    player's own zone gets by default. This is what gives each connected
//    client a fully simulated rendering zone on the host side.
//    (X4MP_FULLSIM=1 instead activates EVERY ship — CPU-heavy, testing only.)
// 2. Places a player-owned simulation satellite in each high-simulation
//    sector (once) so the game keeps it active.
// 3. Keeps the sector macro map fresh (client sector placement) and, when
//    X4MP_STREAMSHIPS=1, the stream cache that feeds the OBJ broadcast.
static void net_maintain_universe() {
    if (!g_game || !g_game->GetAllFactions || !g_game->GetAllFactionShips) return;
    if (!g_game->GetPlayerObjectID || !g_game->GetContextByClass) return;

    g_zone_objs.clear();
    g_zone_objs_zone = 0;

    // --- high-simulation sector set: server player + all connected clients ---
    g_highsim_sectors.clear();
    UniverseID host_player = g_game->GetPlayerObjectID();
    if (host_player) {
        UniverseID hp_sector = g_game->GetContextByClass(host_player, "sector", false);
        if (hp_sector) g_highsim_sectors.insert(hp_sector);
    }
    // Prune client ship state for clients that are no longer connected, and
    // add each live client's sector to the high-simulation set.
    for (auto it = g_client_ship_state.begin(); it != g_client_ship_state.end();) {
        struct sockaddr_in ca; memset(&ca, 0, sizeof(ca));
        ca.sin_family = AF_INET;
        ca.sin_addr.s_addr = (uint32_t)(it->first >> 16);           // network order
        ca.sin_port = htons((uint16_t)(it->first & 0xffff));        // host -> network
        if (!find_client(ca)) {
            // Client is gone: remove its orphaned ghost ship AND ghost stations
            // from the host universe (otherwise they linger after a reconnect).
            auto shipit = g_client_ships.find(it->first);
            if (shipit != g_client_ships.end()) {
                if (g_game->IsValidComponent && g_game->IsValidComponent(shipit->second))
                    g_game->RemoveComponent(shipit->second);
                g_client_ships.erase(shipit);
            }
            auto stnit = g_client_stations.find(it->first);
            if (stnit != g_client_stations.end()) {
                for (UniverseID stn : stnit->second) {
                    if (stn && g_game->IsValidComponent && g_game->IsValidComponent(stn))
                        g_game->RemoveComponent(stn);
                }
                g_client_stations.erase(stnit);
            }
            it = g_client_ship_state.erase(it);
            continue;
        }
        if (it->second.host_sector) g_highsim_sectors.insert(it->second.host_sector);
        ++it;
    }

    // Deduplicate sector macro lookups within this pass (Lua calls are
    // expensive): ~140 unique sectors instead of one per ship.
    std::unordered_map<UniverseID, std::string> sector_macro_cache;
    uint32_t total_ships = 0;

    const char* factions[64];
    uint32_t nf = g_game->GetAllFactions(factions, 64, true);
    for (uint32_t f = 0; f < nf; f++) {
        std::vector<UniverseID> ships;
        uint32_t ns = enumerate_faction_ships(factions[f], ships);
        for (uint32_t i = 0; i < ns; i++) {
            UniverseID obj = ships[i];
            if (g_game->IsValidComponent && !g_game->IsValidComponent(obj)) continue;
            if (host_player && obj == host_player) continue; // streamed via PLAYER cid=0
            if (g_host_only_objects.count(obj)) continue;    // satellites / client ghosts
            total_ships++;
            UniverseID sector = g_game->GetContextByClass(obj, "sector", false);
            if (sector == 0) continue;
            // High simulation for this sector? (server player or a client is here)
            bool highsim = g_fullsim || g_highsim_sectors.count(sector) > 0;
            if (highsim && g_game->ActivateObject) g_game->ActivateObject(obj, true);
            // Player-owned satellite in each high-simulation sector (once).
            if (highsim && g_game->SpawnObjectAtPos2 && g_sat_sectors.insert(sector).second) {
                UIPosRot sp; std::memset(&sp, 0, sizeof(sp));
                g_game->SpawnObjectAtPos2("eq_arg_satellite_01_macro", sector, sp, "player");
            }
            // Sector macro map (client sector placement + high-sim set).
            auto scm = sector_macro_cache.find(sector);
            if (scm == sector_macro_cache.end()) {
                char sm[160]; get_macro(sector, sm, sizeof(sm));
                scm = sector_macro_cache.emplace(sector, std::string(sm[0] ? sm : "?")).first;
            }
            if (scm->second[0] && scm->second[0] != '?') g_host_sector_by_macro[scm->second] = sector;
            // Stream cache (always kept: the per-client zone sync stream and
            // the legacy full broadcast both read from it).
            ZoneObj zo; zo.id = obj; zo.sector = sector;
            snprintf(zo.faction, sizeof(zo.faction), "%s", factions[f] ? factions[f] : "player");
            auto msc = g_ship_macro_cache.find(obj);
            if (msc != g_ship_macro_cache.end()) {
                snprintf(zo.macro, sizeof(zo.macro), "%s", msc->second.c_str());
            } else {
                char macro[160]; get_macro(obj, macro, sizeof(macro));
                const char* m = macro[0] ? macro : "?";
                snprintf(zo.macro, sizeof(zo.macro), "%s", m);
                g_ship_macro_cache[obj] = m;
            }
            snprintf(zo.sector_macro, sizeof(zo.sector_macro), "%s", scm->second.c_str());
            g_zone_objs.push_back(zo);
        }
    }
    // Rebuild the per-sector index (pointers into g_zone_objs, valid until
    // the next maintenance pass).
    g_sector_ship_index.clear();
    for (auto& zo : g_zone_objs) g_sector_ship_index[zo.sector].push_back(&zo);
    g_last_index_rebuild = std::chrono::steady_clock::now();
    if (g_debug) {
        char dbg[220];
        snprintf(dbg, sizeof(dbg), "x4mp: [DBG] HOST refreshed ALL ships: %u (highsim sectors: %zu, streamed: %zu)",
                 total_ships, g_highsim_sectors.size(), g_zone_objs.size());
        net_debug(dbg);
    }
}

// HOST: stream the current positions of the cached zone objects to clients.
// Called every frame (fast — no enumeration). All OBJ lines are batched into a
// single UDP packet to avoid flooding the network with 200 packets/frame.
// Send a filled OBJ batch (one UDP packet) to every client's stream port.
static void net_send_obj_batch(const char* batch, size_t len) {
    if (len == 0) return;
    char tmp[60000];
    size_t n = len < sizeof(tmp) - 1 ? len : sizeof(tmp) - 1;
    memcpy(tmp, batch, n);
    tmp[n] = 0;
    net_send_stream(tmp);
}

// HOST: send a batch to ONE client over the UDP data stream (fire-and-forget;
// a full buffer just drops the datagram — the next tick resends the full state).
static void net_send_udp_to(NetClient& c, const char* msg, size_t len) {
    // Consolidated UDP mode (X4MP_LEGACY_NET=0, udp): the data rides the SAME
    // control socket (g_sock, 7777) as the control. Legacy UDP mode: the data
    // uses a separate socket (g_stream_udp_sock, 7778).
    int sock = g_legacy_net ? g_stream_udp_sock : g_sock;
    if (sock < 0) return;
    struct sockaddr_in sa = c.addr;
    if (g_legacy_net) sa.sin_port = htons(c.stream_port); // 7778
    // else: sa.sin_port stays c.addr.sin_port (7777, the control port)
    size_t off = 0;
    while (off < len) {
        size_t chunk = len - off;
        if (chunk > 60000) chunk = 60000;
        ssize_t w = sendto(sock, msg + off, chunk, 0, (struct sockaddr*)&sa, sizeof(sa));
        if (w < 0) { if (errno == EINTR) continue; break; } // drop on error (UDP)
        off += (size_t)w;
    }
}

// Send a filled OBJ batch to ONE client over its data stream (TCP or UDP).
static void net_send_obj_batch_to(NetClient* c, const char* batch, size_t len) {
    if (len == 0) return;
    char tmp[60000];
    size_t n = len < sizeof(tmp) - 1 ? len : sizeof(tmp) - 1;
    memcpy(tmp, batch, n);
    tmp[n] = 0;
    if (g_transport == "udp") net_send_udp_to(*c, tmp, n);
    else net_send_tcp(*c, tmp, n);
}

// HOST: stream the ships in ONE client's current sector (per-client sync).
//
// This is the reconciliation feed: the client binds these ships to its own
// locally simulated ships and corrects them, so every client (and the host)
// shows the same positions. A FULL sector snapshot is sent when the client
// joins or changes sector (c.needs_full); afterwards only ships that moved
// beyond g_delta_m are re-sent (per-client delta state).
static void net_send_client_objects(NetClient& c) {
    if (!c.cur_sector) return;
    auto sit = g_sector_ship_index.find(c.cur_sector);
    if (sit == g_sector_ship_index.end() || sit->second.empty()) {
        // No streamable ships in this sector: still send an (empty) FULL
        // snapshot so the client can prune local ships that died on the host.
        // But ONLY once the index has been rebuilt since this client's sector
        // changed — a not-yet-indexed sector looks empty and would otherwise
        // make the client prune its entire local sector.
        bool index_current = (g_last_index_rebuild >= c.cur_sector_set_time);
        if (c.needs_full && !g_zone_objs.empty() && index_current) {
            c.needs_full = false;
            net_send_obj_batch_to(&c, "FULL 1\n", 7);
        }
        return;
    }

    char batch[60000];
    size_t used = 0;
    // Always send the COMPLETE sector state every tick. LAN bandwidth is cheap,
    // and a full authoritative snapshot every tick keeps the client's view
    // exact — no delta-compression staleness to cause prune/respawn flicker.
    const char* full = "FULL 1\n";
    memcpy(batch, full, 6);
    used = 6;
    for (ZoneObj* zo : sit->second) {
        UniverseID obj = (UniverseID)zo->id;
        if (g_game->IsValidComponent && !g_game->IsValidComponent(obj)) continue;
        UIPosRot pos = g_game->GetObjectPositionInSector(obj);
        char buf[200];
        int n = snprintf(buf, sizeof(buf),
                         "OBJ %llu %s %.3f %.3f %.3f %.3f %.3f %.3f %s %s\n",
                         (unsigned long long)obj, zo->sector_macro,
                         pos.x, pos.y, pos.z, pos.yaw, pos.pitch, pos.roll,
                         zo->faction, zo->macro);
        if (used + (size_t)n >= sizeof(batch)) {
            net_send_obj_batch_to(&c, batch, used);
            used = 0;
        }
        memcpy(batch + used, buf, (size_t)n);
        used += (size_t)n;
    }
    if (used > 0) net_send_obj_batch_to(&c, batch, used);
    c.needs_full = false; // complete state sent this tick; reset the flag
}

static void net_send_objects_host() {
    if (!g_game || g_clients.empty() || g_zone_objs.empty()) return;
    char batch[60000];
    size_t used = 0;
    // Prune stale cached IDs. Objects in the cache may have been destroyed since
    // the last refresh; calling GetContextByClass/GetObjectPositionInSector on a
    // dead ID makes the engine log "Failed to retrieve component/object" every
    // frame (log flood). We validate each entry and drop dead ones immediately.
    std::vector<ZoneObj> live;
    live.reserve(g_zone_objs.size());
    // Delta compression: only re-send an object if it moved beyond g_delta_m
    // (or on a periodic full refresh). This drastically cuts bandwidth/CPU.
    bool force_full = (g_net_tick % g_full_refresh_interval) == 0;
    size_t streamed = 0;
    for (auto& zo : g_zone_objs) {
        UniverseID obj = (UniverseID)zo.id;
        // Skip objects that no longer exist (destroyed). IsValidComponent avoids
        // the engine's "Failed to retrieve" warnings for dead IDs.
        if (g_game->IsValidComponent && !g_game->IsValidComponent(obj)) continue;
        UIPosRot pos = g_game->GetObjectPositionInSector(obj);
        // Delta check: skip if not moved enough and not a forced full refresh.
        if (!force_full && zo.sent) {
            float dx = pos.x - zo.last_x, dy = pos.y - zo.last_y, dz = pos.z - zo.last_z;
            if ((dx*dx + dy*dy + dz*dz) < g_delta_m * g_delta_m) continue;
        }
        zo.last_x = pos.x; zo.last_y = pos.y; zo.last_z = pos.z; zo.sent = true;
        char buf[700];
        int n = snprintf(buf, sizeof(buf),
                         "OBJ %llu %s %.3f %.3f %.3f %.3f %.3f %.3f %s %s\n",
                         (unsigned long long)obj, zo.sector_macro,
                         pos.x, pos.y, pos.z, pos.yaw, pos.pitch, pos.roll,
                         zo.faction, zo.macro);
        // Multi-packet batching: flush when the packet is nearly full so a full
        // universe (~85k ships) is streamed in many packets, not one overflow.
        if (used + (size_t)n >= sizeof(batch)) {
            net_send_obj_batch(batch, used);
            used = 0;
        }
        memcpy(batch + used, buf, (size_t)n);
        used += (size_t)n;
        streamed++;
        live.push_back(zo);
    }
    // Update the cache with only live objects so dead IDs are not re-streamed.
    if (live.size() != g_zone_objs.size()) g_zone_objs = std::move(live);
    if (used > 0) net_send_obj_batch(batch, used);
    if (g_debug && (g_net_tick % 300) == 0) {
        char dbg[160];
        snprintf(dbg, sizeof(dbg), "x4mp: [DBG] HOST OBJ streamed %zu ships%s (cache %zu)", streamed, force_full ? " (FULL)" : "", g_zone_objs.size());
        net_debug(dbg);
    }
}

// HOST: FULL mode — enumerate ALL ships/stations and stream every frame (no
// cache). Expensive, but tests how usable streaming every frame really is.
static void net_send_objects_host_full() {
    if (!g_game || g_clients.empty()) return;
    if (!g_game->GetPlayerZoneID || !g_game->GetAllFactions || !g_game->GetAllFactionShips) return;
    UniverseID player_zone = g_game->GetPlayerZoneID();
    if (player_zone == 0) return;
    const char* factions[64];
    uint32_t nf = g_game->GetAllFactions(factions, 64, true);
    uint32_t total_seen = 0, total_streamed = 0;
    for (uint32_t f = 0; f < nf; f++) {
        std::vector<UniverseID> ships;
        uint32_t ns = enumerate_faction_ships(factions[f], ships);
        for (uint32_t i = 0; i < ns; i++) {
            UniverseID obj = ships[i];
            total_seen++;
            UniverseID zone = g_game->GetContextByClass ? g_game->GetContextByClass(obj, "zone", false) : 0;
            if (zone != player_zone) continue;
            UIPosRot pos = g_game->GetObjectPositionInSector(obj);
            UniverseID sector = g_game->GetContextByClass ? g_game->GetContextByClass(obj, "sector", false) : 0;
            char macro[160]; get_macro(obj, macro, sizeof(macro));
            char buf[600];
            snprintf(buf, sizeof(buf), "OBJ %llu %llu %llu %.3f %.3f %.3f %.3f %.3f %.3f %s %s\n",
                     (unsigned long long)obj, (unsigned long long)zone, (unsigned long long)sector,
                     pos.x, pos.y, pos.z, pos.yaw, pos.pitch, pos.roll,
                     factions[f] ? factions[f] : "player",
                     (macro[0]) ? macro : "?");
            net_send_stream(buf);
            total_streamed++;
        }
        {
            std::vector<UniverseID> stations;
            uint32_t nst = enumerate_faction_stations(factions[f], stations);
            for (uint32_t i = 0; i < nst; i++) {
                UniverseID obj = stations[i];
                total_seen++;
                UniverseID zone = g_game->GetContextByClass ? g_game->GetContextByClass(obj, "zone", false) : 0;
                if (zone != player_zone) continue;
                UIPosRot pos = g_game->GetObjectPositionInSector(obj);
                UniverseID sector = g_game->GetContextByClass ? g_game->GetContextByClass(obj, "sector", false) : 0;
                char macro[160]; get_macro(obj, macro, sizeof(macro));
                char buf[600];
                snprintf(buf, sizeof(buf), "OBJ %llu %llu %llu %.3f %.3f %.3f %.3f %.3f %.3f %s %s\n",
                         (unsigned long long)obj, (unsigned long long)zone, (unsigned long long)sector,
                         pos.x, pos.y, pos.z, pos.yaw, pos.pitch, pos.roll,
                         factions[f] ? factions[f] : "player",
                         (macro[0]) ? macro : "?");
                net_send_stream(buf);
                total_streamed++;
            }
        }
    }
    if (g_debug) {
        char dbg[160];
        snprintf(dbg, sizeof(dbg), "x4mp: [DBG] HOST zone=%llu: saw %u objects, streamed %u",
                 (unsigned long long)player_zone, total_seen, total_streamed);
        net_debug(dbg);
    }
}

// THIN CLIENT: render the host's player state by moving our local player to
// the host's position. The local simulation is paused, so this is the only
// thing driving the client's rendered world.
static void thin_client_render() {
    if (!g_game || !g_client_ready || g_snap_id == 0) return;
    if (!g_game->MovePlayerToSectorPos) return;
    // The host and client are in different universes with runtime-generated
    // zone/sector IDs that do NOT match. So move the client's player to the
    // host's position within the CLIENT's OWN player sector (same geometry as
    // the host's, since we loaded the same save). Using the host's zone ID here
    // fails because that zone does not exist on the client.
    UniverseID player_zone = g_game->GetPlayerZoneID ? g_game->GetPlayerZoneID() : 0;
    UniverseID client_sector = (g_game->GetContextByClass && player_zone)
                                   ? g_game->GetContextByClass(player_zone, "sector", false)
                                   : 0;
    if (!client_sector) return;
    UIPosRot pos;
    pos.x = g_snap_x; pos.y = g_snap_y; pos.z = g_snap_z;
    pos.yaw = 0; pos.pitch = 0; pos.roll = 0;
    g_game->MovePlayerToSectorPos(client_sector, pos);
    if (g_debug) {
        char dbg[160];
        snprintf(dbg, sizeof(dbg), "x4mp: [DBG] CLIENT applied player pos=(%.1f,%.1f,%.1f) sector=%llu",
                 g_snap_x, g_snap_y, g_snap_z, (unsigned long long)client_sector);
        net_debug(dbg);
    }
}

// THIN CLIENT: create/position objects received from the host's OBJ stream, so
// the client renders the host's world instead of its own. Objects are created
// once (SpawnObjectAtPos2) and repositioned each frame (SetObjectSectorPos).
static void thin_client_render_objects() {
    if (!g_game || !g_client_ready || g_client_objs.empty()) return;
    if (!g_game->SpawnObjectAtPos2 || !g_game->SetObjectSectorPos) return;
    // The host and client are in different universes with runtime-generated
    // sector IDs that do NOT match. So create objects in the CLIENT's own
    // player sector, using the host's absolute positions. Because the client's
    // player is placed at the host's position, the relative offsets match.
    UniverseID player_zone = g_game->GetPlayerZoneID ? g_game->GetPlayerZoneID() : 0;
    UniverseID client_sector = (g_game->GetContextByClass && player_zone)
                                   ? g_game->GetContextByClass(player_zone, "sector", false)
                                   : 0;
    if (g_debug) {
        char dbg[256];
        snprintf(dbg, sizeof(dbg), "x4mp: [DBG] CLIENT render_objects: player_zone=%llu client_sector=%llu objs=%zu",
                 (unsigned long long)player_zone, (unsigned long long)client_sector, g_client_objs.size());
        net_debug(dbg);
    }
    if (!client_sector) return;
    std::lock_guard<std::mutex> lk(g_data_mutex);
    for (auto& kv : g_client_objs) {
        ClientObj& o = kv.second;
        UIPosRot offset;
        offset.x = o.x; offset.y = o.y; offset.z = o.z;
        offset.yaw = o.yaw; offset.pitch = o.pitch; offset.roll = o.roll;
        auto it = g_idmap.find(o.id);
        if (it == g_idmap.end()) {
            UniverseID newid = g_game->SpawnObjectAtPos2(o.macro, client_sector, offset,
                                                         (o.faction[0]) ? o.faction : "player");
            if (newid != 0) {
                g_idmap[o.id] = newid;
                if (g_debug) {
                    char dbg[256];
                    snprintf(dbg, sizeof(dbg),
                             "x4mp: [DBG] CLIENT spawned host obj id=%llu macro=%s as client id=%llu sector=%llu",
                             o.id, o.macro, newid, (unsigned long long)client_sector);
                    net_debug(dbg);
                }
            } else if (g_debug) {
                char dbg[256];
                snprintf(dbg, sizeof(dbg),
                         "x4mp: [DBG] CLIENT SpawnObjectAtPos2 FAILED macro=%s sector=%llu pos=(%.1f,%.1f,%.1f)",
                         o.macro, (unsigned long long)client_sector, o.x, o.y, o.z);
                net_debug(dbg);
            }
        } else {
            g_game->SetObjectSectorPos((UniverseID)it->second, client_sector, offset);
        }
    }
}

// OPTION 2: remove the client's own SAVE SHIPS (except the player's ship) so
// the client does not double-render the host's ships (x4mp_stream already
// spawns the authoritative host ships).
//
// STATIONS ARE KEPT: they are static and define sector ownership/claims. The
// client loads the SAME (synced) save as the host, so its own stations match
// the host's. Removing them would wipe every station + sector claim on the
// client, making the universe inaccurate. So we only remove duplicate ships.
static void thin_client_cleanup_own_objects() {
    if (!g_game || g_cleanup_done) return;
    if (!g_game->RemoveComponent || !g_game->GetAllFactions || !g_game->GetAllFactionShips) return;
    // Never remove the player's own ship — in ANY state (flying, docked, in a
    // hangar). GetPlayerControlledShipID returns 0 for a DOCKED player, so the
    // old single-ID exclusion would delete the docked player ship ->
    // "Game Over (killmethod=removed)" -> menu -> save-reload loop.
    UniverseID pids[5] = {0, 0, 0, 0, 0};
    pids[0] = g_game->GetPlayerControlledShipID ? g_game->GetPlayerControlledShipID() : 0;
    pids[1] = g_game->GetPlayerShipID ? g_game->GetPlayerShipID() : 0;
    pids[2] = g_game->GetPlayerOccupiedShipID ? g_game->GetPlayerOccupiedShipID() : 0;
    pids[3] = g_game->GetPlayerObjectID ? g_game->GetPlayerObjectID() : 0;
    pids[4] = (pids[3] && g_game->GetContextByClass) ? g_game->GetContextByClass(pids[3], "ship", false) : 0;
    auto is_player = [&](UniverseID s) {
        for (int i = 0; i < 5; i++) if (pids[i] && pids[i] == s) return true;
        return false;
    };
    const char* factions[64];
    uint32_t nf = g_game->GetAllFactions(factions, 64, true);
    uint32_t removed = 0, total_ships = 0;
    for (uint32_t f = 0; f < nf; f++) {
        std::vector<UniverseID> ships;
        uint32_t ns = enumerate_faction_ships(factions[f], ships);
        total_ships += ns;
        for (uint32_t i = 0; i < ns; i++) {
            if (is_player(ships[i])) continue;
            g_game->RemoveComponent(ships[i]);
            removed++;
        }
        // Stations intentionally NOT removed (sector claims must survive).
    }
    g_cleanup_done = true;
    if (g_debug) {
        char dbg[200];
        snprintf(dbg, sizeof(dbg),
                 "x4mp: [DBG] CLIENT cleanup: %u factions, %u ships, removed %u ships (kept player ships [%llu..%llu]; stations kept)",
                 nf, total_ships, removed, (unsigned long long)pids[0], (unsigned long long)pids[4]);
        net_debug(dbg);
    }
}

// Poll the socket for incoming datagrams and handle the handshake.
// Process a single received message line (from host or client). Messages may
// be batched into one UDP packet, so net_poll splits on newlines and calls this
// for each line.
// Find a host-side client entry by address, or return nullptr.
static NetClient* find_client(const struct sockaddr_in& from) {
    for (auto& c : g_clients)
        if (c.addr.sin_port == from.sin_port && c.addr.sin_addr.s_addr == from.sin_addr.s_addr)
            return &c;
    return nullptr;
}

// Trading (host): converge a ghost ship's cargo to a target snapshot. Diff the
// ghost's current cargo (GetCargo) against `target` and apply AddTradeWare
// (buys) / DropCargo (sells). Called from process_message (main thread).
static void host_apply_cargo(UniverseID ghost,
                             const std::vector<std::pair<std::string,int>>& target) {
    if (!g_game || !ghost || !g_game->IsValidComponent || !g_game->IsValidComponent(ghost)) return;
    if (!g_game->GetNumCargo || !g_game->GetCargo) return;
    std::unordered_map<std::string,int> cur;
    uint32_t n = g_game->GetNumCargo(ghost, "");
    if (n > 0) {
        std::vector<UIWareInfo> buf(n);
        uint32_t got = g_game->GetCargo(buf.data(), n, ghost, "");
        for (uint32_t i = 0; i < got; i++)
            if (buf[i].ware) cur[buf[i].ware] = buf[i].amount;
    }
    for (auto& kv : cur) {
        int tgt = 0;
        for (auto& t : target) if (t.first == kv.first) { tgt = t.second; break; }
        int delta = kv.second - tgt;
        if (delta > 0 && g_game->DropCargo)
            g_game->DropCargo(ghost, kv.first.c_str(), (uint32_t)delta);
    }
    for (auto& t : target) {
        int curamt = cur.count(t.first) ? cur[t.first] : 0;
        int delta = t.second - curamt;
        if (delta > 0 && g_game->AddTradeWare)
            for (int i = 0; i < delta; i++) g_game->AddTradeWare(ghost, t.first.c_str());
    }
}

// Trading: locate the station a trade happened at. Primary: the station's
// UniverseID (both sides load the same save, so static-object IDs match —
// unlike ships, which diverge via spawn/death). Fallback: enumerate all
// stations and match by position (within ~50 m) — stations are static.
static UniverseID find_station_by_trade(unsigned long long uid, float x, float y, float z) {
    if (!g_game || !g_game->IsValidComponent) return 0;
    if (uid && g_game->IsValidComponent((UniverseID)uid)) return (UniverseID)uid;
    if (!g_game->GetAllFactions || !g_game->GetObjectPositionInSector) return 0;
    const char* factions[64];
    uint32_t nf = g_game->GetAllFactions(factions, 64, true);
    UniverseID best = 0; float best_d2 = 50.0f * 50.0f;
    for (uint32_t f = 0; f < nf; f++) {
        std::vector<UniverseID> stns;
        uint32_t ns = enumerate_faction_stations(factions[f], stns);
        for (uint32_t i = 0; i < ns; i++) {
            UniverseID s = stns[i];
            if (g_game->IsValidComponent && !g_game->IsValidComponent(s)) continue;
            UIPosRot p = g_game->GetObjectPositionInSector(s);
            float dx = p.x - x, dy = p.y - y, dz = p.z - z;
            float d2 = dx*dx + dy*dy + dz*dz;
            if (d2 < best_d2) { best_d2 = d2; best = s; }
        }
    }
    return best;
}

// Trading: apply a discrete trade to a station. delta < 0 -> the station LOSES
// that many units (player bought) -> DropCargo (bulk). delta > 0 -> the station
// GAINS them (player sold) -> AddTradeWare (single-unit, bounded by ship cargo).
static void host_apply_trade(UniverseID station, const std::vector<std::pair<std::string,int>>& deltas) {
    if (!station || !g_game || !g_game->IsValidComponent || !g_game->IsValidComponent(station)) return;
    for (auto& d : deltas) {
        if (d.second < 0) {
            if (g_game->DropCargo) g_game->DropCargo(station, d.first.c_str(), (uint32_t)(-d.second));
        } else if (d.second > 0) {
            if (g_game->AddTradeWare)
                for (int i = 0; i < d.second; i++) g_game->AddTradeWare(station, d.first.c_str());
        }
    }
}

static void process_message(const char* msg, const struct sockaddr_in& from) {
    if (g_net_host) {
        if (strncmp(msg, "JOIN", 4) == 0) {
            NetClient* c = find_client(from);
            if (!c) {
                NetClient nc;
                nc.addr = from;
                nc.last_seen = std::chrono::steady_clock::now();
                // Assign this client a UNIQUE, never-reused player faction so
                // it is distinguishable from the host and from every other
                // client (supports many simultaneous clients, e.g. 8).
                nc.id = g_next_client_id++;
                g_clients.push_back(nc);
                int id = nc.id;
                client_faction_for(id, g_clients.back().faction, sizeof(g_clients.back().faction));
                // A brand-new client has no objects yet, so force a full object
                // stream (clear delta state) so it gets the complete zone.
                for (auto& zo : g_zone_objs) zo.sent = false;
                char resp[64];
                snprintf(resp, sizeof(resp), "WELCOME %d\n", id);
                net_send(&from, resp);
                char lbuf[192];
                snprintf(lbuf, sizeof(lbuf), "x4mp: net: client joined (id=%d, %s:%u) faction=%s", id,
                         inet_ntoa(from.sin_addr), (unsigned)ntohs(from.sin_port),
                         g_clients.back().faction);
                net_log(lbuf);
            } else {
                // Re-join from an existing client (e.g. after a link drop):
                // refresh liveness and re-send WELCOME so the client can
                // re-establish its session id.
                c->last_seen = std::chrono::steady_clock::now();
                int id = c->id;
                char resp[64];
                snprintf(resp, sizeof(resp), "WELCOME %d\n", id);
                net_send_ctrl(c, resp);
            }
        } else if (strncmp(msg, "INPUT", 5) == 0) {
            // Client input/command received. Refresh liveness; log occasionally.
            NetClient* c = find_client(from);
            if (c) c->last_seen = std::chrono::steady_clock::now();
            if ((g_net_tick % 60) == 0) {
                char lbuf[192];
                snprintf(lbuf, sizeof(lbuf), "x4mp: net: HOST got INPUT from %s: %s",
                         inet_ntoa(from.sin_addr), msg);
                net_log(lbuf);
            }
        } else if (strncmp(msg, "PONG", 4) == 0) {
            // Client responded to our PING keepalive.
            NetClient* c = find_client(from);
            if (c) c->last_seen = std::chrono::steady_clock::now();
        } else if (strncmp(msg, "LOADING", 7) == 0) {
            // Client is loading its save — exempt from dead-client pruning.
            NetClient* c = find_client(from);
            if (c) c->loading = true;
        } else if (strncmp(msg, "READY", 5) == 0) {
            // Client finished loading — resume normal liveness pruning.
            NetClient* c = find_client(from);
            if (c) { c->loading = false; c->last_seen = std::chrono::steady_clock::now(); }
        } else if (strncmp(msg, "STREAM", 6) == 0) {
            // Client reports its dedicated stream port (x4mp_stream). The host
            // sends OBJ/PLAYER rendering data to this port, not the control port.
            int port = 7778;
            sscanf(msg + 6, "%d", &port);
            NetClient* c = find_client(from);
            if (c && port > 0 && port < 65535) c->stream_port = (uint16_t)port;
        } else if (strncmp(msg, "ACT", 3) == 0) {
            // Client ACTION (player-action replication channel). The client
            // reports an action it performed (fire/build/trade/board); the host
            // will execute it authoritatively and broadcast the result. Transport
            // is in place; per-action execution is added incrementally. Log every
            // received action (it is low-rate: only on real actions).
            NetClient* c = find_client(from);
            if (c) c->last_seen = std::chrono::steady_clock::now();
            // ACT BUILD <seq> <macro> <x> <y> <z> <yaw> <pitch> <roll> <sector_macro>
            // Issue 3: a station the client built after load. Spawn a ghost of
            // it in the host's universe under the client's faction (so it never
            // streams back to the building client, which already has it).
            if (strncmp(msg + 4, "BUILD", 5) == 0 && g_game && g_game->SpawnObjectAtPos2) {
                uint32_t seq; char macro[160], csector[160] = {0};
                float x, y, z, yaw, pitch, roll;
                if (sscanf(msg + 10, "%u %159s %f %f %f %f %f %f %159s",
                           &seq, macro, &x, &y, &z, &yaw, &pitch, &roll, csector) >= 8) {
                    UniverseID sector = 0;
                    if (csector[0] && g_host_sector_by_macro.count(csector))
                        sector = g_host_sector_by_macro[csector];
                    if (!sector && g_game->GetPlayerZoneID && g_game->GetContextByClass)
                        sector = g_game->GetContextByClass(g_game->GetPlayerZoneID(), "sector", false);
                    uint64_t key = ((uint64_t)from.sin_addr.s_addr << 16) | (uint32_t)ntohs(from.sin_port);
                    // Lazy faction computation (JOIN-time assignment is too
                    // early — the universe may not exist yet).
                    char cfaction[80];
                    client_faction_for(c ? c->id : 1, cfaction, sizeof(cfaction));
                    UIPosRot pos; pos.x = x; pos.y = y; pos.z = z;
                    pos.yaw = yaw; pos.pitch = pitch; pos.roll = roll;
                    std::vector<UniverseID>& spawned = g_client_stations[key];
                    if ((int)spawned.size() <= (int)seq) spawned.resize((int)seq + 1, 0);
                    if (spawned[seq] == 0 && sector && cfaction[0]) {
                        UniverseID stn = g_game->SpawnObjectAtPos2(macro[0] ? macro : "?", sector, pos, cfaction);
                        spawned[seq] = stn; // remember even if 0 (avoid spawn retry storms)
                        if (stn) g_host_only_objects.insert(stn);
                        if (g_debug) {
                            char dbg[300];
                            snprintf(dbg, sizeof(dbg), "x4mp: [DBG] HOST spawned client station ghost seq=%u id=%llu macro=%s sector=%llu faction=%s",
                                     seq, (unsigned long long)stn, macro, (unsigned long long)sector, cfaction);
                            net_debug(dbg);
                        }
                    }
                }
            }
            // Combat: ACT KILL <host_id> — a client's player destroyed ship
            // host_id (its local sim already removed it). Remove the host's
            // authoritative copy and re-broadcast so every client drops it.
            if (strncmp(msg + 4, "KILL", 4) == 0) {
                unsigned long long hid = 0;
                if (sscanf(msg + 8, "%llu", &hid) == 1 && hid) {
                    if (g_game && g_game->IsValidComponent && g_game->RemoveComponent
                        && g_game->IsValidComponent((UniverseID)hid)) {
                        g_game->RemoveComponent((UniverseID)hid);
                        g_host_only_objects.erase((UniverseID)hid);
                    }
                    char b[64];
                    snprintf(b, sizeof(b), "KILL %llu\n", (unsigned long long)hid);
                    net_send_stream(b); // data channel -> x4mp_stream on all clients
                    if (g_debug) { char dbg[160]; snprintf(dbg, sizeof(dbg), "x4mp: [DBG] HOST kill ship id=%llu (broadcast)", (unsigned long long)hid); net_debug(dbg); }
                }
            } else if (strncmp(msg + 4, "PLAYERDIED", 10) == 0) {
                // Combat: this client's player ship died. Remove its ghost and
                // relay the death so OTHER clients drop this client's player.
                uint64_t key = ((uint64_t)from.sin_addr.s_addr << 16) | (uint32_t)ntohs(from.sin_port);
                auto git = g_client_ships.find(key);
                if (git != g_client_ships.end()) {
                    if (g_game && g_game->IsValidComponent && g_game->RemoveComponent
                        && g_game->IsValidComponent(git->second))
                        g_game->RemoveComponent(git->second);
                    g_host_only_objects.erase(git->second);
                    g_client_ships.erase(git);
                    g_client_ship_state.erase(key);
                }
                char b[64];
                snprintf(b, sizeof(b), "PLAYERDIED %llu\n", (unsigned long long)key);
                for (auto& cl : g_clients) {
                    if (cl.addr.sin_port == from.sin_port && cl.addr.sin_addr.s_addr == from.sin_addr.s_addr) continue;
                    net_send_tcp(cl, b, strlen(b));
                }
                if (g_debug) { char dbg[160]; snprintf(dbg, sizeof(dbg), "x4mp: [DBG] HOST player died (key=%llu), ghost removed + relayed", (unsigned long long)key); net_debug(dbg); }
            } else if (strncmp(msg + 4, "CARGO", 5) == 0) {
                // Trading: ACT CARGO <count> <ware> <amt> ... — a client's player
                // cargo changed. Converge this client's ghost cargo and re-
                // broadcast so other clients' view of the player matches.
                uint64_t key = ((uint64_t)from.sin_addr.s_addr << 16) | (uint32_t)ntohs(from.sin_port);
                // Parse the ware/amount pairs after "ACT CARGO <count>".
                const char* s = msg;
                std::vector<std::string> tokens;
                while (*s) {
                    while (*s == ' ') s++;
                    if (!*s) break;
                    const char* st = s;
                    while (*s && *s != ' ') s++;
                    tokens.push_back(std::string(st, (size_t)(s - st)));
                }
                // tokens: [ACT][CARGO][count][ware][amt]...
                if (tokens.size() >= 3) {
                    std::vector<std::pair<std::string,int>> wares;
                    for (size_t i = 3; i + 1 < tokens.size(); i += 2)
                        wares.push_back({ tokens[i], atoi(tokens[i + 1].c_str()) });
                    auto git = g_client_ships.find(key);
                    if (git != g_client_ships.end()) {
                        host_apply_cargo(git->second, wares);
                    }
                    // Re-broadcast to the OTHER clients (keyed by this sender's
                    // key). msg = "ACT CARGO <count> <ware> <amt>...\n"; the
                    // payload after "ACT CARGO " is "<count> <ware> <amt>...\n".
                    char b[16384];
                    snprintf(b, sizeof(b), "CARGO %llu %s", (unsigned long long)key, msg + 10);
                    for (auto& cl : g_clients) {
                        if (cl.addr.sin_port == from.sin_port && cl.addr.sin_addr.s_addr == from.sin_addr.s_addr) continue;
                        net_send_tcp(cl, b, strlen(b));
                    }
                    if (g_debug) { char dbg[200]; snprintf(dbg, sizeof(dbg), "x4mp: [DBG] HOST cargo sync (key=%llu, %d wares) applied + relayed", (unsigned long long)key, (int)wares.size()); net_debug(dbg); }
                }
            } else if (strncmp(msg + 4, "TRADE", 5) == 0) {
                // Trading (host-authoritative): ACT TRADE <uid> <x> <y> <z>
                // <ware> <delta>... — a client's player traded at a station.
                // Apply the discrete trade to the host's copy of that station
                // (authoritative) and relay to the other clients.
                const char* s = msg;
                std::vector<std::string> tokens;
                while (*s) {
                    while (*s == ' ') s++;
                    if (!*s) break;
                    const char* st = s;
                    while (*s && *s != ' ') s++;
                    tokens.push_back(std::string(st, (size_t)(s - st)));
                }
                // tokens: [ACT][TRADE][uid][x][y][z][ware][delta]...
                if (tokens.size() >= 7) {
                    unsigned long long uid = strtoull(tokens[2].c_str(), nullptr, 10);
                    float fx = (float)atof(tokens[3].c_str());
                    float fy = (float)atof(tokens[4].c_str());
                    float fz = (float)atof(tokens[5].c_str());
                    std::vector<std::pair<std::string,int>> deltas;
                    for (size_t i = 6; i + 1 < tokens.size(); i += 2)
                        deltas.push_back({ tokens[i], atoi(tokens[i + 1].c_str()) });
                    UniverseID station = find_station_by_trade(uid, fx, fy, fz);
                    if (station) host_apply_trade(station, deltas);
                    // Relay to the OTHER clients (they apply it to their station).
                    char b[16384];
                    snprintf(b, sizeof(b), "%s", msg + 4); // "TRADE <uid> <x> <y> <z> <ware> <delta>..."
                    for (auto& cl : g_clients) {
                        if (cl.addr.sin_port == from.sin_port && cl.addr.sin_addr.s_addr == from.sin_addr.s_addr) continue;
                        net_send_tcp(cl, b, strlen(b));
                    }
                    if (g_debug) { char dbg[200]; snprintf(dbg, sizeof(dbg), "x4mp: [DBG] HOST trade applied to station=%llu (uid=%llu, %d wares) + relayed", (unsigned long long)station, (unsigned long long)uid, (int)deltas.size()); net_debug(dbg); }
                }
            } else if (strncmp(msg + 4, "CAPTURE", 7) == 0) {
                // Boarding: ACT CAPTURE <host_id> — a client captured ship
                // host_id (boarded + took it). Make it authoritative: transfer
                // the target to the client's ghost faction and broadcast so
                // every client shows the capture.
                unsigned long long hid = 0;
                if (sscanf(msg + 11, "%llu", &hid) == 1 && hid &&
                    g_game && g_game->IsValidComponent && g_game->SetComponentOwner &&
                    g_game->IsValidComponent((UniverseID)hid)) {
                    NetClient* sender = find_client(from);
                    char cfaction[80] = {0};
                    if (sender) client_faction_for(sender->id, cfaction, sizeof(cfaction));
                    if (cfaction[0]) g_game->SetComponentOwner((UniverseID)hid, cfaction);
                    char b[160];
                    snprintf(b, sizeof(b), "CAPTURE %llu %s\n", (unsigned long long)hid, cfaction[0] ? cfaction : "player");
                    for (auto& cl : g_clients) {
                        if (cl.addr.sin_port == from.sin_port && cl.addr.sin_addr.s_addr == from.sin_addr.s_addr) continue;
                        net_send_tcp(cl, b, strlen(b));
                    }
                    if (g_debug) { char dbg[200]; snprintf(dbg, sizeof(dbg), "x4mp: [DBG] HOST capture id=%llu -> faction %s (relayed)", (unsigned long long)hid, cfaction[0] ? cfaction : "?"); net_debug(dbg); }
                }
            }
            char lbuf[256];
            snprintf(lbuf, sizeof(lbuf), "x4mp: [ACT] from %s (id=%d): %s",
                     inet_ntoa(from.sin_addr), c ? c->id : -1, msg);
            net_log(lbuf);
        } else if (strncmp(msg, "PLAYER", 6) == 0) {
            // Client reports its ship: PLAYER <x> <y> <z> <yaw> <pitch> <roll> <macro> [sector_macro]
            // 1) place a ghost in the client's ACTUAL sector (mapped to host),
            // 2) move the host player into that sector so the game fully simulates it,
            // 3) relay the client's ship to all OTHER clients.
            float x, y, z, yaw, pitch, roll; char macro[160], csector[160] = {0};
            if (sscanf(msg, "PLAYER %f %f %f %f %f %f %159s %159s",
                       &x, &y, &z, &yaw, &pitch, &roll, macro, csector) >= 7) {
                uint64_t key = ((uint64_t)from.sin_addr.s_addr << 16) | (uint32_t)ntohs(from.sin_port);
                ClientShipState& st = g_client_ship_state[key];
                st.x = x; st.y = y; st.z = z; st.yaw = yaw; st.pitch = pitch; st.roll = roll;
                snprintf(st.macro, sizeof(st.macro), "%s", macro[0] ? macro : "?");
                if (csector[0]) snprintf(st.sector_macro, sizeof(st.sector_macro), "%s", csector);
                // This client's ghost faction — computed LAZILY here (not at
                // JOIN, which arrives before the universe/faction list exist).
                // "" = not ready yet: skip the spawn this tick, retry next.
                NetClient* sender = find_client(from);
                char cfaction[80];
                client_faction_for(sender ? sender->id : 1, cfaction, sizeof(cfaction));
                // Determine the host's version of the client's sector. If the
                // client sent a sector macro and we know it, use it (so the ghost
                // is in the RIGHT sector). Otherwise fall back to the host's
                // player sector (old behaviour).
                UniverseID sector = 0;
                if (st.sector_macro[0] && g_host_sector_by_macro.count(st.sector_macro))
                    sector = g_host_sector_by_macro[st.sector_macro];
                if (!sector && g_game && g_game->GetPlayerZoneID && g_game->GetContextByClass)
                    sector = g_game->GetContextByClass(g_game->GetPlayerZoneID(), "sector", false);
                st.host_sector = sector;
                // Per-client sync stream: a sector change requires a fresh FULL
                // snapshot of the new sector (delta state is per-sector).
                if (sender && sector && sector != sender->cur_sector) {
                    sender->cur_sector = sector;
                    sender->needs_full = true;
                    sender->cur_sector_set_time = std::chrono::steady_clock::now();
                    sender->last_sent.clear();
                }
                UIPosRot pos; pos.x = x; pos.y = y; pos.z = z;
                pos.yaw = yaw; pos.pitch = pitch; pos.roll = roll;
                auto it = g_client_ships.find(key);
                if (it == g_client_ships.end()) {
                    if (g_game && g_game->SpawnObjectAtPos2 && sector && cfaction[0]) {
                        UniverseID ship = g_game->SpawnObjectAtPos2(st.macro, sector, pos, cfaction);
                        if (ship) {
                            g_client_ships[key] = ship;
                            g_host_only_objects.insert(ship); // never stream ghosts back
                        }
                        if (g_debug) { char dbg[280]; snprintf(dbg, sizeof(dbg), "x4mp: [DBG] HOST spawned client ghost id=%llu macro=%s sector=%s(%llu) faction=%s", (unsigned long long)ship, st.macro, st.sector_macro[0]?st.sector_macro:"?", (unsigned long long)sector, cfaction); net_debug(dbg); }
                    }
                } else {
                    if (g_game && g_game->SetObjectSectorPos && sector)
                        g_game->SetObjectSectorPos(it->second, sector, pos);
                }
                // LEGACY (X4MP_TELEPORT=1): force the game to fully simulate the
                // client's sector by moving the host player into it. Off by
                // default — the high-simulation set (ActivateObject on the
                // client's sector ships, see net_maintain_universe) achieves the
                // same without teleporting the server player around.
                if (g_teleport_enabled && sector && g_game && g_game->GetPlayerObjectID && 
                    g_game->GetContextByClass && g_game->SetObjectSectorPos) {
                    UniverseID host_player = g_game->GetPlayerObjectID();
                    UniverseID host_cur_sector = host_player ? g_game->GetContextByClass(host_player, "sector", false) : 0;
                    if (host_cur_sector != sector) {
                        // Rate-limit: moving the host player (a teleport) stresses the
                        // engine. If the client is flying fast through many sectors,
                        // teleporting every update can crash the game. Only move at
                        // most once per g_player_move_interval_ms.
                        auto now = std::chrono::steady_clock::now();
                        if (now - g_last_player_move > std::chrono::milliseconds(g_player_move_interval_ms)) {
                            g_last_player_move = now;
                            // Move the host player's SHIP directly (SetObjectSectorPos),
                            // NOT the camera (MovePlayerToSectorPos). The host player is
                            // docked, so MovePlayerToSectorPos only moves the camera and
                            // the ship stays put. Moving the ship makes the client's
                            // sector high-attention and lets the client see the host ship.
                            g_game->SetObjectSectorPos(host_player, sector, pos);
                            if (g_game->MovePlayerToSectorPos) g_game->MovePlayerToSectorPos(sector, pos);
                            if (g_debug) { char dbg[280]; snprintf(dbg, sizeof(dbg), "x4mp: [DBG] HOST moved player SHIP into client sector=%s(%llu) pos=(%.1f,%.1f,%.1f)", st.sector_macro[0]?st.sector_macro:"?", (unsigned long long)sector, x, y, z); net_debug(dbg); }
                        } else if (g_debug) {
                            char dbg[200]; snprintf(dbg, sizeof(dbg), "x4mp: [DBG] HOST player move rate-limited (client sector=%s)", st.sector_macro[0]?st.sector_macro:"?"); net_debug(dbg);
                        }
                    }
                }
                // Relay to all OTHER clients (via their stream port), including
                // this client's unique faction and sector macro so they can
                // render the ghost in the correct sector. Deferred (no relay)
                // while the faction list is unavailable.
                if (cfaction[0]) {
                    char relay[360];
                    int rn = snprintf(relay, sizeof(relay),
                                      "PLAYER %llu %.3f %.3f %.3f %.3f %.3f %.3f %s %s %s\n",
                                      (unsigned long long)key, x, y, z, yaw, pitch, roll, st.macro, cfaction,
                                      st.sector_macro[0] ? st.sector_macro : "?");
                    (void)rn;
                    for (auto& c : g_clients) {
                        if (c.addr.sin_port == from.sin_port && c.addr.sin_addr.s_addr == from.sin_addr.s_addr) continue;
                        net_send_tcp(c, relay, strlen(relay));
                    }
                }
            }
        }
    } else if (g_net_client) {
        // Any data from the host proves the link is alive.
        g_client_last_recv = std::chrono::steady_clock::now();
        if (strncmp(msg, "WELCOME", 7) == 0) {
            g_net_connected = true;
            net_log("x4mp: net: CONNECTED to host (handshake OK)");
        } else if (strncmp(msg, "PING", 4) == 0) {
            // Keepalive: answer so the host can confirm we are alive.
            net_send_client("PONG\n");
        } else if (strncmp(msg, "PLAYER", 6) == 0) {
            // Host relayed another client's ship: PLAYER <cid> <x> <y> <z> <yaw> <pitch> <roll> <macro>
            // Spawn/update a ghost of that remote client in OUR player sector so
            // we can see them. (We never receive our own relay, so no self-ghost.)
            unsigned long long cid; float x, y, z, yaw, pitch, roll; char macro[160], faction[80];
            if (sscanf(msg, "PLAYER %llu %f %f %f %f %f %f %159s %79s",
                       &cid, &x, &y, &z, &yaw, &pitch, &roll, macro, faction) == 9) {
                UniverseID player_zone = (g_game && g_game->GetPlayerZoneID) ? g_game->GetPlayerZoneID() : 0;
                UniverseID sector = (g_game && g_game->GetContextByClass && player_zone)
                    ? g_game->GetContextByClass(player_zone, "sector", false) : 0;
                UIPosRot pos; pos.x = x; pos.y = y; pos.z = z;
                pos.yaw = yaw; pos.pitch = pitch; pos.roll = roll;
                auto it = g_remote_ships.find(cid);
                if (it == g_remote_ships.end()) {
                    if (g_game && g_game->SpawnObjectAtPos2 && sector) {
                        UniverseID ship = g_game->SpawnObjectAtPos2(macro[0] ? macro : "?", sector, pos,
                                                                   faction[0] ? faction : "player");
                        if (ship) g_remote_ships[cid] = ship;
                    }
                } else {
                    if (g_game && g_game->SetObjectSectorPos && sector)
                        g_game->SetObjectSectorPos(it->second, sector, pos);
                }
            }
        } else if (strncmp(msg, "SNAP", 4) == 0) { // <-- was SNAP
            // Parse host snapshot: SNAP <id> <zone> <x> <y> <z> <yaw> <pitch> <roll> <tick>
            unsigned long long id, zone; float x, y, z, yaw, pitch, roll; unsigned tick;
            if (sscanf(msg, "SNAP %llu %llu %f %f %f %f %f %f %u",
                       &id, &zone, &x, &y, &z, &yaw, &pitch, &roll, &tick) == 9) {
                g_snap_id = id; g_snap_zone = zone;
                g_snap_x = x; g_snap_y = y; g_snap_z = z; g_snap_tick = tick;
                // Debug: continuous display of received host state.
                if (g_debug) {
                    char dbg[320];
                    snprintf(dbg, sizeof(dbg),
                             "x4mp: [DBG] CLIENT received host player id=%llu zone=%llu pos=(%.1f,%.1f,%.1f) rot=(%.1f,%.1f,%.1f) hosttick=%u",
                             id, zone, x, y, z, yaw, pitch, roll, tick);
                    net_debug(dbg);
                }
            }
        } else if (strncmp(msg, "OBJ", 3) == 0) {
            // Parse host object: OBJ <id> <zone> <sector> <x> <y> <z> <yaw> <pitch> <roll> <faction> <macro>
            unsigned long long id, zone, sector; float x, y, z, yaw, pitch, roll; char faction[80], macro[160];
            if (sscanf(msg, "OBJ %llu %llu %llu %f %f %f %f %f %f %79s %159s",
                       &id, &zone, &sector, &x, &y, &z, &yaw, &pitch, &roll, faction, macro) == 11) {
                std::lock_guard<std::mutex> lk(g_data_mutex);
                ClientObj& o = g_client_objs[id];
                o.id = id; o.zone = zone; o.sector = sector;
                o.x = x; o.y = y; o.z = z; o.yaw = yaw; o.pitch = pitch; o.roll = roll;
                snprintf(o.faction, sizeof(o.faction), "%s", faction);
                snprintf(o.macro, sizeof(o.macro), "%s", macro);
                if (g_debug) {
                    char dbg[380];
                    snprintf(dbg, sizeof(dbg),
                             "x4mp: [DBG] CLIENT got OBJ id=%llu zone=%llu sector=%llu pos=(%.1f,%.1f,%.1f) faction=%s macro=%s",
                             id, zone, sector, x, y, z, faction, macro);
                    net_debug(dbg);
                }
            }
        }
    }
}

static void net_poll() {
    // Consolidated TCP mode (X4MP_LEGACY_NET=0, tcp): accept new client
    // connections, then recv from each client's connection (control + data are
    // multiplexed on it). We create the NetClient on ACCEPT (the connection is
    // the identity); the JOIN line then just confirms it (sends WELCOME).
    if (!g_legacy_net && g_transport == "tcp") {
        if (g_net_host) {
        if (g_listen_sock >= 0) {
            for (;;) {
                struct sockaddr_in from; socklen_t fl = sizeof(from);
                int fd = accept(g_listen_sock, (struct sockaddr*)&from, &fl);
                if (fd < 0) break; // no more pending (EWOULDBLOCK) or error
                int one = 1;
                setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
                int fl2 = fcntl(fd, F_GETFL, 0);
                fcntl(fd, F_SETFL, fl2 | O_NONBLOCK);
                // A reconnect from the same peer replaces its stale entry
                // (same IP, new ephemeral port): close/erase any old entry for
                // this peer so g_clients does not grow unbounded.
                for (auto it = g_clients.begin(); it != g_clients.end(); ) {
                    if (it->addr.sin_addr.s_addr == from.sin_addr.s_addr) {
                        net_close_fd(it->tcp_fd, "accept_dedupe");
                        it = g_clients.erase(it);
                    } else {
                        ++it;
                    }
                }
                NetClient nc;
                nc.addr = from;
                nc.last_seen = std::chrono::steady_clock::now();
                nc.id = g_next_client_id++;
                nc.tcp_fd = fd;
                client_faction_for(nc.id, nc.faction, sizeof(nc.faction));
                g_clients.push_back(nc);
                char lbuf[192];
                snprintf(lbuf, sizeof(lbuf), "x4mp: net: client connected (id=%d, fd=%d, %s) faction=%s",
                         nc.id, fd, inet_ntoa(from.sin_addr), nc.faction);
                net_log(lbuf);
                net_dump_fds("accept");
            }
        }
        for (auto& c : g_clients) {
            if (c.tcp_fd < 0) continue;
            char buf[65536];
            ssize_t n = 0;
            while ((n = recv(c.tcp_fd, buf, sizeof(buf) - 1, 0)) > 0) {
                buf[n] = 0;
                c.tcp_recv_buf.append(buf, (size_t)n);
            }
            if (n == 0) {
                // Peer (client) closed gracefully (FIN). Log it so we know the
                // client initiated the drop.
                char m[160];
                snprintf(m, sizeof(m), "x4mp: net: HOST recv=0 (CLIENT closed) id=%d fd=%d", c.id, c.tcp_fd);
                net_log(m);
                net_close_fd(c.tcp_fd, "host_recv_eof"); c.tcp_fd = -1; c.tcp_recv_buf.clear();
                continue;
            }
            if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
                // Connection error: drop the fd.
                char m[200];
                snprintf(m, sizeof(m), "x4mp: net: HOST recv error id=%d fd=%d errno=%d (%s)", c.id, c.tcp_fd, errno, strerror(errno));
                net_log(m);
                net_close_fd(c.tcp_fd, "host_recv_error"); c.tcp_fd = -1; c.tcp_recv_buf.clear();
                continue;
            }
            size_t start = 0;
            for (;;) {
                size_t nl = c.tcp_recv_buf.find('\n', start);
                if (nl == std::string::npos) break;
                c.tcp_recv_buf[nl] = 0;
                if (nl > start) process_message(c.tcp_recv_buf.data() + start, c.addr);
                start = nl + 1;
            }
            if (start > 0) c.tcp_recv_buf.erase(0, start);
            if (c.tcp_recv_buf.size() > 4000000) c.tcp_recv_buf.clear();
        }
        } else if (g_net_client) {
            // Client: recv from the host over the TCP connection (g_sock).
            if (g_client_tcp_connecting) {
                int err = 0; socklen_t el = sizeof(err);
                if (getsockopt(g_sock, SOL_SOCKET, SO_ERROR, &err, &el) == 0 && err == 0) {
                    g_client_tcp_connecting = false;
                    net_log("x4mp: net: CLIENT TCP connected to host");
                } else {
                    return; // not connected yet; retry next tick
                }
            }
            char buf[65536];
            ssize_t n = 0;
            while ((n = recv(g_sock, buf, sizeof(buf) - 1, 0)) > 0) {
                buf[n] = 0;
                g_client_tcp_buf.append(buf, (size_t)n);
                g_client_last_recv = std::chrono::steady_clock::now();
            }
            if (n == 0) {
                // Peer (host) closed gracefully (FIN).
                net_log("x4mp: net: CLIENT recv=0 (HOST closed) — reconnecting");
                net_close_fd(g_sock, "client_recv_eof"); g_sock = -1; g_client_tcp_buf.clear();
                g_net_connected = false;
                return;
            }
            if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
                char m[200];
                snprintf(m, sizeof(m), "x4mp: net: CLIENT recv error errno=%d (%s) — reconnecting", errno, strerror(errno));
                net_log(m);
                net_close_fd(g_sock, "client_recv_error"); g_sock = -1; g_client_tcp_buf.clear();
                g_net_connected = false;
                return;
            }
            size_t start = 0;
            for (;;) {
                size_t nl = g_client_tcp_buf.find('\n', start);
                if (nl == std::string::npos) break;
                g_client_tcp_buf[nl] = 0;
                const char* line = g_client_tcp_buf.data() + start;
                // Data lines (OBJ / PLAYER relay / FULL) -> x4mp_stream
                // reconciliation. Control lines (WELCOME / PING) ->
                // process_message. FULL must reach x4mp_stream: it marks a
                // complete sector snapshot and gates convergence + pruning.
                if (strncmp(line, "OBJ", 3) == 0 || strncmp(line, "PLAYER", 6) == 0 ||
                    strncmp(line, "FULL", 4) == 0 || strncmp(line, "STA", 3) == 0 ||
                    strncmp(line, "KILL", 4) == 0 || strncmp(line, "PLAYERDIED", 10) == 0 ||
                    strncmp(line, "CARGO", 5) == 0 || strncmp(line, "CAPTURE", 7) == 0 ||
                    strncmp(line, "TRADE", 5) == 0) {
                    if (g_api && g_api->raise_event) g_api->raise_event("x4mp_stream.data", (void*)line);
                } else {
                    process_message(line, g_host_sa);
                }
                start = nl + 1;
            }
            if (start > 0) g_client_tcp_buf.erase(0, start);
            if (g_client_tcp_buf.size() > 4000000) g_client_tcp_buf.clear();
        }
        return;
    }
    if (g_sock < 0) return;
    char buf[65536];
    struct sockaddr_in from; socklen_t fromlen = sizeof(from);
    ssize_t n;
    while ((n = recvfrom(g_sock, buf, sizeof(buf) - 1, 0, (struct sockaddr*)&from, &fromlen)) > 0) {
        buf[n] = 0;
        // Process each line in the packet (messages may be batched).
        char* save = nullptr;
        char* line = strtok_r(buf, "\n", &save);
        while (line) {
            // Consolidated UDP mode: data lines (OBJ/PLAYER) go to x4mp_stream
            // (reconciliation); control lines (WELCOME/PING) to process_message.
            // Legacy: every line goes to process_message (data arrives on 7778).
            if (!g_legacy_net && g_transport == "udp" &&
                (strncmp(line, "OBJ", 3) == 0 || strncmp(line, "PLAYER", 6) == 0 ||
                 strncmp(line, "FULL", 4) == 0 || strncmp(line, "STA", 3) == 0 ||
                 strncmp(line, "KILL", 4) == 0 || strncmp(line, "PLAYERDIED", 10) == 0 ||
                 strncmp(line, "CARGO", 5) == 0 || strncmp(line, "CAPTURE", 7) == 0 ||
                 strncmp(line, "TRADE", 5) == 0)) {
                if (g_api && g_api->raise_event) g_api->raise_event("x4mp_stream.data", (void*)line);
            } else {
                process_message(line, from);
            }
            line = strtok_r(nullptr, "\n", &save);
        }
    }
}

// Called each frame: poll incoming data + send periodic keepalive/snapshot.
static void net_update() {
    net_poll();
    // New consolidated TCP mode: g_sock may be -1 (host: no UDP socket — it uses
    // g_listen_sock; client: connection dropped). Don't bail early — the periodic
    // send (host) and the reconnect logic (client) still need to run.
    bool have_sock = (g_sock >= 0) || (!g_legacy_net && g_transport == "tcp");
    if (!have_sock) return;
    g_net_tick++;
    if (g_net_host) {
        // Prune dead clients: remove any that have sent nothing for the timeout.
        if ((g_net_tick % 300) == 0 && !g_clients.empty()) {
            auto now = std::chrono::steady_clock::now();
            size_t before = g_clients.size();
            {
                char dbg[160];
                snprintf(dbg, sizeof(dbg), "x4mp: net: prune check (tick=%u): %zu entries",
                         (unsigned)g_net_tick, g_clients.size());
                net_log(dbg);
            }
            auto rit = std::remove_if(g_clients.begin(), g_clients.end(),
                [&](const NetClient& c) {
                    // Never prune a client that is still loading its save
                    // (it cannot send data while the game is busy loading).
                    if (c.loading) return false;
                    auto age = std::chrono::duration_cast<std::chrono::milliseconds>(
                                   now - c.last_seen).count();
                    return age > g_host_client_timeout_ms;
                });
            // Close the pruned clients' tcp_fd first (the destructor no longer
            // does — see ~NetClient note), then erase them from the vector.
            for (; rit != g_clients.end(); ++rit)
                if (rit->tcp_fd >= 0) { net_close_fd(rit->tcp_fd, "prune"); rit->tcp_fd = -1; }
            g_clients.erase(rit, g_clients.end());
            if (g_clients.size() != before) {
                char lbuf[128];
                snprintf(lbuf, sizeof(lbuf), "x4mp: net: pruned %zu dead client(s); %zu remain",
                         before - g_clients.size(), g_clients.size());
                net_log(lbuf);
            }
        }
        // DIAG: dump the fd table periodically so a silent fd death can be
        // bracketed between two dumps.
        if ((g_net_tick % 300) == 0) net_dump_fds("periodic");
        // Consolidated TCP: fd-less entries are dead connections (the client
        // reconnects via a fresh accept). Erase them so g_clients tracks real
        // connections only (prevents unbounded growth + wasted iteration).
        if (!g_legacy_net && g_transport == "tcp") {
            for (auto it = g_clients.begin(); it != g_clients.end(); ) {
                if (it->tcp_fd < 0) it = g_clients.erase(it);
                else ++it;
            }
        }
        // Debug: enumerate ships/stations across all factions.
        if (g_debug && (g_net_tick % 600) == 0) {
            if (g_game && g_game->GetAllFactions && g_game->GetAllFactionShips) {
                const char* factions[64];
                uint32_t nf = g_game->GetAllFactions(factions, 64, true);
                uint32_t total_ships = 0, total_stations = 0;
                for (uint32_t i = 0; i < nf; i++) {
                    total_ships += g_game->GetNumAllFactionShips ? g_game->GetNumAllFactionShips(factions[i]) : 0;
                    total_stations += g_game->GetNumAllFactionStations ? g_game->GetNumAllFactionStations(factions[i]) : 0;
                }
                char dbg[256];
                snprintf(dbg, sizeof(dbg), "x4mp: [DBG] HOST %u factions: %u ships, %u stations total",
                         nf, total_ships, total_stations);
                net_debug(dbg);
            }
        }
        // Every ~5s send a heartbeat to connected clients.
        if ((g_net_tick % 300) == 0 && !g_clients.empty()) {
            char buf[128];
            snprintf(buf, sizeof(buf), "PING %u\n", g_net_tick);
            net_send_ctrl_host(buf);
        }
        // Simulation maintenance (high-simulation set, per-sector activation,
        // sector macro map, stream cache): every ~5s while hosting, regardless
        // of whether the OBJ broadcast is enabled.
        if ((g_net_tick % 300) == 0) net_maintain_universe();
        // Host station replication (issue 3, symmetric to ACT BUILD): baseline
        // the save's PF stations once, then stream any NEW ones (host player
        // builds) to all clients as STA lines so they appear on the clients.
        if (g_universe_ready && g_game && g_game->GetAllFactionStations) {
            if (!g_host_station_baseline_done) {
                std::vector<UniverseID> sts;
                uint32_t ns = enumerate_faction_stations("player", sts);
                for (uint32_t i = 0; i < ns; i++) if (sts[i]) g_host_station_baseline.insert(sts[i]);
                g_host_station_baseline_done = true;
                if (g_debug) { char d[120]; snprintf(d, sizeof(d), "x4mp: [DBG] HOST station baseline: %zu", g_host_station_baseline.size()); net_debug(d); }
            }
            // Re-send every non-baseline station each cycle (like the client's
            // ACT BUILD resend): STA is idempotent on the client (it spawns
            // once), and keeping last_update fresh prevents the client's
            // 30 s stale-drop from removing it.
            if (g_net_tick % 200 == 0) {
                std::vector<UniverseID> sts;
                uint32_t ns = enumerate_faction_stations("player", sts);
                for (uint32_t i = 0; i < ns; i++) {
                    UniverseID s = sts[i];
                    if (!s || g_host_station_baseline.count(s)) continue;
                    char macro[160], smacro[160] = {0};
                    get_macro(s, macro, sizeof(macro));
                    UniverseID sec = g_game->GetContextByClass ? g_game->GetContextByClass(s, "sector", false) : 0;
                    if (sec) get_macro(sec, smacro, sizeof(smacro));
                    UIPosRot pos = g_game->GetObjectPositionInSector(s);
                    char b[420];
                    snprintf(b, sizeof(b), "STA %llu %s %.3f %.3f %.3f %.3f %.3f %.3f player %s\n",
                             (unsigned long long)s, smacro[0] ? smacro : "?",
                             pos.x, pos.y, pos.z, pos.yaw, pos.pitch, pos.roll, macro[0] ? macro : "?");
                    net_send_stream(b);
                    if (!g_host_station_sent.count(s)) {
                        g_host_station_sent.insert(s);
                        if (g_debug) { char d[280]; snprintf(d, sizeof(d), "x4mp: [DBG] HOST streaming new station id=%llu macro=%s sector=%s", (unsigned long long)s, macro, smacro[0] ? smacro : "?"); net_debug(d); }
                    }
                }
            }
        }
        // Maintain per-client TCP stream connections (connect / reconnect).
        // (Skipped for UDP — no TCP connection is used for the data stream; and
        // skipped in consolidated TCP mode — the client connects to OUR listener.)
        if (g_transport == "tcp" && g_legacy_net) {
            for (auto& c : g_clients) net_client_tcp(c);
        }
        // Rate-limit state streaming: only send SNAP/OBJ every g_update_interval
        // frames (default 15 Hz instead of every frame at 60fps). This cuts
        // bandwidth and CPU by ~4x with no perceptible loss for a thin client.
        if (!g_clients.empty() && (g_net_tick % g_update_interval) == 0) {
            // Layer 2: stream a universe-state snapshot to clients.
            net_send_snapshot_host();
            // Layer 2: stream ships.
            if (g_stream_ships) {
                // Legacy: full-universe broadcast to all clients (testing).
                if (g_objmode == "full") {
                    // FULL mode: enumerate + stream every frame (no cache).
                    net_send_objects_host_full();
                } else {
                    // CACHE mode: stream positions from the maintenance cache
                    // (with delta compression).
                    net_send_objects_host();
                }
            } else {
                // Default: per-client zone-limited sync stream. Each client
                // receives the COMPLETE ships in ITS current sector every tick
                // (full authoritative snapshot — no delta compression), which it
                // binds to its own locally simulated ships (reconciliation —
                // keeps all clients + host showing the same positions).
                for (auto& c : g_clients) net_send_client_objects(c);
            }
        }
    } else if (g_net_client) {
        // Consolidated TCP mode: if the connection dropped, reconnect (recreate
        // the TCP socket + connect). The host re-accepts us as a fresh client.
        if (!g_legacy_net && g_transport == "tcp" && g_sock < 0) {
            net_init_client(g_net_host_ip.c_str(), g_net_port);
            g_net_connected = false;
        }
        // Detect a dead link / host restart: if we were connected but have not
        // received any data from the host for the timeout, drop back to the
        // unconnected state so we re-send JOIN and re-establish the handshake.
        if (g_net_connected) {
            auto age = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::steady_clock::now() - g_client_last_recv).count();
            if (age > g_client_link_timeout_ms) {
                g_net_connected = false;
                net_log("x4mp: net: link timeout — reconnecting to host");
            }
        }
        // Send a JOIN until we're connected (re-sent after a link drop).
        if (!g_net_connected) {
            if ((g_net_tick % 60) == 0) net_send_client("JOIN x4mp\n");
        } else {
            // Tell the host our dedicated stream port once (x4mp_stream).
            if (!g_stream_reported) {
                char sbuf[32];
                snprintf(sbuf, sizeof(sbuf), "STREAM %d\n", g_stream_port);
                net_send_client(sbuf);
                g_stream_reported = true;
            }
            // While loading our save, tell the host so it won't prune us
            // (we can't send INPUT while the game is busy loading).
            if (g_save_loading && (g_net_tick % 60) == 0)
                net_send_client("LOADING\n");
            // Layer 2: send input/commands to the host (rate-limited to the
            // same update interval to reduce uplink traffic).
            if ((g_net_tick % g_update_interval) == 0) {
                char buf[128];
                snprintf(buf, sizeof(buf), "INPUT tick=%u\n", g_net_tick);
                net_send_client(buf);
                // Test action channel (X4MP_TEST_ACTION=1): send a periodic ACT
                // to validate the client->host action transport (the follow-up
                // player-action replication). The host logs every ACT it receives.
                if (g_test_action && g_client_ready && (g_net_tick % 300) == 0) {
                    char abuf[64];
                    snprintf(abuf, sizeof(abuf), "ACT test n=%u\n", g_net_tick);
                    net_send_client(abuf);
                }
                // Send our ship state so the host (and other clients via relay)
                // can see us. Only once we are in the universe with a valid ship.
                if (g_client_ready && g_game && g_game->GetPlayerObjectID &&
                    g_game->GetObjectPositionInSector) {
                    UniverseID ship = g_game->GetPlayerObjectID();
                    if (ship) {
                        UIPosRot pos = g_game->GetObjectPositionInSector(ship);
                        char macro[160]; get_macro(ship, macro, sizeof(macro));
                        // Get our sector macro so the host can place our ghost in
                        // the correct sector and move its player here (to force
                        // simulation of this sector on the host).
                        char csector[160] = {0};
                        UniverseID player_zone = g_game->GetPlayerZoneID ? g_game->GetPlayerZoneID() : 0;
                        UniverseID csec = (g_game->GetContextByClass && player_zone)
                            ? g_game->GetContextByClass(player_zone, "sector", false) : 0;
                        if (csec) get_macro(csec, csector, sizeof(csector));
                        char pbuf[320];
                        snprintf(pbuf, sizeof(pbuf), "PLAYER %.3f %.3f %.3f %.3f %.3f %.3f %s %s\n",
                                 pos.x, pos.y, pos.z, pos.yaw, pos.pitch, pos.roll,
                                 macro[0] ? macro : "?", csector[0] ? csector : "?");
                        net_send_client(pbuf);
                        if (g_debug) {
                            char dbg[340];
                            snprintf(dbg, sizeof(dbg), "x4mp: [DBG] CLIENT sending PLAYER ship=%llu pos=(%.1f,%.1f,%.1f) macro=%s sector=%s",
                                     (unsigned long long)ship, pos.x, pos.y, pos.z, macro[0] ? macro : "?", csector[0] ? csector : "?");
                            net_debug(dbg);
                        }
                    }
                }
                // Issue 4: retry the fleet reassignment until the player-ship
                // APIs report the own ship (they are not ready at universe_ready
                // time; reassigning before then would make the player ship
                // alien-owned and unboardable).
                if (!g_fleet_reassigned && g_net_tick % 100 == 0) {
                    g_fleet_reassigned = client_reassign_player_fleet();
                    if (!g_fleet_reassigned && g_debug) {
                        UniverseID pobj = g_game->GetPlayerObjectID ? g_game->GetPlayerObjectID() : 0;
                        const char* owner = "?";
                        if (pobj && g_game->GetOwnerDetails2) {
                            auto od = g_game->GetOwnerDetails2(pobj);
                            if (od.factionID) owner = od.factionID;
                        }
                        char dbg[220];
                        snprintf(dbg, sizeof(dbg), "x4mp: [DBG] CLIENT fleet-reassign retry (tick=%u): player_obj=%llu owner=%s — not identified yet",
                                 g_net_tick, (unsigned long long)pobj, owner);
                        net_debug(dbg);
                    }
                }
                // Issue 3: report stations this client built after load so the
                // host can spawn ghosts of them. Scanning every ~10 s is plenty
                // (station builds are rare and slow); the seen-set is baselined
                // at ready time with the save's own stations.
                if (g_client_ready && g_game && g_game->GetAllFactionStations &&
                    g_net_tick % 200 == 0) {
                    std::vector<UniverseID> sts;
                    uint32_t ns = enumerate_faction_stations("player", sts);
                    // Re-send EVERY non-baseline station each cycle with a
                    // stable seq: the host dedupes via its spawned[seq] table,
                    // so this is idempotent and recovers from any deferred/
                    // dropped ACT BUILD. Baseline stations (from the save) are
                    // never reported — the host already has them.
                    for (uint32_t i = 0; i < ns; i++) {
                        UniverseID s = sts[i];
                        if (!s || g_client_station_baseline.count(s)) continue;
                        auto sit = g_client_station_seq_map.find(s);
                        bool first = (sit == g_client_station_seq_map.end());
                        uint32_t seq = first ? g_client_station_seq++ : sit->second;
                        if (first) g_client_station_seq_map[s] = seq;
                        char macro[160], csector[160] = {0};
                        get_macro(s, macro, sizeof(macro));
                        UIPosRot pos = g_game->GetObjectPositionInSector(s);
                        UniverseID sec = g_game->GetContextByClass ? g_game->GetContextByClass(s, "sector", false) : 0;
                        if (sec) get_macro(sec, csector, sizeof(csector));
                        char b[420];
                        snprintf(b, sizeof(b), "ACT BUILD %u %s %.3f %.3f %.3f %.3f %.3f %.3f %s\n",
                                 seq, macro[0] ? macro : "?", pos.x, pos.y, pos.z,
                                 pos.yaw, pos.pitch, pos.roll, csector[0] ? csector : "?");
                        net_send_client(b);
                        if (first) {
                            char dbg[300];
                            snprintf(dbg, sizeof(dbg), "x4mp: [DBG] CLIENT reporting new station seq=%u id=%llu macro=%s sector=%s",
                                     seq, (unsigned long long)s, macro, csector[0] ? csector : "?");
                            net_debug(dbg);
                        }
                    }
                }
            }
        }
    }
}

// The game takes ~14s to reach the start menu ("Game initialisation time until
// intro video"). Connecting before that fails with "Failed to initialize the
// network engine". Wait this long before the first join attempt.
static const int64_t   g_join_delay_ms = 20000;

static void read_config() {
    const char* auto_mode = std::getenv("X4MP_AUTO");
    if (auto_mode) g_auto = auto_mode;
    const char* ta = std::getenv("X4MP_TEST_ACTION");
    if (ta && *ta == '1') g_test_action = true;
    const char* tm = std::getenv("X4MP_TEST_MENU");
    if (tm && *tm) {
        g_test_menu = true;
        g_test_menu_role = tm;
        g_test_menu_time = std::chrono::steady_clock::now();
    }
    const char* pf = std::getenv("X4MP_PERF_LOG");
    if (pf && *pf) g_perf_log_path = pf;
    g_perf_log_time = std::chrono::steady_clock::now();
    const char* ip = std::getenv("X4MP_SERVER_IP");
    if (ip) g_server_ip = ip;
    const char* mod = std::getenv("X4MP_MODULE");
    if (mod) g_module = mod;
    const char* diff = std::getenv("X4MP_DIFFICULTY");
    if (diff) g_difficulty = diff;
    const char* save = std::getenv("X4MP_SAVE");
    if (save) g_save = save;
    const char* port = std::getenv("X4MP_PORT");
    if (port && *port) g_net_port = (uint16_t)atoi(port);
    if (ip) g_net_host_ip = ip;
    const char* sp = std::getenv("X4MP_STREAM_PORT");
    if (sp) { int v = atoi(sp); if (v > 0 && v < 65535) g_stream_port = v; }
    const char* tr = std::getenv("X4MP_TRANSPORT");
    if (tr && *tr) {
        std::string t = tr;
        if (t == "udp" || t == "UDP") g_transport = "udp";
        else g_transport = "tcp";
    }
    const char* ln = std::getenv("X4MP_LEGACY_NET");
    if (ln) g_legacy_net = (*ln != '0');   // default OFF (consolidated); X4MP_LEGACY_NET=1 restores the legacy split-port mode
    const char* rel = std::getenv("X4MP_RELEVANCE_M");
    if (rel) { float v = (float)atof(rel); if (v >= 1000.0f && v <= 1000000.0f) g_relevance_m = v; }
    const char* dbg = std::getenv("X4MP_DEBUG");
    if (dbg && *dbg == '1') g_debug = true;
    const char* om = std::getenv("X4MP_OBJMODE");
    if (om && *om) g_objmode = om;
    const char* cl = std::getenv("X4MP_CLEANUP");
    if (cl && *cl == '1') g_cleanup_enabled = true;  // default off: keep own ships
    const char* st = std::getenv("X4MP_STREAMS");
    if (st && *st) {
        int n = atoi(st);
        if (n >= 0 && n <= 8) g_num_streams = n;
    }
    const char* ps = std::getenv("X4MP_PAUSE");
    if (ps && *ps == '1') g_pause_enabled = true;
    const char* fs = std::getenv("X4MP_FULLSIM");
    if (fs && *fs == '1') g_fullsim = true;   // default off: high-sim set only
    const char* tp = std::getenv("X4MP_TELEPORT");
    if (tp && *tp == '1') g_teleport_enabled = true;
    const char* ss = std::getenv("X4MP_STREAMSHIPS");
    if (ss && *ss == '1') g_stream_ships = true;
    const char* ut = std::getenv("X4MP_UNIVERSE_TIMEOUT");
    if (ut && *ut) {
        long long v = atoll(ut);
        if (v >= 0) g_universe_timeout_ms = v * 1000; // seconds -> ms (0 = strict wait)
    }
    const char* hto = std::getenv("X4MP_HOST_TIMEOUT");
    if (hto && *hto) {
        long long v = atoll(hto);
        if (v >= 0) g_host_client_timeout_ms = v * 1000;
    }
    const char* cto = std::getenv("X4MP_CLIENT_TIMEOUT");
    if (cto && *cto) {
        long long v = atoll(cto);
        if (v >= 0) g_client_link_timeout_ms = v * 1000;
    }
    const char* uhz = std::getenv("X4MP_UPDATE_HZ");
    if (uhz && *uhz) {
        int hz = atoi(uhz);
        if (hz >= 1 && hz <= 60) g_update_interval = 60 / hz; // 60fps / target hz
    }
    const char* dm = std::getenv("X4MP_DELTA_M");
    if (dm && *dm) {
        float v = (float)atof(dm);
        if (v >= 0) g_delta_m = v;
    }
}

// Start the data-stream threads (separate ports 7778+). Host streams categories;
// client receives them off the main thread.
static void start_streams() {
    if (g_streams_running) return;
    g_streams_running = true;
    if (g_auto == "host") {
        for (int i = 0; i < g_num_streams; i++) {
            g_stream_threads.emplace_back(host_stream_thread, (int)g_net_port + 1 + i, (i % 2) == 0);
            g_stream_threads.back().detach();
        }
        char lbuf[128];
        snprintf(lbuf, sizeof(lbuf), "x4mp: net: HOST started %d data-stream threads (ports %u-%u)",
                 g_num_streams, (unsigned)(g_net_port + 1), (unsigned)(g_net_port + g_num_streams));
        net_log(lbuf);
    } else if (g_auto == "client") {
        for (int i = 0; i < g_num_streams; i++) {
            g_stream_threads.emplace_back(client_receive_thread, (int)g_net_port + 1 + i);
            g_stream_threads.back().detach();
        }
        char lbuf[128];
        snprintf(lbuf, sizeof(lbuf), "x4mp: net: CLIENT started %d data-receive threads (ports %u-%u)",
                 g_num_streams, (unsigned)(g_net_port + 1), (unsigned)(g_net_port + g_num_streams));
        net_log(lbuf);
    }
}

// Start our custom network based on role (called once the role is known).
static void net_start() {
    if (g_auto == "host" && !g_net_host) {
        net_init_host(g_net_port);
        // Consolidated UDP mode: the data rides the control socket (g_sock, 7777),
        // so no separate data socket is needed. Legacy UDP mode: create one (7778).
        if (g_transport == "udp" && g_stream_udp_sock < 0 && g_legacy_net) {
            g_stream_udp_sock = socket(AF_INET, SOCK_DGRAM, 0);
            if (g_stream_udp_sock >= 0) {
                int fl = fcntl(g_stream_udp_sock, F_GETFL, 0);
                fcntl(g_stream_udp_sock, F_SETFL, fl | O_NONBLOCK);
                char m[160];
                snprintf(m, sizeof(m), "x4mp: net: HOST data stream using UDP (sendto clients' port %d)", g_stream_port);
                net_log(m);
            } else {
                char m[128];
                snprintf(m, sizeof(m), "x4mp: net: WARNING — could not create UDP data socket; falling back to TCP");
                net_log(m);
                g_transport = "tcp";
            }
        } else if (g_transport == "tcp") {
            char m[160];
            snprintf(m, sizeof(m), "x4mp: net: HOST data stream using TCP");
            net_log(m);
        }
    } else if (g_auto == "client" && !g_net_client) {
        net_init_client(g_net_host_ip.c_str(), g_net_port);
    }
}

// ---- Actions --------------------------------------------------------------

static void do_host(const char* module, const char* difficulty) {
    if (g_host_done) {
        g_api->log(X4NATIVE_LOG_WARN, "x4mp: host already started — ignoring duplicate request");
        return;
    }
    g_host_done = true;
    // Start the data-stream threads only AFTER the game is loaded (creating
    // them during init interferes with the game's startup and causes shutdown).
    start_streams();
    const char* m = (module && *module) ? module : g_module.c_str();
    const char* d = (difficulty && *difficulty) ? difficulty : g_difficulty.c_str();
    g_api->log(X4NATIVE_LOG_INFO, "x4mp: HOST — NewMultiplayerGame");
    if (g_game && g_game->NewMultiplayerGame) {
        g_game->NewMultiplayerGame(m, d);
        g_api->log(X4NATIVE_LOG_INFO, "x4mp: NewMultiplayerGame invoked; RakNet host socket active");
    } else {
        g_api->log(X4NATIVE_LOG_ERROR, "x4mp: NewMultiplayerGame not available!");
    }
}

// Raw connect call (no duplicate-guard) so we can retry. Calling
// ConnectToMultiplayerGame repeatedly is safe — it just keeps trying to reach
// the host; the game transitions into the universe once a connection succeeds.
static void connect_raw(const char* server_ip) {
    const char* ip = (server_ip && *server_ip) ? server_ip : g_server_ip.c_str();
    g_api->log(X4NATIVE_LOG_INFO, "x4mp: CLIENT — ConnectToMultiplayerGame");
    if (g_game && g_game->ConnectToMultiplayerGame) {
        g_game->ConnectToMultiplayerGame(ip);
        g_api->log(X4NATIVE_LOG_INFO, "x4mp: ConnectToMultiplayerGame invoked; joining");
    } else {
        g_api->log(X4NATIVE_LOG_ERROR, "x4mp: ConnectToMultiplayerGame not available!");
    }
}

static void do_join(const char* server_ip) {
    if (g_join_done) {
        g_api->log(X4NATIVE_LOG_WARN, "x4mp: join already attempted — ignoring duplicate request");
        return;
    }
    g_join_done = true;
    connect_raw(server_ip);
}

// ---- Event callbacks ------------------------------------------------------

// In-game menu "Host Multiplayer" (from the main menu). This drives OUR
// custom networking (not X4's built-in EgoNet multiplayer). We set the role
// at runtime, start our custom transport now, and load the designated save
// (X4MP_SAVE) if one was prepared by the launcher. The existing frame-update
// / on_game_loaded logic then finishes the job (load save -> do_host ->
// start_streams).
// Combat (client): x4mp_stream detected the player's local kills / death and
// raises this with the ACT line to send. Forward it over the control channel
// (net_send_client). Runs on the main thread (raised from x4mp_stream's
// on_frame_update), so the UDP sendto is safe.
static void on_send_act(const char* /*event_name*/, void* data, void* /*userdata*/) {
    if (g_net_client && data) net_send_client((const char*)data);
}

static void on_host_request(const char* /*event_name*/, void* data, void* /*userdata*/) {
    const char* module = (data && *(const char*)data) ? (const char*)data : nullptr;
    g_api->log(X4NATIVE_LOG_INFO, "x4mp: HOST requested (in-game menu)");
    g_auto = "host";                       // set role at runtime (menu-driven)
    if (module && *module) g_module = module;
    net_start();                           // start our custom network (UDP 7777) now
    if (!g_save.empty()) {
        g_save_pending = true;             // load the save first; do_host on game loaded
        char buf[256];
        snprintf(buf, sizeof(buf), "x4mp: menu host — will load save '%s' then start streaming",
                 g_save.c_str());
        g_api->log(X4NATIVE_LOG_INFO, buf);
    } else {
        g_host_pending = true;             // no save: host a new game on next frame
        g_api->log(X4NATIVE_LOG_INFO, "x4mp: menu host — no save, hosting a new game");
    }
}

// In-game menu "Join Multiplayer" (from the main menu, arg = host IP). Sets the
// role at runtime, connects our custom transport to the host, and loads the
// designated save (X4MP_SAVE). The existing client logic (load save -> universe
// ready -> client_mark_ready -> start_streams) finishes the job. We do NOT call
// do_join() (ConnectToMultiplayerGame) — our custom net (net_start) is what
// actually connects.
static void on_join_request(const char* /*event_name*/, void* data, void* /*userdata*/) {
    const char* ip = (data && *(const char*)data) ? (const char*)data : nullptr;
    g_api->log(X4NATIVE_LOG_INFO, "x4mp: JOIN requested (in-game menu)");
    g_auto = "client";                     // set role at runtime (menu-driven)
    if (ip && *ip) { g_server_ip = ip; g_net_host_ip = ip; }
    net_start();                           // connect our custom network (UDP 7777) now
    if (!g_save.empty()) {
        g_save_pending = true;             // load the same save; client logic connects
        char buf[256];
        snprintf(buf, sizeof(buf), "x4mp: menu join — will load save '%s' then connect to %s",
                 g_save.c_str(), g_net_host_ip.c_str());
        g_api->log(X4NATIVE_LOG_INFO, buf);
    } else {
        g_newgame_pending = true;          // no save: thin client, new game
        g_api->log(X4NATIVE_LOG_INFO, "x4mp: menu join — no save, thin client new game");
    }
}

// Forward decl: client_mark_ready() is defined below (after on_frame_update).
static void client_mark_ready(const char* source);

static void on_frame_update(const char* /*event_name*/, void* data, void* /*userdata*/) {
    (void)data;
    // Drive our custom network every frame (measured for PERF logging).
    {
        auto tu0 = std::chrono::steady_clock::now();
        net_update();
        auto tu1 = std::chrono::steady_clock::now();
        g_perf_net_us += std::chrono::duration_cast<std::chrono::microseconds>(tu1 - tu0).count();
        g_perf_net_count++;
    }
    // PERF: log FPS + per-tick net_update cost every 2s to a file.
    g_perf_frames++;
    {
        auto now = std::chrono::steady_clock::now();
        long long elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - g_perf_log_time).count();
        if (elapsed_ms >= 2000 && g_perf_frames > 0) {
            double fps = g_perf_frames * 1000.0 / (double)elapsed_ms;
            long long avg_net_us = g_perf_net_count ? (g_perf_net_us / g_perf_net_count) : 0;
            g_perf_frames = 0; g_perf_net_us = 0; g_perf_net_count = 0;
            g_perf_log_time = now;
            FILE* pf = fopen(g_perf_log_path.c_str(), "a");
            if (pf) {
                char pline[256];
                snprintf(pline, sizeof(pline),
                         "role=%s fps=%.1f net_update_avg_us=%lld clients=%zu\n",
                         g_auto.c_str(), fps, avg_net_us, g_clients.size());
                fputs(pline, pf);
                fclose(pf);
            }
        }
    }
    // Test hook: simulate the in-game menu click after a delay (so the
    // menu-driven host/join flow can be verified without clicking).
    if (g_test_menu && !g_test_menu_done) {
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::steady_clock::now() - g_test_menu_time).count();
        if (elapsed >= 25000) {
            g_test_menu_done = true;
            char lbuf[160];
            snprintf(lbuf, sizeof(lbuf), "x4mp: TEST MENU — simulating '%s' click (menu-driven flow)",
                     g_test_menu_role.c_str());
            g_api->log(X4NATIVE_LOG_INFO, lbuf);
            if (g_test_menu_role == "host") {
                on_host_request(nullptr, nullptr, nullptr);
            } else {
                const char* ip = g_server_ip.c_str();
                on_join_request(nullptr, (void*)ip, nullptr);
            }
        }
    }
    // THIN CLIENT: render the host's streamed state. When paused (thin client),
    // move the player to the host's position. When NOT paused, the user flies
    // the client's ship, so we don't override their position.
    if (g_auto == "client") {
        // FALLBACK: if the client's 2nd load pass is stuck/slow (resource
        // contention on a shared machine), on_universe_ready may never fire and
        // the client would hang forever. Once game_loaded has fired the client's
        // player + sector context are valid, so after a generous timeout we mark
        // ready anyway. Set X4MP_UNIVERSE_TIMEOUT=0 to disable (strict wait).
        if (!g_client_ready && g_game_loaded_fired && g_universe_timeout_ms > 0) {
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                               std::chrono::steady_clock::now() - g_game_loaded_time)
                               .count();
            if (elapsed >= g_universe_timeout_ms) {
                g_api->log(X4NATIVE_LOG_WARN,
                           "x4mp: universe_ready timeout — enabling client render via fallback");
                client_mark_ready("timeout");
            }
        }
        if (g_pause_enabled) thin_client_render();
        // Object rendering is handled by the dedicated x4mp_stream extension
        // (smoothing + dead-object pruning). x4mp.so intentionally does NOT call
        // thin_client_render_objects() here — doing so would spawn a SECOND copy
        // of every host object (double-render / duplicate ships under the real
        // ones). x4mp.so keeps only the control channel (SNAP camera, PLAYER
        // relay, INPUT) and the own-object cleanup below.
        //
        // OPTION 2: remove the client's own save objects shortly after the save
        // has loaded, so the client renders ONLY host-streamed objects. Enabled
        // by default for the thin client (no more duplicates under player/NPC
        // ships). Disable with X4MP_CLEANUP=0 if you want to keep the client's
        // own universe visible.
        if (g_cleanup_pending && !g_cleanup_done && g_cleanup_enabled) {
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                               std::chrono::steady_clock::now() - g_ready_time)
                               .count();
            if (elapsed >= 5000) {
                g_cleanup_pending = false;
                thin_client_cleanup_own_objects();
            }
        }
    }

    // THIN CLIENT: wait for the game to reach the main menu, then start a NEW
    // game (minimal universe for rendering context — we do NOT load a save).
    if (g_auto == "client" && g_newgame_pending && !g_newgame_done) {
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::steady_clock::now() - g_start)
                           .count();
        if (elapsed >= g_join_delay_ms) {
            g_newgame_pending = false;
            g_newgame_done = true;
            if (g_game && g_game->NewGame) {
                g_game->NewGame(g_module.c_str(), 0, nullptr);
                g_api->log(X4NATIVE_LOG_INFO, "x4mp: thin client — starting new game");
            } else {
                g_api->log(X4NATIVE_LOG_ERROR, "x4mp: NewGame not available!");
            }
        }
    }
    // Host on the next frame after a host request. We do NOT require being
    // in-game (off the start menu) — NewMultiplayerGame is designed to be
    // called from the start menu context.
    if (g_host_pending && !g_host_done) {
        g_host_pending = false;
        do_host(nullptr, nullptr);
        return;
    }
    // Save-loading host flow: wait until the game's save list is loaded, then
    // raise the game's "loadSave" Lua event (which calls LoadGame). Hosting is
    // deferred until on_game_loaded fires (see on_game_loaded).
    if (g_save_pending && !g_save_loading && !g_host_done) {
        bool list_ready = (g_game && g_game->IsSaveListLoadingComplete)
                               ? g_game->IsSaveListLoadingComplete()
                               : false;
        if (list_ready) {
            g_save_pending = false;
            g_save_loading = true;
            char buf[512];
            snprintf(buf, sizeof(buf), "x4mp: save list ready — loading save '%s'", g_save.c_str());
            g_api->log(X4NATIVE_LOG_INFO, buf);
            if (g_api->raise_lua_event)
                g_api->raise_lua_event("loadSave", g_save.c_str());
            else
                g_api->log(X4NATIVE_LOG_ERROR, "x4mp: raise_lua_event unavailable — cannot load save");
        }
    }

    // Fresh-new-game auto-host (no save). If a save is being loaded, hosting
    // is deferred to on_game_loaded.
    if (g_auto == "host" && !g_host_done && g_save.empty()) {
        do_host(nullptr, nullptr);
        return;
    }
    if ((++g_heartbeat % 600) == 0) {
        if (g_auto == "client")
            g_api->log(X4NATIVE_LOG_INFO,
                       g_client_ready ? "x4mp: heartbeat — CLIENT in universe"
                                      : "x4mp: heartbeat — CLIENT loading save");
        else if (g_host_done)
            g_api->log(X4NATIVE_LOG_INFO, "x4mp: heartbeat — HOST active");
    }
}

static void on_game_loaded(const char* /*event_name*/, void* /*data*/, void* /*userdata*/) {
    g_api->log(X4NATIVE_LOG_INFO, "x4mp: game loaded");
    // CLIENT: the save's FIRST load pass is done (entity IDs valid), but the
    // universe is NOT fully built yet — X4 loads saves in two passes. We must
    // NOT start spawning/rendering host objects now, because the game's
    // background "Movement worker" thread is still rebuilding the universe and
    // our main-thread calls into the object system race with it -> SIGSEGV in
    // the worker (heap corruption in a std::vector realloc).
    //
    // So for the client we only clear the save-loading flag here and wait for
    // on_universe_ready (fires after the 2nd pass, on event_universe_generated)
    // before marking the client ready. See on_universe_ready().
    if (g_auto == "client") {
        g_save_loading = false;
        g_game_loaded_fired = true;
        g_game_loaded_time = std::chrono::steady_clock::now();
        g_api->log(X4NATIVE_LOG_INFO, "x4mp: client save loaded — waiting for universe ready (2nd pass)");
        return;
    }
    // If we were loading a save, the save has now finished loading -> host it.
    if (g_save_loading) {
        g_save_loading = false;
        g_api->log(X4NATIVE_LOG_INFO, "x4mp: save loaded — hosting now");
        do_host(nullptr, nullptr);
        return;
    }
    // Fresh-new-game auto-host (no save).
    if (g_auto == "host" && !g_host_done && g_save.empty()) do_host(nullptr, nullptr);
}

// CLIENT (issue 4): in a shared save, the player faction ("player") owns the
// HOST player's fleet. Loaded locally, those ships render on the client as the
// client's OWN ships — the client "sees the host's ships as its own". Reassign
// every player-faction ship (except the ship the client itself pilots) to the
// host-display faction so the host's fleet renders as a foreign faction.
// The client's own piloted ship keeps the vanilla player faction so the client
// player recognises it (and can still board it — an alien-owned ship is
// unboardable).
//
// Returns true once done. Returns false if the player-ship APIs are not ready
// yet (all sources return 0 right after universe_ready — retrying is required,
// otherwise the player's OWN ship would be reassigned and become unboardable).
static bool client_reassign_player_fleet() {
    if (!g_game || !g_game->GetAllFactions || !g_game->GetAllFactionShips ||
        !g_game->SetComponentOwner) return false;
    ensure_real_factions();
    if (g_real_factions.empty()) return false; // nothing safe to reassign to
    // Identify the client's own player ship(s) to exclude (multi-source: a
    // docked player returns 0 from GetPlayerControlledShipID).
    UniverseID keep[5] = {0,0,0,0,0};
    keep[0] = g_game->GetPlayerControlledShipID ? g_game->GetPlayerControlledShipID() : 0;
    keep[1] = g_game->GetPlayerShipID ? g_game->GetPlayerShipID() : 0;
    keep[2] = g_game->GetPlayerOccupiedShipID ? g_game->GetPlayerOccupiedShipID() : 0;
    keep[3] = g_game->GetPlayerObjectID ? g_game->GetPlayerObjectID() : 0;
    keep[4] = (keep[3] && g_game->GetContextByClass) ? g_game->GetContextByClass(keep[3], "ship", false) : 0;
    // Campaign saves: the player pilots a FOREIGN-faction ship (e.g. the
    // Boron1 "alliance" ship). Then every PF ship is the player's OWN ship —
    // there is nothing of the host's to hide, so we are done (do not retry).
    if (keep[3] && g_game->GetOwnerDetails2) {
        auto od = g_game->GetOwnerDetails2(keep[3]);
        if (od.factionID && strcmp(od.factionID, "player") != 0) {
            if (g_debug) {
                char dbg[200];
                snprintf(dbg, sizeof(dbg), "x4mp: [DBG] CLIENT fleet reassign skipped: player pilots foreign faction '%s' (PF ships are the player's own)", od.factionID);
                net_debug(dbg);
            }
            return true;
        }
    }
    // Only reassign ships owned by the vanilla player faction.
    std::vector<UniverseID> pfships;
    uint32_t n = enumerate_faction_ships("player", pfships);
    // Safety: only proceed once the player's OWN ship is positively identified
    // as a member of the player-faction ship set. An ambiguous non-zero
    // GetPlayerObjectID (e.g. a station) must never be taken as "identified",
    // or the player's real ship would get reassigned and become unboardable.
    // Identify which of the keep IDs is actually a player-faction ship.
    // IMPORTANT: this must stay strict. In a campaign save the piloted ship is
    // a FOREIGN faction (e.g. "alliance") and none of the keep IDs is in the
    // PF set — then we must NOT reassign anything: the PF ships there are the
    // player's OWN ships (e.g. their docked scout), not the host's fleet.
    bool identified = false;
    for (int i = 0; i < 5; i++) {
        if (!keep[i]) continue;
        for (uint32_t j = 0; j < n; j++) if (pfships[j] == keep[i]) { identified = true; break; }
        if (identified) break;
    }
    if (!identified) return false; // nothing safe to exclude — retry/never
    const char* fhost = g_real_factions[0].c_str();
    auto is_keep = [&](UniverseID s) {
        for (int i = 0; i < 5; i++) if (keep[i] && keep[i] == s) return true;
        return false;
    };
    uint32_t reassigned = 0;
    for (uint32_t i = 0; i < n; i++) {
        UniverseID s = pfships[i];
        if (!s || is_keep(s)) continue;
        g_game->SetComponentOwner(s, fhost);
        reassigned++;
    }
    char dbg[260];
    snprintf(dbg, sizeof(dbg), "x4mp: [DBG] CLIENT reassigned %u/%u player-faction ships -> %s (keep=[%llu %llu %llu %llu %llu])",
             reassigned, n, fhost,
             (unsigned long long)keep[0], (unsigned long long)keep[1], (unsigned long long)keep[2],
             (unsigned long long)keep[3], (unsigned long long)keep[4]);
    net_debug(dbg);
    return true;
}

// Mark the thin client ready to render host-streamed objects. This is the ONLY
// place g_client_ready is set, so object spawning never starts before the game
// is in a usable state.
//   source == "universe_ready" : the 2nd load pass fully completed (safe path).
//   source == "timeout"        : fallback — the 2nd pass is stuck/slow on a
//                                resource-constrained client. The client's player
//                                and sector context are already valid (game_loaded
//                                fired), so we render anyway. The game is not
//                                actively rebuilding (main thread blocked on I/O),
//                                so spawning does not race the Movement worker.
static void client_mark_ready(const char* source) {
    if (g_client_ready) return;
    g_client_ready = true;
    char lbuf[192];
    snprintf(lbuf, sizeof(lbuf), "x4mp: client in universe — ready to render host state (via %s)", source);
    g_api->log(X4NATIVE_LOG_INFO, lbuf);
    // Reassign the host's player-faction fleet so it renders as foreign, not
    // as the client's own ships (issue 4). The player-ship APIs may not be
    // ready this early — on failure, retry every ~5 s from net_update.
    g_fleet_reassigned = client_reassign_player_fleet();
    // Baseline the save's player-faction stations (issue 3): they already
    // exist on the host (same save) and must never be reported (duplicates).
    // Only stations built AFTER load are new client builds to replicate.
    g_client_station_baseline.clear();
    if (g_game && g_game->GetAllFactionStations) {
        std::vector<UniverseID> sts;
        uint32_t ns = enumerate_faction_stations("player", sts);
        for (uint32_t i = 0; i < ns; i++) if (sts[i]) g_client_station_baseline.insert(sts[i]);
    }
    // Tell the host we finished loading so it resumes normal liveness pruning.
    net_send_client("READY\n");
    // Start the data-receive threads only after we are ready to render.
    start_streams();
    // OPTION 2: after a short delay remove the client's own save objects so
    // it renders ONLY host-streamed objects.
    g_cleanup_pending = true;
    g_ready_time = std::chrono::steady_clock::now();
    // THIN CLIENT: optionally pause the local simulation (the host is
    // authoritative) to offload universe calculation. OFF by default so the
    // client stays responsive and the user can fly. Enable with X4MP_PAUSE=1.
    if (g_pause_enabled) {
        if (g_api->raise_lua_event)
            g_api->raise_lua_event("x4mp.pause", NULL);
        else
            g_api->log(X4NATIVE_LOG_WARN, "x4mp: raise_lua_event unavailable — cannot pause client");
    }
    // Debug: probe which Lua API functions are available for object streaming.
    if (g_api->raise_lua_event)
        g_api->raise_lua_event("x4mp.debug_api", NULL);
}

// Universe fully built (2nd load pass complete, event_universe_generated).
// This is the definitive "world ready" signal. Only now is it safe for the
// client to start spawning/rendering host-streamed objects without racing the
// game's background simulation threads.
static void on_universe_ready(const char* /*event_name*/, void* /*data*/, void* /*userdata*/) {
    g_universe_ready = true;
    g_api->log(X4NATIVE_LOG_INFO, "x4mp: universe ready (2nd pass complete)");
    if (g_auto != "client") return;
    g_save_loading = false;
    client_mark_ready("universe_ready");
}

// ---- Extension entry ------------------------------------------------------

X4NATIVE_EXPORT int x4native_api_version(void) {
    return X4NATIVE_API_VERSION;
}

X4NATIVE_EXPORT int x4native_init(X4NativeAPI* api) {
    g_api = api;
    read_config();
    g_game = api->game;
    g_start = std::chrono::steady_clock::now();

    api->log(X4NATIVE_LOG_INFO, "x4mp: init (auto, server_ip)");

    if (!g_game)
        api->log(X4NATIVE_LOG_WARN, "x4mp: game function table NOT available yet");

    // Register Lua->C++ bridges so the menu can trigger host/join.
    // The menu fires <raise_lua_event name="x4mp.host"/> / "x4mp.join".
    if (api->register_lua_bridge) {
        api->register_lua_bridge("x4mp.host", "x4mp_host_request");
        api->register_lua_bridge("x4mp.join", "x4mp_join_request");
        api->log(X4NATIVE_LOG_INFO, "x4mp: registered Lua bridges x4mp.host / x4mp.join");
    } else {
        api->log(X4NATIVE_LOG_WARN, "x4mp: register_lua_bridge unavailable");
    }

    // Subscribe to the bridged C++ events + lifecycle events.
    g_sub_host   = api->subscribe("x4mp_host_request", on_host_request, nullptr, api);
    g_sub_join   = api->subscribe("x4mp_join_request", on_join_request, nullptr, api);
    g_sub_loaded = api->subscribe("on_game_loaded",    on_game_loaded, nullptr, api);
    g_sub_universe = api->subscribe("on_universe_ready", on_universe_ready, nullptr, api);
    g_sub_tick   = api->subscribe("on_frame_update", on_frame_update, nullptr, api);
    // Combat: receive ACT lines (kill / player-death) from x4mp_stream and send
    // them to the host over the control channel.
    g_sub_send_act = api->subscribe("x4mp.send_act", on_send_act, nullptr, api);

    // Optional env-driven fallback (OFF by default).
    if (g_auto == "client") {
        // THIN CLIENT (Option 1 intermediate): load the SAME save as the host
        // (so sector IDs match) but do NOT simulate — pause and render ONLY
        // objects streamed from the host. If no save is specified, fall back to
        // a minimal new game.
        if (g_save.empty()) {
            g_newgame_pending = true;
            api->log(X4NATIVE_LOG_INFO, "x4mp: X4MP_AUTO=client — thin client: no save, start a new game");
        } else {
            g_save_pending = true;
            char buf[256];
            snprintf(buf, sizeof(buf), "x4mp: X4MP_AUTO=client — thin client: will load save '%s' (paused, render host state)", g_save.c_str());
            api->log(X4NATIVE_LOG_INFO, buf);
        }
    } else if (g_auto == "host") {
        if (g_save.empty()) {
            api->log(X4NATIVE_LOG_INFO, "x4mp: X4MP_AUTO=host — will host a NEW game on game loaded");
        } else {
            g_save_pending = true;
            char buf[512];
            snprintf(buf, sizeof(buf), "x4mp: X4MP_AUTO=host — will LOAD save '%s' then host", g_save.c_str());
            api->log(X4NATIVE_LOG_INFO, buf);
        }
    }

    // Start our custom network transport (independent of SLNet).
    net_start();

    api->log(X4NATIVE_LOG_INFO, "x4mp: subscribed to host/join/loaded/universe/tick events");
    return X4NATIVE_OK;
}

X4NATIVE_EXPORT void x4native_shutdown(void) {
    if (!g_api) return;
    g_api->log(X4NATIVE_LOG_INFO, "x4mp: shutting down");
    if (g_sub_tick)   g_api->unsubscribe(g_sub_tick);
    if (g_sub_loaded) g_api->unsubscribe(g_sub_loaded);
    if (g_sub_host)   g_api->unsubscribe(g_sub_host);
    if (g_sub_join)   g_api->unsubscribe(g_sub_join);
    if (g_sub_universe) g_api->unsubscribe(g_sub_universe);
    if (g_sub_send_act) g_api->unsubscribe(g_sub_send_act);
    if (g_sock >= 0) { net_close_fd(g_sock, "shutdown_g_sock"); g_sock = -1; }
    // Release the consolidated-mode TCP listener + any per-client connections.
    // The game re-initializes the extension on its 2-pass save load; if we don't
    // close these here, the first pass's listener keeps port 7778 bound and the
    // second pass's bind() fails (g_listen_sock = -1 -> host never accepts).
    if (g_listen_sock >= 0) { net_close_fd(g_listen_sock, "shutdown_listen"); g_listen_sock = -1; }
    for (auto& c : g_clients) if (c.tcp_fd >= 0) { net_close_fd(c.tcp_fd, "shutdown_client"); c.tcp_fd = -1; }
    g_clients.clear();
    g_client_tcp_buf.clear();
    g_net_host = false;
    g_net_client = false;
    g_net_connected = false;
}