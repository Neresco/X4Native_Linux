// Filesystem stop requests become Lua work only from the verified UI callback.
#include "x4native_extension.h"
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
static X4NativeAPI* api = nullptr;
static std::filesystem::path directory;
static std::chrono::steady_clock::time_point last_poll{};
static int frame_subscription = 0, save_subscription = 0;
static bool requested = false;
static void frame(const char*, void*, void*) {
    try {
        auto now = std::chrono::steady_clock::now();
        if (requested || now-last_poll < std::chrono::seconds(1)) return;
        last_poll=now;
        if (std::filesystem::exists(directory/"stop-requested")) {
            requested=true;
            api->raise_lua_event("x4_host.save_stop", nullptr);
        }
    } catch (...) { api->log(X4NATIVE_LOG_WARN,"host control: stop request unavailable"); }
}
static void saved(const char*, void*, void*) {
    try {
        if (requested) {
            std::ofstream output(directory/"save-complete",std::ios::trunc);
            output << "game_saved\n";
        }
    } catch (...) { api->log(X4NATIVE_LOG_WARN,"host control: save acknowledgement unavailable"); }
}
X4NATIVE_EXPORT int x4native_api_version() { return X4NATIVE_API_VERSION; }
X4NATIVE_EXPORT int x4native_init(X4NativeAPI* context) {
    if (!context || context->api_version!=X4NATIVE_API_VERSION) return X4NATIVE_ERROR;
    api=context;
    const char* configured=std::getenv("X4_HOST_CONTROL_DIR");
    if (!configured || !*configured) return X4NATIVE_ERROR;
    try {
        directory=configured;
        if (!directory.is_absolute() || !std::filesystem::is_directory(directory)) return X4NATIVE_ERROR;
        frame_subscription=api->subscribe("on_frame_update",frame,nullptr,api);
        save_subscription=api->subscribe("x4_host.save_complete",saved,nullptr,api);
        return X4NATIVE_OK;
    } catch (...) { return X4NATIVE_ERROR; }
}
X4NATIVE_EXPORT void x4native_shutdown() {
    if (api) { api->unsubscribe(frame_subscription); api->unsubscribe(save_subscription); }
    api=nullptr; requested=false;
}
