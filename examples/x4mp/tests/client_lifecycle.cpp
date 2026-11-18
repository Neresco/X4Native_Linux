// Exercise the real TCP setup and menu-session restore code. Network traffic
// is confined to one ephemeral loopback listener; game functions are stubbed.
#include "../x4mp.cpp"

static std::vector<unsigned char> session;
static std::vector<unsigned char> display_name;
static bool player_valid = false;
static bool valid_player() { return player_valid; }
static void fake_log(int, const char*) {}
static int subscribe(const char*, X4NativeEventCallback, void*, void*) { return 1; }
static int stash_set(const char*, const char* key, const void* data, uint32_t size) {
    auto bytes = static_cast<const unsigned char*>(data);
    auto& destination = !strcmp(key, "player_name") ? display_name : session;
    destination.assign(bytes, bytes + size); return 1;
}
static const void* stash_get(const char*, const char* key, uint32_t* size) {
    auto& source = !strcmp(key, "player_name") ? display_name : session;
    *size = static_cast<uint32_t>(source.size()); return source.empty() ? nullptr : source.data();
}
static int stash_remove(const char*, const char*) { session.clear(); return 1; }
static void check(bool value, const char* message) {
    if (!value) { std::fprintf(stderr, "FAIL: %s\n", message); std::exit(1); }
}
int main() {
    X4GameFunctions game{}; game.IsPlayerValid = valid_player;
    X4NativeAPI context{}; context.game = &game; context.log = fake_log;
    context.subscribe = subscribe; context.stash_set = stash_set;
    context.stash_get = stash_get; context.stash_remove = stash_remove;
    g_api = &context; g_game = &game;
    on_player_name(nullptr, const_cast<char*>("TestPilot"), nullptr);
    check(g_client_name == "TestPilot", "player name setting");
    on_player_name(nullptr, const_cast<char*>("bad\nJOIN"), nullptr);
    check(g_client_name == "TestPilot", "protocol injection in player name");
    x4mp_sock_ensure_init();
#ifdef _WIN32
    check(x4mp_connect_pending(WSAEWOULDBLOCK), "Windows pending connect rejected");
#else
    check(x4mp_connect_pending(EINPROGRESS), "POSIX pending connect rejected");
#endif
    int listener = static_cast<int>(socket(AF_INET, SOCK_STREAM, 0));
    check(listener >= 0, "loopback socket");
    sockaddr_in address{}; address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    check(bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0, "loopback bind");
    check(listen(listener, 1) == 0, "loopback listen");
    socklen_t length = sizeof(address);
    check(getsockname(listener, reinterpret_cast<sockaddr*>(&address), &length) == 0, "loopback port");
    g_stream_port = ntohs(address.sin_port); g_legacy_net = false; g_transport = "tcp";
    check(net_init_client("127.0.0.1", 0), "native nonblocking TCP setup");
    check(g_net_client && g_sock >= 0, "native client socket lost");
    const auto first_socket = g_sock;
    check(!net_init_client("127.0.0.1", 0) && g_sock == first_socket, "retry flooded a pending connection");
    close(g_sock); close(listener); g_sock = -1;

    g_auto = "client"; g_net_host_ip = "127.0.0.1"; g_save = "save_001";
    remember_menu_session(2);
    g_auto.clear(); g_net_host_ip = "192.0.2.1"; g_save_loading = false;
    g_save_pending = true; g_num_streams = 0;
    g_client_name = "default";
    // Existing transport is stubbed as started so init does no more networking.
    g_net_client = true;
    check(x4native_init(&context) == X4NATIVE_OK, "session restore init");
    check(g_auto == "client" && g_net_host_ip == "127.0.0.1", "menu role/address lost on reload");
    check(g_client_name == "TestPilot", "player name lost on reload");
    check(g_save_loading && !g_save_pending, "reload queued a second save load");
    check(!g_universe_ready && !g_client_ready, "first load pass bypassed readiness");
    on_game_loaded(nullptr, nullptr, nullptr);
    check(!g_client_ready, "game-loaded poll bypassed second pass");
    on_universe_ready(nullptr, nullptr, nullptr);
    check(g_client_ready && g_universe_ready, "real universe event did not ready client");
    player_valid = true; g_auto.clear(); g_universe_ready = false; g_client_ready = false;
    check(restore_menu_session() == 3 && g_auto == "client", "ready-session intent lost on UI rebuild");
    on_game_loaded(nullptr, nullptr, nullptr);
    check(!g_universe_ready && !g_client_ready, "reload inferred readiness from a valid player");
    on_universe_ready(nullptr, nullptr, nullptr);
    check(g_universe_ready && g_client_ready, "definitive event did not restore readiness");
    player_valid = false;
    check(restore_menu_session() == 0 && session.empty(), "main menu retained ready-session intent");
    std::puts("PASS: loopback TCP, menu role/address restore, no duplicate load, real readiness and main-menu expiry");
}
