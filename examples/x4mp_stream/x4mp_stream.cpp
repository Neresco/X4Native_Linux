// ==========================================================================
// x4mp_stream - Client-side server->client object stream handler for X4MP.
//
// This extension runs ONLY on the CLIENT. It owns a dedicated UDP socket
// (default port 7778) that receives the host's object/player stream (OBJ and
// PLAYER messages) and renders it with SMOOTHING:
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
#include <chrono>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>

static X4NativeAPI*    g_api  = nullptr;
static X4GameFunctions* g_game = nullptr;

// ---- Config (env overrides) ----------------------------------------------
static int   g_stream_port   = 7778;   // X4MP_STREAM_PORT
static int   g_render_interval = 3;    // frames between render passes (20Hz @60fps)
static float g_smooth_tau    = 0.12f;  // interpolation time constant (seconds)

// ---- Remote object state --------------------------------------------------
struct RemoteObj {
    unsigned long long host_id = 0;
    char macro[160] = {0};
    char faction[80] = {0};
    // interpolation state
    float tx=0, ty=0, tz=0, tyaw=0, tpitch=0, troll=0; // latest target
    float px=0, py=0, pz=0;                            // previous position
    std::chrono::steady_clock::time_point last_update;
    UniverseID client_id = 0;                          // spawned ghost (0 = none)
    bool has_target = false;
    bool spawned = false;
};

static std::unordered_map<unsigned long long, RemoteObj> g_objs;
static std::mutex g_mutex;

// ---- Network --------------------------------------------------------------
static int g_sock = -1;
static std::thread g_recv_thread;
static volatile bool g_running = false;

static void net_log(const char* msg) {
    if (g_api && g_api->log) g_api->log(X4NATIVE_LOG_INFO, msg);
}

// ---- Receive thread: parse packets, store target positions ----------------
static void parse_line(char* line) {
    if (strncmp(line, "OBJ", 3) == 0) {
        unsigned long long id, zone, sector;
        float x, y, z, yaw, pitch, roll;
        char faction[80], macro[160];
        if (sscanf(line, "OBJ %llu %llu %llu %f %f %f %f %f %f %79s %159s",
                   &id, &zone, &sector, &x, &y, &z, &yaw, &pitch, &roll, faction, macro) == 11) {
            std::lock_guard<std::mutex> lk(g_mutex);
            RemoteObj& o = g_objs[id];
            if (o.has_target) { o.px = o.tx; o.py = o.ty; o.pz = o.tz; }
            o.host_id = id;
            snprintf(o.macro, sizeof(o.macro), "%s", macro[0] ? macro : "?");
            snprintf(o.faction, sizeof(o.faction), "%s", faction[0] ? faction : "player");
            o.tx = x; o.ty = y; o.tz = z; o.tyaw = yaw; o.tpitch = pitch; o.troll = roll;
            o.last_update = std::chrono::steady_clock::now();
            o.has_target = true;
        }
    } else if (strncmp(line, "PLAYER", 6) == 0) {
        // PLAYER <cid> <x> <y> <z> <yaw> <pitch> <roll> <macro>
        unsigned long long cid;
        float x, y, z, yaw, pitch, roll;
        char macro[160];
        if (sscanf(line, "PLAYER %llu %f %f %f %f %f %f %159s",
                   &cid, &x, &y, &z, &yaw, &pitch, &roll, macro) == 8) {
            std::lock_guard<std::mutex> lk(g_mutex);
            RemoteObj& o = g_objs[cid];
            if (o.has_target) { o.px = o.tx; o.py = o.ty; o.pz = o.tz; }
            o.host_id = cid;
            snprintf(o.macro, sizeof(o.macro), "%s", macro[0] ? macro : "?");
            snprintf(o.faction, sizeof(o.faction), "player");
            o.tx = x; o.ty = y; o.tz = z; o.tyaw = yaw; o.tpitch = pitch; o.troll = roll;
            o.last_update = std::chrono::steady_clock::now();
            o.has_target = true;
        }
    }
}

