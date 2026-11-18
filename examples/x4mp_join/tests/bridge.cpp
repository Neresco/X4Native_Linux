#include "../join_bridge.cpp"
#include <iostream>
#include <stdexcept>

static std::vector<std::string> messages;
static int lua_event(const char* name, const char* value) {
    messages.push_back(std::string(name) + "=" + value); return 0;
}
static void log_message(int, const char*) {}
static void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
int main() {
    X4NativeAPI context{}; context.raise_lua_event = lua_event; context.log = log_message;
    api = &context;
    // Test directory is supplied by CTest's disposable build directory.
    directory = std::filesystem::current_path() / "bridge-test-state";
    check(!std::filesystem::exists(directory), "preserve previous failed test state");
    std::filesystem::create_directory(directory);
    request(nullptr, const_cast<char*>("join-1|127.0.0.1"), nullptr);
    check(read("request.json") == "{\"id\":\"join-1\",\"ip\":\"127.0.0.1\"}\n", "request serialization");
    request(nullptr, const_cast<char*>("join-2|../../escape"), nullptr);
    check(read("request.json").find("join-1") != std::string::npos, "invalid request replaced valid request");
    cancel(nullptr, const_cast<char*>("join-1"), nullptr);
    check(read("cancel.json") == "{\"id\":\"join-1\"}\n", "cancel lost request identity");
    write("catalog.txt", "test|Test|127.0.0.1");
    write("status.txt", "join-1|syncing|50%||");
    poll(nullptr, nullptr, nullptr); poll(nullptr, nullptr, nullptr);
    check(messages.size() == 6, "reload poll must resend current session and state");
    write("status.txt", std::string(max_bytes + 1, 'x'));
    poll(nullptr, nullptr, nullptr);
    check(messages.size() == 8, "oversized status must not reach Lua");
    // Remove only the exact files this test owns; retain failures for inspection.
    for (auto name : {"request.json", "cancel.json", "catalog.txt", "status.txt"})
        std::filesystem::remove(directory / name);
    std::filesystem::remove(directory);
    std::cout << "PASS: native request/cancel, invalid input, bounded status and reload recovery\n";
}
