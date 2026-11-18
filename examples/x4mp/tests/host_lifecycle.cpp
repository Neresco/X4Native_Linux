// Regression: hosting an existing world must not replace it or lose its role.
// Include the real lifecycle/callback implementation with a minimal game API.
#include "../x4mp.cpp"
#include <cstdio>
#include <cstdlib>

static int new_world_calls = 0;
static bool player_valid = true;
static bool fake_player_valid() { return player_valid; }
static void fake_new_world(const char*, const char*) { ++new_world_calls; }
static void fake_log(int, const char*) {}

static void check(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        std::exit(1);
    }
}

int main() {
    X4GameFunctions game{};
    game.IsPlayerValid = fake_player_valid;
    game.NewMultiplayerGame = fake_new_world;
    X4NativeAPI api{};
    api.game = &game;
    api.log = fake_log;
    g_api = &api;
    g_game = &game;
    g_auto = "host";
    // No sockets are opened: transport has already been selected by the
    // request handler, and this test exercises lifecycle/frame scheduling.
    g_net_host = true;
    g_num_streams = 0;
    g_perf_log_time = std::chrono::steady_clock::now();

    g_universe_ready = false;
    do_host(nullptr, nullptr);
    check(!g_host_done, "host must wait for universe readiness");
    check(g_host_pending, "waiting host must retry from a frame callback");
    check(new_world_calls == 0, "waiting must not replace the loaded universe");
    on_frame_update(nullptr, nullptr, nullptr);
    check(!g_host_done && g_host_pending, "frame callback must retain the readiness wait");

    g_universe_ready = true;
    on_frame_update(nullptr, nullptr, nullptr);
    check(g_host_done && !g_host_pending, "ready world must become the host");
    check(g_auto == "host", "runtime-selected host role must survive");
    check(new_world_calls == 0, "hosting must preserve the existing universe");
    int updates = g_perf_net_count;
    on_frame_update(nullptr, nullptr, nullptr);
    check(g_perf_net_count == updates + 1, "native network update must continue after hosting");
    do_host(nullptr, nullptr);
    check(new_world_calls == 0, "duplicate host request must preserve the world");

    // Retain the original new-world path when no player/world exists.
    player_valid = false;
    g_host_done = false;
    do_host(nullptr, nullptr);
    check(new_world_calls == 1, "empty-world host path must retain its existing behavior");
    std::puts("PASS: host readiness, world preservation and continuing native callbacks");
}