static void recv_loop() {
    char buf[65536];
    while (g_running) {
        ssize_t n = recv(g_sock, buf, sizeof(buf) - 1, 0);
        if (n <= 0) { usleep(2000); continue; }
        buf[n] = 0;
        // A packet may contain multiple newline-delimited lines.
        char* save = nullptr;
        for (char* line = strtok_r(buf, "\n", &save); line; line = strtok_r(nullptr, "\n", &save)) {
            if (line[0]) parse_line(line);
        }
    }
}

// ---- Main-thread render pass ----------------------------------------------
// Interpolate each ghost toward its target and position it. Only touches the
// game API from the main thread (called from on_frame_update).
static void render_pass() {
    if (!g_game) return;
    if (!g_game->SpawnObjectAtPos2 || !g_game->SetObjectSectorPos) return;
    if (!g_game->GetPlayerZoneID || !g_game->GetContextByClass) return;

    UniverseID player_zone = g_game->GetPlayerZoneID();
    if (player_zone == 0) return;
    UniverseID client_sector = g_game->GetContextByClass(player_zone, "sector", false);
    if (!client_sector) return;

    auto now = std::chrono::steady_clock::now();

    std::lock_guard<std::mutex> lk(g_mutex);
    std::vector<unsigned long long> dead;
    static unsigned dbg_tick = 0;
    static unsigned spawned_total = 0;
    unsigned spawned_now = 0;
    if ((++dbg_tick % 300) == 0) {
        char dbg[256];
        // Show the target of the first tracked object so we can see if the
        // host is sending changing positions (movement) or static ones.
        float sx = 0, sy = 0, sz = 0; unsigned long long sid = 0;
        for (auto& kv : g_objs) { sid = kv.first; sx = kv.second.tx; sy = kv.second.ty; sz = kv.second.tz; break; }
        snprintf(dbg, sizeof(dbg), "x4mp_stream: [DBG] objs=%zu spawned=%u total=%u sector=%llu sample=%llu pos=(%.1f,%.1f,%.1f)",
                 g_objs.size(), spawned_now, spawned_total, (unsigned long long)client_sector, sid, sx, sy, sz);
        net_log(dbg);
    }
    for (auto& kv : g_objs) {
        RemoteObj& o = kv.second;
        if (!o.has_target) continue;
        // Prune objects not updated for a while (host stopped sending / destroyed).
        // Long timeout so stationary objects (not re-sent due to delta compression)
        // do not flicker. The host sends periodic full refreshes to keep the set
        // authoritative; this only clears objects truly gone for a long time.
        auto age = std::chrono::duration_cast<std::chrono::milliseconds>(now - o.last_update).count();
        if (age > 30000) { dead.push_back(kv.first); continue; }

        // Interpolate: exponential chase toward the latest target. This smooths
        // movement without teleporting, at any host update rate.
        float dt = std::chrono::duration_cast<std::chrono::duration<float>>(now - o.last_update).count();
        float alpha = 1.0f - expf(-dt / (g_smooth_tau > 0 ? g_smooth_tau : 0.12f));
        if (alpha > 1.0f) alpha = 1.0f;
        float ix = o.px + (o.tx - o.px) * alpha;
        float iy = o.py + (o.ty - o.py) * alpha;
        float iz = o.pz + (o.tz - o.pz) * alpha;

        UIPosRot pos; pos.x = ix; pos.y = iy; pos.z = iz;
        pos.yaw = o.tyaw; pos.pitch = o.tpitch; pos.roll = o.troll;

        if (!o.spawned) {
            UniverseID newid = g_game->SpawnObjectAtPos2(o.macro, client_sector, pos,
                                                         (o.faction[0]) ? o.faction : "player");
            if (newid != 0) {
                o.client_id = newid;
                o.spawned = true;
                spawned_total++;
                if (spawned_total < 60 || (spawned_total % 30) == 0) {
                    char dbg[192];
                    snprintf(dbg, sizeof(dbg), "x4mp_stream: [DBG] SPAWN id=%llu macro=%s at (%.0f,%.0f,%.0f) total=%u",
                             (unsigned long long)kv.first, o.macro, ix, iy, iz, spawned_total);
                    net_log(dbg);
                }
            }
        } else {
            spawned_now++;
            if (g_game->IsValidComponent && !g_game->IsValidComponent(o.client_id)) {
                if ((dbg_tick % 300) == 0) {
                    char dbg[160];
                    snprintf(dbg, sizeof(dbg), "x4mp_stream: [DBG] ghost id=%llu became invalid; respawning", (unsigned long long)o.client_id);
                    net_log(dbg);
                }
                o.spawned = false; // ghost was destroyed; respawn next pass
                continue;
            }
            g_game->SetObjectSectorPos(o.client_id, client_sector, pos);
        }
    }
    for (auto id : dead) g_objs.erase(id);
}

