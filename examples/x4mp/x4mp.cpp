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
// ---------------------------------------------------------------------------
#include <cstdlib>
#include <cstring>
#include <string>
#include <cstdio>
#include <chrono>
#include <vector>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <unordered_map>
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
static uint16_t        g_net_port = 7777;
static std::string     g_net_host_ip = "192.168.1.16";
static struct sockaddr_in g_host_sa;

// Host-side client tracking with liveness (for dead-client pruning).
struct NetClient {
    struct sockaddr_in addr;
    std::chrono::steady_clock::time_point last_seen;
    bool loading = false; // client is loading its save (don't prune during load)
    uint16_t stream_port = 7778; // client's dedicated stream port (x4mp_stream)
};
static std::vector<NetClient> g_clients;

// Host-side representation of each connected client's ship (so the host can
// SEE the client's ship). Key = hash of client sockaddr; value = spawned ship id.
static std::unordered_map<uint64_t, UniverseID> g_client_ships;

// Host-side cached ship state per client, used to relay to other clients so
// that clients can see each other. Key = hash of client sockaddr.
struct ClientShipState {
    float x=0, y=0, z=0, yaw=0, pitch=0, roll=0;
    char macro[160] = {0};
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
static bool            g_cleanup_enabled = true; // X4MP_CLEANUP=0 disables client own-object removal
static bool            g_pause_enabled = false;  // X4MP_PAUSE=1 pauses client (thin-client offload)

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

static bool net_init_host(uint16_t port) {
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
        close(g_sock); g_sock = -1;
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
    g_sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (g_sock < 0) { net_log("x4mp: net: socket() failed"); return false; }
    memset(&g_host_sa, 0, sizeof(g_host_sa));
    g_host_sa.sin_family = AF_INET;
    g_host_sa.sin_port = htons(port);
    if (inet_pton(AF_INET, ip, &g_host_sa.sin_addr) != 1) {
        net_log("x4mp: net: invalid server IP");
        close(g_sock); g_sock = -1;
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

// Send object/player RENDERING data to each client's dedicated stream port
// (handled by the x4mp_stream extension on the client).
static void net_send_stream(const char* msg) {
    for (auto& c : g_clients) {
        struct sockaddr_in sa = c.addr;
        sa.sin_port = htons(c.stream_port);
        net_send(&sa, msg);
        if (g_debug && (g_net_tick % 300) == 0) {
            char dbg[160];
            snprintf(dbg, sizeof(dbg), "x4mp: [DBG] HOST stream->%s:%u len=%zu",
                     inet_ntoa(c.addr.sin_addr), (unsigned)c.stream_port, strlen(msg));
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
    // Delta-compression state: last position sent to clients. We only re-send
    // an object if it moved beyond g_delta_m (or a periodic full refresh).
    float last_x = 0, last_y = 0, last_z = 0;
    bool sent = false;
};
static std::vector<ZoneObj> g_zone_objs;
static unsigned long long g_zone_objs_zone = 0;

// Network performance tuning.
static int    g_update_interval = 3;   // frames between state sends (3 = 20Hz; client interpolates)
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
    net_send_host(buf);
    // Also relay the host's OWN ship to all clients (cid=0) so they can SEE
    // the server's player ship as a separate entity. (SNAP alone only moves the
    // client's camera when paused; this spawns a visible ghost of the host.)
    if (g_game && g_game->GetPlayerObjectID && g_game->GetObjectPositionInSector) {
        UniverseID pship = g_game->GetPlayerObjectID();
        if (pship) {
            UIPosRot ppos = g_game->GetObjectPositionInSector(pship);
            char pmacro[160]; get_macro(pship, pmacro, sizeof(pmacro));
            char relay[256];
            snprintf(relay, sizeof(relay), "PLAYER 0 %.3f %.3f %.3f %.3f %.3f %.3f %s\n",
                     ppos.x, ppos.y, ppos.z, ppos.yaw, ppos.pitch, ppos.roll,
                     pmacro[0] ? pmacro : "?");
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
                        UniverseID objs[2048];
                        uint32_t no = g_game->GetAllFactionShips(objs, 2048, factions[f]);
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
                        UniverseID objs[2048];
                        uint32_t no = g_game->GetAllFactionStations(objs, 2048, factions[f]);
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
// HOST: refresh the cached list of objects in the player's current zone.
// Enumerating all ships/stations is expensive (~85k), so we only do this at a
// low frequency; positions are then streamed every frame from the cache.
static void net_refresh_zone_objects() {
    if (!g_game || !g_game->GetPlayerZoneID || !g_game->GetAllFactions || !g_game->GetAllFactionShips) return;
    UniverseID player_zone = g_game->GetPlayerZoneID();
    if (player_zone == 0) return;
    g_zone_objs.clear();
    g_zone_objs_zone = player_zone;
    // The host's own player ship is streamed separately via PLAYER cid=0, so
    // exclude it from the OBJ stream to avoid duplicate ghosts on the client.
    UniverseID host_player = g_game->GetPlayerObjectID ? g_game->GetPlayerObjectID() : 0;
    const char* factions[64];
    uint32_t nf = g_game->GetAllFactions(factions, 64, true);
    for (uint32_t f = 0; f < nf; f++) {
        UniverseID ships[2048];
        uint32_t ns = g_game->GetAllFactionShips(ships, 2048, factions[f]);
        for (uint32_t i = 0; i < ns; i++) {
            UniverseID obj = ships[i];
            if (g_game->IsValidComponent && !g_game->IsValidComponent(obj)) continue;
            if (host_player && obj == host_player) continue; // skip host player ship
            UniverseID zone = g_game->GetContextByClass ? g_game->GetContextByClass(obj, "zone", false) : 0;
            if (zone != player_zone) continue;
            ZoneObj zo; zo.id = obj;
            snprintf(zo.faction, sizeof(zo.faction), "%s", factions[f] ? factions[f] : "player");
            char macro[160]; get_macro(obj, macro, sizeof(macro));
            snprintf(zo.macro, sizeof(zo.macro), "%s", macro[0] ? macro : "?");
            g_zone_objs.push_back(zo);
        }
        if (g_game->GetAllFactionStations) {
            UniverseID stations[2048];
            uint32_t nst = g_game->GetAllFactionStations(stations, 2048, factions[f]);
            for (uint32_t i = 0; i < nst; i++) {
                UniverseID obj = stations[i];
                if (g_game->IsValidComponent && !g_game->IsValidComponent(obj)) continue;
                UniverseID zone = g_game->GetContextByClass ? g_game->GetContextByClass(obj, "zone", false) : 0;
                if (zone != player_zone) continue;
                ZoneObj zo; zo.id = obj;
                snprintf(zo.faction, sizeof(zo.faction), "%s", factions[f] ? factions[f] : "player");
                char macro[160]; get_macro(obj, macro, sizeof(macro));
                snprintf(zo.macro, sizeof(zo.macro), "%s", macro[0] ? macro : "?");
                g_zone_objs.push_back(zo);
            }
        }
    }
    if (g_debug) {
        char dbg[200];
        snprintf(dbg, sizeof(dbg), "x4mp: [DBG] HOST refreshed zone=%llu objects: %zu",
                 (unsigned long long)player_zone, g_zone_objs.size());
        net_debug(dbg);
    }
}

// HOST: stream the current positions of the cached zone objects to clients.
// Called every frame (fast — no enumeration). All OBJ lines are batched into a
// single UDP packet to avoid flooding the network with 200 packets/frame.
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
    for (auto& zo : g_zone_objs) {
        UniverseID obj = (UniverseID)zo.id;
        // Skip objects that no longer exist (destroyed). IsValidComponent avoids
        // the engine's "Failed to retrieve" warnings for dead IDs.
        if (g_game->IsValidComponent && !g_game->IsValidComponent(obj)) continue;
        UniverseID zone = g_game->GetContextByClass ? g_game->GetContextByClass(obj, "zone", false) : 0;
        if (zone == 0) continue; // no zone context -> not a live world object
        UIPosRot pos = g_game->GetObjectPositionInSector(obj);
        // Delta check: skip if not moved enough and not a forced full refresh.
        if (!force_full && zo.sent) {
            float dx = pos.x - zo.last_x, dy = pos.y - zo.last_y, dz = pos.z - zo.last_z;
            if ((dx*dx + dy*dy + dz*dz) < g_delta_m * g_delta_m) continue;
        }
        zo.last_x = pos.x; zo.last_y = pos.y; zo.last_z = pos.z; zo.sent = true;
        UniverseID sector = g_game->GetContextByClass ? g_game->GetContextByClass(obj, "sector", false) : 0;
        char buf[600];
        int n = snprintf(buf, sizeof(buf), "OBJ %llu %llu %llu %.3f %.3f %.3f %.3f %.3f %.3f %s %s\n",
                         (unsigned long long)obj, (unsigned long long)zone, (unsigned long long)sector,
                         pos.x, pos.y, pos.z, pos.yaw, pos.pitch, pos.roll,
                         zo.faction, zo.macro);
        if (used + (size_t)n < sizeof(batch)) {
            memcpy(batch + used, buf, (size_t)n);
            used += (size_t)n;
        }
        live.push_back(zo);
    }
    // Update the cache with only live objects so dead IDs are not re-streamed.
    if (live.size() != g_zone_objs.size()) g_zone_objs = std::move(live);
    if (used > 0) {
        batch[used] = 0;
        net_send_stream(batch); // -> client's dedicated stream port (x4mp_stream)
        if (g_debug && (g_net_tick % 300) == 0) {
            char dbg[160];
            snprintf(dbg, sizeof(dbg), "x4mp: [DBG] HOST OBJ batch: %zu bytes, %zu objs%s", used, live.size(), force_full ? " (FULL)" : "");
            net_debug(dbg);
        }
    } else if (g_debug && (g_net_tick % 300) == 0) {
        net_debug("x4mp: [DBG] HOST OBJ batch EMPTY (all filtered/stationary)");
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
        UniverseID ships[2048];
        uint32_t ns = g_game->GetAllFactionShips(ships, 2048, factions[f]);
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
        if (g_game->GetAllFactionStations) {
            UniverseID stations[2048];
            uint32_t nst = g_game->GetAllFactionStations(stations, 2048, factions[f]);
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

// OPTION 2: remove the client's own save objects (except the player's ship) so
// the client renders ONLY host-streamed objects. Called once after the save has
// fully loaded, so the client no longer renders the full universe (lower CPU).
static void thin_client_cleanup_own_objects() {
    if (!g_game || g_cleanup_done) return;
    if (!g_game->RemoveComponent || !g_game->GetAllFactions || !g_game->GetAllFactionShips) return;
    UniverseID player_ship = g_game->GetPlayerControlledShipID ? g_game->GetPlayerControlledShipID() : 0;
    const char* factions[64];
    uint32_t nf = g_game->GetAllFactions(factions, 64, true);
    uint32_t removed = 0, total_ships = 0, total_stations = 0;
    for (uint32_t f = 0; f < nf; f++) {
        UniverseID ships[2048];
        uint32_t ns = g_game->GetAllFactionShips(ships, 2048, factions[f]);
        total_ships += ns;
        for (uint32_t i = 0; i < ns; i++) {
            if (ships[i] == player_ship) continue;
            g_game->RemoveComponent(ships[i]);
            removed++;
        }
        if (g_game->GetAllFactionStations) {
            UniverseID stations[2048];
            uint32_t nst = g_game->GetAllFactionStations(stations, 2048, factions[f]);
            total_stations += nst;
            for (uint32_t i = 0; i < nst; i++) {
                g_game->RemoveComponent(stations[i]);
                removed++;
            }
        }
    }
    g_cleanup_done = true;
    if (g_debug) {
        char dbg[200];
        snprintf(dbg, sizeof(dbg),
                 "x4mp: [DBG] CLIENT cleanup: %u factions, %u ships, %u stations, removed %u (kept player ship %llu)",
                 nf, total_ships, total_stations, removed, (unsigned long long)player_ship);
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

static void process_message(const char* msg, const struct sockaddr_in& from) {
    if (g_net_host) {
        if (strncmp(msg, "JOIN", 4) == 0) {
            NetClient* c = find_client(from);
            if (!c) {
                NetClient nc;
                nc.addr = from;
                nc.last_seen = std::chrono::steady_clock::now();
                g_clients.push_back(nc);
                // A brand-new client has no objects yet, so force a full object
                // stream (clear delta state) so it gets the complete zone.
                for (auto& zo : g_zone_objs) zo.sent = false;
                char resp[64];
                int id = (int)g_clients.size();
                snprintf(resp, sizeof(resp), "WELCOME %d\n", id);
                net_send(&from, resp);
                char lbuf[128];
                snprintf(lbuf, sizeof(lbuf), "x4mp: net: client joined (id=%d, %s:%u)", id,
                         inet_ntoa(from.sin_addr), (unsigned)ntohs(from.sin_port));
                net_log(lbuf);
            } else {
                // Re-join from an existing client (e.g. after a link drop):
                // refresh liveness and re-send WELCOME so the client can
                // re-establish its session id.
                c->last_seen = std::chrono::steady_clock::now();
                int id = (int)(c - g_clients.data()) + 1;
                char resp[64];
                snprintf(resp, sizeof(resp), "WELCOME %d\n", id);
                net_send(&from, resp);
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
        } else if (strncmp(msg, "PLAYER", 6) == 0) {
            // Client reports its ship: PLAYER <x> <y> <z> <yaw> <pitch> <roll> <macro>
            // 1) spawn/update a ghost on the host (so the host SEES the client),
            // 2) relay the client's ship to all OTHER clients (so clients see
            //    each other). Host's own ship is already streamed via SNAP/OBJ.
            float x, y, z, yaw, pitch, roll; char macro[160];
            if (sscanf(msg, "PLAYER %f %f %f %f %f %f %159s",
                       &x, &y, &z, &yaw, &pitch, &roll, macro) == 7) {
                uint64_t key = ((uint64_t)from.sin_addr.s_addr << 16) | (uint32_t)ntohs(from.sin_port);
                ClientShipState& st = g_client_ship_state[key];
                st.x = x; st.y = y; st.z = z; st.yaw = yaw; st.pitch = pitch; st.roll = roll;
                snprintf(st.macro, sizeof(st.macro), "%s", macro[0] ? macro : "?");
                // Spawn/update the ghost on the host (host's player sector).
                UniverseID sector = (g_game && g_game->GetPlayerZoneID && g_game->GetContextByClass)
                    ? g_game->GetContextByClass(g_game->GetPlayerZoneID(), "sector", false) : 0;
                UIPosRot pos; pos.x = x; pos.y = y; pos.z = z;
                pos.yaw = yaw; pos.pitch = pitch; pos.roll = roll;
                auto it = g_client_ships.find(key);
                if (it == g_client_ships.end()) {
                    if (g_game && g_game->SpawnObjectAtPos2 && sector) {
                        UniverseID ship = g_game->SpawnObjectAtPos2(st.macro, sector, pos, "player");
                        if (ship) g_client_ships[key] = ship;
                        if (g_debug) { char dbg[200]; snprintf(dbg, sizeof(dbg), "x4mp: [DBG] HOST spawned client ghost id=%llu macro=%s", (unsigned long long)ship, st.macro); net_debug(dbg); }
                    }
                } else {
                    if (g_game && g_game->SetObjectSectorPos && sector)
                        g_game->SetObjectSectorPos(it->second, sector, pos);
                }
                // Relay to all OTHER clients (via their stream port).
                char relay[256];
                int rn = snprintf(relay, sizeof(relay),
                                  "PLAYER %llu %.3f %.3f %.3f %.3f %.3f %.3f %s\n",
                                  (unsigned long long)key, x, y, z, yaw, pitch, roll, st.macro);
                for (auto& c : g_clients) {
                    if (c.addr.sin_port == from.sin_port && c.addr.sin_addr.s_addr == from.sin_addr.s_addr) continue;
                    struct sockaddr_in sa = c.addr;
                    sa.sin_port = htons(c.stream_port);
                    net_send(&sa, relay);
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
            net_send(&g_host_sa, "PONG\n");
        } else if (strncmp(msg, "PLAYER", 6) == 0) {
            // Host relayed another client's ship: PLAYER <cid> <x> <y> <z> <yaw> <pitch> <roll> <macro>
            // Spawn/update a ghost of that remote client in OUR player sector so
            // we can see them. (We never receive our own relay, so no self-ghost.)
            unsigned long long cid; float x, y, z, yaw, pitch, roll; char macro[160];
            if (sscanf(msg, "PLAYER %llu %f %f %f %f %f %f %159s",
                       &cid, &x, &y, &z, &yaw, &pitch, &roll, macro) == 8) {
                UniverseID player_zone = (g_game && g_game->GetPlayerZoneID) ? g_game->GetPlayerZoneID() : 0;
                UniverseID sector = (g_game && g_game->GetContextByClass && player_zone)
                    ? g_game->GetContextByClass(player_zone, "sector", false) : 0;
                UIPosRot pos; pos.x = x; pos.y = y; pos.z = z;
                pos.yaw = yaw; pos.pitch = pitch; pos.roll = roll;
                auto it = g_remote_ships.find(cid);
                if (it == g_remote_ships.end()) {
                    if (g_game && g_game->SpawnObjectAtPos2 && sector) {
                        UniverseID ship = g_game->SpawnObjectAtPos2(macro[0] ? macro : "?", sector, pos, "player");
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
            process_message(line, from);
            line = strtok_r(nullptr, "\n", &save);
        }
    }
}

// Called each frame: poll incoming data + send periodic keepalive/snapshot.
static void net_update() {
    net_poll();
    if (g_sock < 0) return;
    g_net_tick++;
    if (g_net_host) {
        // Prune dead clients: remove any that have sent nothing for the timeout.
        if ((g_net_tick % 300) == 0 && !g_clients.empty()) {
            auto now = std::chrono::steady_clock::now();
            size_t before = g_clients.size();
            g_clients.erase(std::remove_if(g_clients.begin(), g_clients.end(),
                [&](const NetClient& c) {
                    // Never prune a client that is still loading its save
                    // (it cannot send data while the game is busy loading).
                    if (c.loading) return false;
                    auto age = std::chrono::duration_cast<std::chrono::milliseconds>(
                                   now - c.last_seen).count();
                    return age > g_host_client_timeout_ms;
                }), g_clients.end());
            if (g_clients.size() != before) {
                char lbuf[128];
                snprintf(lbuf, sizeof(lbuf), "x4mp: net: pruned %zu dead client(s); %zu remain",
                         before - g_clients.size(), g_clients.size());
                net_log(lbuf);
            }
        }
        // Debug: enumerate ships/stations across all factions.
        if (g_debug && (g_net_tick % 600) == 0) {
            if (g_game && g_game->GetAllFactions && g_game->GetAllFactionShips) {
                const char* factions[64];
                uint32_t nf = g_game->GetAllFactions(factions, 64, true);
                uint32_t total_ships = 0, total_stations = 0;
                for (uint32_t i = 0; i < nf; i++) {
                    UniverseID ships[2048];
                    total_ships += g_game->GetAllFactionShips(ships, 2048, factions[i]);
                    if (g_game->GetAllFactionStations) {
                        UniverseID stations[2048];
                        total_stations += g_game->GetAllFactionStations(stations, 2048, factions[i]);
                    }
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
            net_send_host(buf);
        }
        // Rate-limit state streaming: only send SNAP/OBJ every g_update_interval
        // frames (default 15 Hz instead of every frame at 60fps). This cuts
        // bandwidth and CPU by ~4x with no perceptible loss for a thin client.
        if (!g_clients.empty() && (g_net_tick % g_update_interval) == 0) {
            // Layer 2: stream a universe-state snapshot to clients.
            net_send_snapshot_host();
            // Layer 2: stream objects in the player's zone.
            if (g_objmode == "full") {
                // FULL mode: enumerate + stream every frame (no cache).
                net_send_objects_host_full();
            } else {
                // CACHE mode: refresh the zone-object cache at low frequency,
                // then stream positions from the cache (with delta compression).
                if ((g_net_tick % 300) == 0) net_refresh_zone_objects();
                net_send_objects_host();
            }
        }
    } else if (g_net_client) {
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
            if ((g_net_tick % 60) == 0) net_send(&g_host_sa, "JOIN x4mp\n");
        } else {
            // Tell the host our dedicated stream port once (x4mp_stream).
            if (!g_stream_reported) {
                char sbuf[32];
                snprintf(sbuf, sizeof(sbuf), "STREAM %d\n", g_stream_port);
                net_send(&g_host_sa, sbuf);
                g_stream_reported = true;
            }
            // While loading our save, tell the host so it won't prune us
            // (we can't send INPUT while the game is busy loading).
            if (g_save_loading && (g_net_tick % 60) == 0)
                net_send(&g_host_sa, "LOADING\n");
            // Layer 2: send input/commands to the host (rate-limited to the
            // same update interval to reduce uplink traffic).
            if ((g_net_tick % g_update_interval) == 0) {
                char buf[128];
                snprintf(buf, sizeof(buf), "INPUT tick=%u\n", g_net_tick);
                net_send(&g_host_sa, buf);
                // Send our ship state so the host (and other clients via relay)
                // can see us. Only once we are in the universe with a valid ship.
                if (g_client_ready && g_game && g_game->GetPlayerObjectID &&
                    g_game->GetObjectPositionInSector) {
                    UniverseID ship = g_game->GetPlayerObjectID();
                    if (ship) {
                        UIPosRot pos = g_game->GetObjectPositionInSector(ship);
                        char macro[160]; get_macro(ship, macro, sizeof(macro));
                        char pbuf[256];
                        snprintf(pbuf, sizeof(pbuf), "PLAYER %.3f %.3f %.3f %.3f %.3f %.3f %s\n",
                                 pos.x, pos.y, pos.z, pos.yaw, pos.pitch, pos.roll,
                                 macro[0] ? macro : "?");
                        net_send(&g_host_sa, pbuf);
                        if (g_debug) {
                            char dbg[300];
                            snprintf(dbg, sizeof(dbg), "x4mp: [DBG] CLIENT sending PLAYER ship=%llu pos=(%.1f,%.1f,%.1f) macro=%s",
                                     (unsigned long long)ship, pos.x, pos.y, pos.z, macro[0] ? macro : "?");
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
    const char* rel = std::getenv("X4MP_RELEVANCE_M");
    if (rel) { float v = (float)atof(rel); if (v >= 1000.0f && v <= 1000000.0f) g_relevance_m = v; }
    const char* dbg = std::getenv("X4MP_DEBUG");
    if (dbg && *dbg == '1') g_debug = true;
    const char* om = std::getenv("X4MP_OBJMODE");
    if (om && *om) g_objmode = om;
    const char* cl = std::getenv("X4MP_CLEANUP");
    if (cl && *cl == '0') g_cleanup_enabled = false;
    const char* st = std::getenv("X4MP_STREAMS");
    if (st && *st) {
        int n = atoi(st);
        if (n >= 0 && n <= 8) g_num_streams = n;
    }
    const char* ps = std::getenv("X4MP_PAUSE");
    if (ps && *ps == '1') g_pause_enabled = true;
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

static void on_host_request(const char* /*event_name*/, void* data, void* /*userdata*/) {
    const char* module = (data && *(const char*)data) ? (const char*)data : nullptr;
    g_api->log(X4NATIVE_LOG_INFO, "x4mp: host requested — will host on next frame update");
    // Defer to the frame update. Calling NewMultiplayerGame synchronously
    // from the Lua menu handler crashes the game. The frame update runs
    // outside the Lua call stack, in the game's native loop.
    g_host_pending = true;
    if (module && *module) g_module = module;
}

static void on_join_request(const char* /*event_name*/, void* data, void* /*userdata*/) {
    const char* ip = (data && *(const char*)data) ? (const char*)data : nullptr;
    do_join(ip);
}

// Forward decl: client_mark_ready() is defined below (after on_frame_update).
static void client_mark_ready(const char* source);

static void on_frame_update(const char* /*event_name*/, void* data, void* /*userdata*/) {
    (void)data;
    // Drive our custom network every frame.
    net_update();
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
        thin_client_render_objects();
        // OPTION 2: remove the client's own save objects shortly after the save
        // has loaded, so the client renders only host-streamed objects.
        if (g_cleanup_pending && !g_cleanup_done && g_cleanup_enabled) {
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                               std::chrono::steady_clock::now() - g_ready_time)
                               .count();
            if (elapsed >= 30000) {
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
    // Tell the host we finished loading so it resumes normal liveness pruning.
    net_send(&g_host_sa, "READY\n");
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
    if (g_sock >= 0) { close(g_sock); g_sock = -1; }
}