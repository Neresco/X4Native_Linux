// Small, platform-neutral UI transport. Snapshot/network work belongs to the
// background coordinator; game actions remain in the UI lifecycle.
#include "x4native_extension.h"
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <vector>

namespace {
X4NativeAPI* api = nullptr;
std::filesystem::path directory;
std::vector<int> subscriptions;
constexpr auto max_bytes = 16384u;

bool token(const std::string& value, size_t limit) {
    if (value.empty() || value.size() > limit) return false;
    for (unsigned char c : value)
        if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') ||
              (c >= 'a' && c <= 'z') || c == '.' || c == ':' || c == '-' || c == '_')) return false;
    return true;
}

std::string read(const char* name) {
    std::ifstream file(directory / name, std::ios::binary);
    if (!file) return {};
    std::string result(max_bytes + 1, '\0');
    file.read(result.data(), static_cast<std::streamsize>(result.size()));
    result.resize(static_cast<size_t>(file.gcount()));
    return result.size() <= max_bytes ? result : std::string{};
}

bool write(const char* name, const std::string& data) {
    // One UI producer and one coordinator consumer. Rename publishes a complete
    // message; the consumer claims it by rename before another request is made.
    auto temporary = directory / (std::string(name) + ".next");
    { std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
      if (!file || !(file << data) || !file.flush()) return false; }
    std::error_code error;
    std::filesystem::rename(temporary, directory / name, error);
    return !error;
}

void emit_error(const std::string& id, const char* message) {
    const auto data = id + "|error|" + message + "||";
    api->raise_lua_event("x4mp.join_flow.status", data.c_str());
}

void request(const char*, void* data, void*) {
    try {
        const std::string value = data ? static_cast<const char*>(data) : "";
        const auto split = value.find('|');
        if (split == std::string::npos) return;
        const auto id = value.substr(0, split), address = value.substr(split + 1);
        if (!token(id, 64) || !token(address, 253)) return;
        if (directory.empty()) { emit_error(id, "Join service is not configured."); return; }
        if (!write("request.json", "{\"id\":\"" + id + "\",\"ip\":\"" + address + "\"}\n"))
            emit_error(id, "Could not submit the join request. Please retry.");
    } catch (...) { api->log(X4NATIVE_LOG_ERROR, "x4mp_join: request failed"); }
}

void cancel(const char*, void* data, void*) {
    try {
        const std::string id = data ? static_cast<const char*>(data) : "";
        if (!directory.empty() && token(id, 64))
            write("cancel.json", "{\"id\":\"" + id + "\"}\n");
    } catch (...) { api->log(X4NATIVE_LOG_ERROR, "x4mp_join: cancellation failed"); }
}

void poll(const char*, void*, void*) {
    try {
        // A fresh coordinator directory identifies a new game-process session.
        // This is an opaque reset marker, never a credential or permission.
        const auto session = directory.empty() ? std::string("unconfigured") :
            std::to_string(std::hash<std::filesystem::path>{}(directory));
        api->raise_lua_event("x4mp.join_flow.session", session.c_str());
        if (directory.empty()) return;
        auto catalog = read("catalog.txt");
        auto status = read("status.txt");
        // Send complete current state on every explicit UI poll. This restores
        // state after a Lua/menu reload without relying on a worker callback.
        if (!catalog.empty()) api->raise_lua_event("x4mp.join_flow.servers", catalog.c_str());
        if (!status.empty()) api->raise_lua_event("x4mp.join_flow.status", status.c_str());
    } catch (...) { api->log(X4NATIVE_LOG_WARN, "x4mp_join: status unavailable"); }
}
}

X4NATIVE_EXPORT int x4native_api_version() { return X4NATIVE_API_VERSION; }
X4NATIVE_EXPORT int x4native_init(X4NativeAPI* context) {
    if (!context || context->api_version != X4NATIVE_API_VERSION ||
        !context->subscribe || !context->unsubscribe || !context->log ||
        !context->raise_lua_event) return X4NATIVE_ERROR;
    api = context;
    try {
#ifdef _WIN32
        const wchar_t* configured = _wgetenv(L"X4MP_JOIN_DIRECTORY");
#else
        const char* configured = std::getenv("X4MP_JOIN_DIRECTORY");
#endif
        if (configured && *configured) {
            directory = std::filesystem::path(configured);
            if (!directory.is_absolute() || !std::filesystem::is_directory(directory)) return X4NATIVE_ERROR;
        }
        subscriptions.push_back(api->subscribe("x4mp.join_flow.request", request, nullptr, api));
        subscriptions.push_back(api->subscribe("x4mp.join_flow.cancel", cancel, nullptr, api));
        subscriptions.push_back(api->subscribe("x4mp.join_flow.poll", poll, nullptr, api));
        api->log(X4NATIVE_LOG_INFO, "x4mp_join: UI bridge initialized");
        return X4NATIVE_OK;
    } catch (...) { return X4NATIVE_ERROR; }
}
X4NATIVE_EXPORT void x4native_shutdown() {
    if (api) for (auto id : subscriptions) if (id > 0) api->unsubscribe(id);
    subscriptions.clear(); directory.clear(); api = nullptr;
}