// ---- Frame update (main thread) -------------------------------------------
static void on_frame_update(const char* /*name*/, void* /*data*/, void* /*ud*/) {
    static unsigned tick = 0;
    if ((++tick % (unsigned)(g_render_interval > 0 ? g_render_interval : 3)) != 0) return;
    render_pass();
}

// ---- Init / shutdown -------------------------------------------------------
static int g_sub_tick = -1;

X4NATIVE_EXPORT int x4native_api_version(void) { return 1; }

X4NATIVE_EXPORT int x4native_init(X4NativeAPI* api) {
    g_api = api;
    g_game = api->game;

    const char* sp = std::getenv("X4MP_STREAM_PORT");
    if (sp) { int v = atoi(sp); if (v > 0 && v < 65535) g_stream_port = v; }
    const char* ri = std::getenv("X4MP_RENDER_INTERVAL");
    if (ri) { int v = atoi(ri); if (v >= 1 && v <= 60) g_render_interval = v; }
    const char* tau = std::getenv("X4MP_SMOOTH_TAU");
    if (tau) { float v = (float)atof(tau); if (v >= 0.001f && v <= 2.0f) g_smooth_tau = v; }

    // Open the stream socket.
    g_sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (g_sock < 0) { net_log("x4mp_stream: failed to create socket"); return X4NATIVE_OK; }
    int one = 1;
    setsockopt(g_sock, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in sa; memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_ANY);
    sa.sin_port = htons((uint16_t)g_stream_port);
    if (bind(g_sock, (struct sockaddr*)&sa, sizeof(sa)) < 0) {
        char m[128]; snprintf(m, sizeof(m), "x4mp_stream: bind port %d failed", g_stream_port);
        net_log(m);
        close(g_sock); g_sock = -1;
        return X4NATIVE_OK;
    }
    int flags = fcntl(g_sock, F_GETFL, 0);
    fcntl(g_sock, F_SETFL, flags | O_NONBLOCK);

    g_running = true;
    g_recv_thread = std::thread(recv_loop);

    g_sub_tick = api->subscribe("on_frame_update", on_frame_update, nullptr, api);

    char m[128];
    snprintf(m, sizeof(m), "x4mp_stream: listening on UDP %d; render interval %d frames", g_stream_port, g_render_interval);
    net_log(m);
    return X4NATIVE_OK;
}

X4NATIVE_EXPORT void x4native_shutdown(void) {
    g_running = false;
    if (g_recv_thread.joinable()) g_recv_thread.join();
    if (g_sock >= 0) close(g_sock);
    g_sock = -1;
    if (g_api && g_api->unsubscribe && g_sub_tick >= 0) g_api->unsubscribe(g_sub_tick);
    net_log("x4mp_stream: shutting down");
}
