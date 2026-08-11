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

#include <x4native_extension.h>
#include <x4_game_func_table.h>

static X4NativeAPI*   g_api       = nullptr;
static X4GameFunctions* g_game    = nullptr;
static int             g_sub_tick  = 0;
static int             g_sub_loaded = 0;
static int             g_sub_host  = 0;
static int             g_sub_join  = 0;
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
static uint16_t        g_net_port = 7777;
static std::string     g_net_host_ip = "192.168.1.16";
static struct sockaddr_in g_host_sa;
static std::vector<struct sockaddr_in> g_clients;
static unsigned        g_net_tick = 0;
static bool            g_debug = false;   // X4MP_DEBUG=1 -> continuous streamed-data display

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
    char buf[128];
    snprintf(buf, sizeof(buf), "x4mp: net: CLIENT targeting %s:%u", ip, (unsigned)port);
    net_log(buf);
    return true;
}

static void net_send(const struct sockaddr_in* to, const char* msg) {
    if (g_sock < 0) return;
    sendto(g_sock, msg, (size_t)strlen(msg), 0, (struct sockaddr*)to, sizeof(*to));
}

static void net_send_host(const char* msg) {
    for (auto& c : g_clients) net_send(&c, msg);
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

// HOST: stream the objects in the player's current zone (ships + stations).
// This mirrors how X4 single-player works: only what is near the player is
// fully rendered/simulated; everything far away is reduced. We enumerate all
// ships/stations at a low frequency and stream only those in the player's zone.
static void net_send_objects_host() {
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
            net_send_host(buf);
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
                net_send_host(buf);
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
    UIPosRot pos;
    pos.x = g_snap_x; pos.y = g_snap_y; pos.z = g_snap_z;
    pos.yaw = 0; pos.pitch = 0; pos.roll = 0;
    g_game->MovePlayerToSectorPos((UniverseID)g_snap_zone, pos);
    if (g_debug) {
        char dbg[160];
        snprintf(dbg, sizeof(dbg), "x4mp: [DBG] CLIENT applied player pos=(%.1f,%.1f,%.1f) zone=%llu",
                 g_snap_x, g_snap_y, g_snap_z, g_snap_zone);
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
static void net_poll() {
    if (g_sock < 0) return;
    char buf[4096];
    struct sockaddr_in from; socklen_t fromlen = sizeof(from);
    ssize_t n;
    while ((n = recvfrom(g_sock, buf, sizeof(buf) - 1, 0, (struct sockaddr*)&from, &fromlen)) > 0) {
        buf[n] = 0;
        std::string msg(buf);
        if (g_net_host) {
            if (msg.rfind("JOIN", 0) == 0) {
                bool found = false;
                for (auto& c : g_clients)
                    if (c.sin_port == from.sin_port && c.sin_addr.s_addr == from.sin_addr.s_addr) { found = true; break; }
                if (!found) {
                    g_clients.push_back(from);
                    char resp[64];
                    int id = (int)g_clients.size();
                    snprintf(resp, sizeof(resp), "WELCOME %d\n", id);
                    net_send(&from, resp);
                    char lbuf[128];
                    snprintf(lbuf, sizeof(lbuf), "x4mp: net: client joined (id=%d, %s:%u)", id,
                             inet_ntoa(from.sin_addr), (unsigned)ntohs(from.sin_port));
                    net_log(lbuf);
                }
            } else if (msg.rfind("INPUT", 0) == 0) {
                // Client input/command received. Log occasionally.
                if ((g_net_tick % 60) == 0) {
                    char lbuf[192];
                    snprintf(lbuf, sizeof(lbuf), "x4mp: net: HOST got INPUT from %s: %s",
                             inet_ntoa(from.sin_addr), msg.c_str());
                    net_log(lbuf);
                }
            }
        } else if (g_net_client) {
            if (msg.rfind("WELCOME", 0) == 0) {
                g_net_connected = true;
                net_log("x4mp: net: CONNECTED to host (handshake OK)");
            } else if (msg.rfind("SNAP", 0) == 0) {
                // Parse host snapshot: SNAP <id> <zone> <x> <y> <z> <yaw> <pitch> <roll> <tick>
                unsigned long long id, zone; float x, y, z, yaw, pitch, roll; unsigned tick;
                if (sscanf(buf, "SNAP %llu %llu %f %f %f %f %f %f %u",
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
            } else if (msg.rfind("OBJ", 0) == 0) {
                // Parse host object: OBJ <id> <zone> <sector> <x> <y> <z> <yaw> <pitch> <roll> <faction> <macro>
                unsigned long long id, zone, sector; float x, y, z, yaw, pitch, roll; char faction[80], macro[160];
                if (sscanf(buf, "OBJ %llu %llu %llu %f %f %f %f %f %f %79s %159s",
                           &id, &zone, &sector, &x, &y, &z, &yaw, &pitch, &roll, faction, macro) == 11) {
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
}

// Called each frame: poll incoming data + send periodic keepalive/snapshot.
static void net_update() {
    net_poll();
    if (g_sock < 0) return;
    g_net_tick++;
    if (g_net_host) {
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
        // Layer 2: stream a universe-state snapshot to clients (~every 30 frames).
        if ((g_net_tick % 30) == 0 && !g_clients.empty()) {
            net_send_snapshot_host();
        }
        // Layer 2: stream objects in the player's zone (~every 5s).
        if ((g_net_tick % 300) == 0 && !g_clients.empty()) {
            net_send_objects_host();
        }
    } else if (g_net_client) {
        // Send a JOIN until we're connected.
        if (!g_net_connected) {
            if ((g_net_tick % 60) == 0) net_send(&g_host_sa, "JOIN x4mp\n");
        } else {
            // Layer 2: send input/commands to the host (~every 30 frames).
            if ((g_net_tick % 30) == 0) {
                char buf[128];
                snprintf(buf, sizeof(buf), "INPUT tick=%u\n", g_net_tick);
                net_send(&g_host_sa, buf);
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
    const char* dbg = std::getenv("X4MP_DEBUG");
    if (dbg && *dbg == '1') g_debug = true;
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

static void on_frame_update(const char* /*event_name*/, void* data, void* /*userdata*/) {
    (void)data;
    // Drive our custom network every frame.
    net_update();
    // THIN CLIENT: render the host's streamed state (local sim is paused).
    if (g_auto == "client") {
        thin_client_render();
        thin_client_render_objects();
        // OPTION 2: remove the client's own save objects shortly after the save
        // has loaded, so the client renders only host-streamed objects.
        if (g_cleanup_pending && !g_cleanup_done) {
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
    // CLIENT: the save finished loading -> we are now in a universe. Mark ready
    // (we do NOT host; the host is authoritative).
    if (g_auto == "client") {
        g_save_loading = false;
        g_client_ready = true;
        g_api->log(X4NATIVE_LOG_INFO, "x4mp: client in universe — ready to render host state");
        // OPTION 2: after a short delay remove the client's own save objects so
        // it renders ONLY host-streamed objects.
        g_cleanup_pending = true;
        g_ready_time = std::chrono::steady_clock::now();
        // THIN CLIENT: pause the local simulation (the host is authoritative).
        // This offloads universe calculation from the client to the host.
        if (g_api->raise_lua_event)
            g_api->raise_lua_event("x4mp.pause", NULL);
        else
            g_api->log(X4NATIVE_LOG_WARN, "x4mp: raise_lua_event unavailable — cannot pause client");
        // Debug: probe which Lua API functions are available for object streaming.
        if (g_api->raise_lua_event)
            g_api->raise_lua_event("x4mp.debug_api", NULL);
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

    api->log(X4NATIVE_LOG_INFO, "x4mp: subscribed to host/join/loaded/tick events");
    return X4NATIVE_OK;
}

X4NATIVE_EXPORT void x4native_shutdown(void) {
    if (!g_api) return;
    g_api->log(X4NATIVE_LOG_INFO, "x4mp: shutting down");
    if (g_sub_tick)   g_api->unsubscribe(g_sub_tick);
    if (g_sub_loaded) g_api->unsubscribe(g_sub_loaded);
    if (g_sub_host)   g_api->unsubscribe(g_sub_host);
    if (g_sub_join)   g_api->unsubscribe(g_sub_join);
    if (g_sock >= 0) { close(g_sock); g_sock = -1; }
}