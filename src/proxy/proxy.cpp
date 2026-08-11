// ---------------------------------------------------------------------------
// x4native_64.so — Thin Proxy Shared Library (Linux)
// ---------------------------------------------------------------------------

#include "lua_api.h"
#include "x4native_defs.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>
#include <mutex>
#include <dlfcn.h>
#include <unistd.h>
#include <linux/limits.h>
#include <filesystem>

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------
// Serializes ALL core load/reload/dlopen/dlclose/dlsym operations. X4 can
// re-enter luaopen_x4native() from worker threads (e.g. the "Movement worker"
// spawned when NewMultiplayerGame starts a universe). Without this lock, two
// threads can run load_core() concurrently: both copy_file() to the same
// x4native_core_live.so and dlopen() it while the other is still reading it,
// corrupting the module and causing a SIGSEGV inside dlsym()/ld-linux.
// recursive so reload_core() may call load_core() without deadlocking.
static std::recursive_mutex g_core_mutex;

static lua_State*       g_lua            = nullptr;
static void*            g_core_module    = nullptr;
static CoreDispatch     g_dispatch       = {};
static core_init_fn     g_core_init      = nullptr;
static core_shutdown_fn g_core_shutdown  = nullptr;
static std::string      g_ext_root;
static std::string      g_core_path;
static std::string      g_core_live_path;
static bool             g_initialized    = false;

// ---------------------------------------------------------------------------
// Core hot-reload state
// ---------------------------------------------------------------------------
#ifdef X4N_WITH_RELOAD
#include <fstream>
#include <nlohmann/json.hpp>

static bool g_autoreload_enabled   = false;
static bool g_autoreload_checked   = false;
static std::filesystem::file_time_type g_last_core_mtime;

static void read_autoreload_setting() {
    std::string path = g_ext_root + "x4native_settings.json";
    std::ifstream file(path);
    if (!file.is_open()) {
        if (g_dispatch.log) g_dispatch.log(2, "Core autoreload: settings file not found");
        return;
    }
    try {
        auto cfg = nlohmann::json::parse(file);
        if (cfg.contains("autoreload") && cfg["autoreload"].is_boolean())
            g_autoreload_enabled = cfg["autoreload"].get<bool>();
    } catch (...) {}
    if (g_dispatch.log)
        g_dispatch.log(1, g_autoreload_enabled
            ? "Core autoreload: ENABLED"
            : "Core autoreload: disabled");
}

static bool core_modified_since_last_check() {
    auto mtime = std::filesystem::last_write_time(g_core_path);
    if (g_last_core_mtime.time_since_epoch().count() == 0) {
        g_last_core_mtime = mtime;
        return false;
    }
    if (mtime > g_last_core_mtime) {
        g_last_core_mtime = mtime;
        if (g_dispatch.log) g_dispatch.log(1, "Autoreload: core .so modified on disk");
        return true;
    }
    return false;
}
#endif // X4N_WITH_RELOAD

// ---------------------------------------------------------------------------
// Stash
// ---------------------------------------------------------------------------
static std::unordered_map<std::string,
           std::unordered_map<std::string, std::vector<uint8_t>>> g_stash;
static std::mutex g_stash_mutex;

static int proxy_stash_set(const char* ns, const char* key,
                           const void* data, uint32_t size) {
    if (!ns || !key || (!data && size > 0)) return 0;
    std::lock_guard lock(g_stash_mutex);
    auto& entry = g_stash[ns][key];
    entry.assign(static_cast<const uint8_t*>(data),
                 static_cast<const uint8_t*>(data) + size);
    return 1;
}

static const void* proxy_stash_get(const char* ns, const char* key,
                                   uint32_t* out_size) {
    if (!ns || !key) return nullptr;
    std::lock_guard lock(g_stash_mutex);
    auto ns_it = g_stash.find(ns);
    if (ns_it == g_stash.end()) return nullptr;
    auto it = ns_it->second.find(key);
    if (it == ns_it->second.end()) return nullptr;
    if (out_size) *out_size = static_cast<uint32_t>(it->second.size());
    return it->second.data();
}

static int proxy_stash_remove(const char* ns, const char* key) {
    if (!ns || !key) return 0;
    std::lock_guard lock(g_stash_mutex);
    auto ns_it = g_stash.find(ns);
    if (ns_it == g_stash.end()) return 0;
    return ns_it->second.erase(key) > 0 ? 1 : 0;
}

static void proxy_stash_clear(const char* ns) {
    if (!ns) return;
    std::lock_guard lock(g_stash_mutex);
    g_stash.erase(ns);
}

// Forward declarations
static int proxy_raise_lua_event(const char* name, const char* param);
static int proxy_register_lua_bridge(const char* lua_event, const char* cpp_event);
static bool proxy_get_lua_property(const char* getter_fn, X4nLuaKey key,
                                   const char* field, X4nLuaValueType vt,
                                   void* out);
static bool proxy_get_lua_property_str(const char* getter_fn, X4nLuaKey key,
                                       const char* field,
                                       char* out_buf, size_t buf_size);

// ---------------------------------------------------------------------------
// Path helpers
// ---------------------------------------------------------------------------
static std::string detect_ext_root() {
    Dl_info info;
    if (dladdr((void*)&detect_ext_root, &info)) {
        std::string p(info.dli_fname);
        p = std::filesystem::canonical(p).string();
        auto pos = p.rfind("/native/");
        if (pos != std::string::npos)
            return p.substr(0, pos + 1);
        pos = p.rfind('/');
        return (pos != std::string::npos) ? p.substr(0, pos + 1) : p;
    }
    return ".";
}

// ---------------------------------------------------------------------------
// Core .so loading
// ---------------------------------------------------------------------------
static bool load_core() {
    std::lock_guard<std::recursive_mutex> lock(g_core_mutex);
    // Load the ORIGINAL core .so directly. We deliberately do NOT copy it to a
    // shared "live" path first: multiple proxy instances (X4 can dlopen the
    // proxy several times / re-enter it from worker threads) all used to
    // copy_file() to the SAME x4native_core_live.so concurrently, corrupting
    // the file while another thread was dlopen()ing it -> SIGSEGV inside
    // dlsym()/ld-linux when NewMultiplayerGame spawns a "Movement worker" that
    // re-enters luaopen_x4native(). dlopen() of the original path is safe
    // (glibc refcounts the same inode; no file is rewritten).
    g_core_module = dlopen(g_core_path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!g_core_module) {
        fprintf(stderr, "X4Native proxy: dlopen failed for core .so: %s\n", dlerror());
        return false;
    }

    g_core_init     = reinterpret_cast<core_init_fn>(
                           dlsym(g_core_module, "core_init"));
    g_core_shutdown = reinterpret_cast<core_shutdown_fn>(
                           dlsym(g_core_module, "core_shutdown"));

    if (!g_core_init) {
        fprintf(stderr, "X4Native proxy: core_init export not found\n");
        dlclose(g_core_module);
        g_core_module = nullptr;
        return false;
    }

    CoreInitContext ctx = {};
    ctx.lua_state = g_lua;
    ctx.ext_root  = g_ext_root.c_str();
    ctx.dispatch  = &g_dispatch;
    ctx.raise_lua_event = proxy_raise_lua_event;
    ctx.register_lua_bridge = proxy_register_lua_bridge;
    ctx.stash_set    = proxy_stash_set;
    ctx.stash_get    = proxy_stash_get;
    ctx.stash_remove = proxy_stash_remove;
    ctx.stash_clear  = proxy_stash_clear;
    ctx.get_lua_property     = proxy_get_lua_property;
    ctx.get_lua_property_str = proxy_get_lua_property_str;

    if (g_core_init(&ctx) != 0) {
        fprintf(stderr, "X4Native proxy: core_init returned error\n");
        dlclose(g_core_module);
        g_core_module = nullptr;
        return false;
    }

#ifdef X4N_WITH_RELOAD
    read_autoreload_setting();
#endif

    return true;
}

static bool reload_core() {
    std::lock_guard<std::recursive_mutex> lock(g_core_mutex);
    if (g_dispatch.prepare_reload)
        g_dispatch.prepare_reload();

    if (g_core_module) {
        if (g_core_shutdown)
            g_core_shutdown();
        dlclose(g_core_module);
        g_core_module = nullptr;
    }

    g_dispatch      = {};
    g_core_init     = nullptr;
    g_core_shutdown = nullptr;

    return load_core();
}

static bool first_load_guarded() noexcept {
    try {
        g_ext_root       = detect_ext_root();
        g_core_path      = g_ext_root + "native/x4native_core.so";
        g_core_live_path = g_ext_root + "native/x4native_core_live.so";
        return load_core();
    } catch (...) {
        fprintf(stderr, "X4Native proxy: exception during first core load\n");
        return false;
    }
}

static bool reload_core_guarded() noexcept {
    try {
        return reload_core();
    } catch (...) {
        fprintf(stderr, "X4Native proxy: exception during core reload\n");
        return false;
    }
}

static bool core_needs_reload() {
    // With the proxy now dlopen()ing the original core path directly, there is
    // no separate live copy to compare against. Auto-reload is off by default
    // (x4native_settings.json autoreload=false); manual reload via the Lua
    // reload() API still works. So never force a reload on re-entry.
    (void)g_core_live_path;
    return false;
}

// ---------------------------------------------------------------------------
// Lua-facing API functions
// ---------------------------------------------------------------------------
static int l_discover_extensions(lua_State* L) {
    if (g_dispatch.discover_extensions)
        g_dispatch.discover_extensions();
    return 0;
}

static int l_raise_event(lua_State* L) {
    const char* ev = x4n::lua::L_checkstring(L, 1);
    const char* param = nullptr;
    if (x4n::lua::gettop(L) >= 2 && x4n::lua::type(L, 2) == LUA_TSTRING)
        param = x4n::lua::tostring(L, 2);
    if (g_dispatch.raise_event)
        g_dispatch.raise_event(ev, param);
    return 0;
}

static int proxy_raise_lua_event(const char* name, const char* param) {
    if (!g_lua) return -1;
    x4n::lua::getfield(g_lua, LUA_GLOBALSINDEX, "CallEventScripts");
    x4n::lua::pushstring(g_lua, name);
    if (param) x4n::lua::pushstring(g_lua, param);
    else       x4n::lua::pushnil(g_lua);
    return x4n::lua::pcall(g_lua, 2, 0, 0);
}

static bool prepare_call(int top, const char* getter_fn,
                          const X4nLuaKey& key, const char* field) {
    if (key.type == X4N_KEY_UINT64) {
        x4n::lua::getfield(g_lua, LUA_GLOBALSINDEX, "ConvertStringToLuaID");
        if (x4n::lua::type(g_lua, -1) != LUA_TFUNCTION) {
            x4n::lua::settop(g_lua, top);
            return false;
        }
        char id_str[24];
        snprintf(id_str, sizeof(id_str), "%llu",
                 (unsigned long long)key.v.u);
        x4n::lua::pushstring(g_lua, id_str);
        if (x4n::lua::pcall(g_lua, 1, 1, 0) != 0) {
            x4n::lua::settop(g_lua, top);
            return false;
        }
    }

    x4n::lua::getfield(g_lua, LUA_GLOBALSINDEX, getter_fn);
    if (x4n::lua::type(g_lua, -1) != LUA_TFUNCTION) {
        x4n::lua::settop(g_lua, top);
        return false;
    }

    if (key.type == X4N_KEY_UINT64) {
        x4n::lua::insert(g_lua, -2);
    } else {
        x4n::lua::pushstring(g_lua, key.v.s ? key.v.s : "");
    }
    x4n::lua::pushstring(g_lua, field);
    return true;
}

static bool proxy_get_lua_property(const char* getter_fn, X4nLuaKey key,
                                   const char* field, X4nLuaValueType vt,
                                   void* out) {
    if (!g_lua || !getter_fn || !field || !out) return false;
    int top = x4n::lua::gettop(g_lua);
    if (!prepare_call(top, getter_fn, key, field)) return false;

    bool ok = false;
    if (x4n::lua::pcall(g_lua, 2, 1, 0) == 0) {
        int t = x4n::lua::type(g_lua, -1);
        switch (vt) {
            case X4N_VAL_INT64:
                if (t == LUA_TNUMBER) {
                    *static_cast<int64_t*>(out) =
                        static_cast<int64_t>(x4n::lua::tointeger(g_lua, -1));
                    ok = true;
                }
                break;
            case X4N_VAL_DOUBLE:
                if (t == LUA_TNUMBER) {
                    *static_cast<double*>(out) =
                        static_cast<double>(x4n::lua::tonumber(g_lua, -1));
                    ok = true;
                }
                break;
            case X4N_VAL_BOOL:
                if (t == LUA_TBOOLEAN) {
                    *static_cast<bool*>(out) =
                        x4n::lua::toboolean(g_lua, -1) != 0;
                    ok = true;
                }
                break;
        }
    }
    x4n::lua::settop(g_lua, top);
    return ok;
}

static bool proxy_get_lua_property_str(const char* getter_fn, X4nLuaKey key,
                                       const char* field, char* out_buf,
                                       size_t buf_size) {
    if (!g_lua || !getter_fn || !field || !out_buf || buf_size == 0)
        return false;
    int top = x4n::lua::gettop(g_lua);
    if (!prepare_call(top, getter_fn, key, field)) return false;

    bool ok = false;
    if (x4n::lua::pcall(g_lua, 2, 1, 0) == 0) {
        if (x4n::lua::type(g_lua, -1) == LUA_TSTRING) {
            size_t len = 0;
            const char* s = x4n::lua::tolstring(g_lua, -1, &len);
            if (s && len + 1 <= buf_size) {
                std::memcpy(out_buf, s, len);
                out_buf[len] = '\0';
                ok = true;
            }
        }
    }
    x4n::lua::settop(g_lua, top);
    return ok;
}

// ---------------------------------------------------------------------------
// Dynamic Lua→C++ event bridge
// ---------------------------------------------------------------------------
static std::unordered_map<std::string, std::string> g_lua_bridges;

static int bridge_handler(lua_State* L) {
    const char* cpp_event = x4n::lua::tostring(L, lua_upvalueindex(1));
    if (cpp_event && g_dispatch.raise_event) {
        const char* param = nullptr;
        if (x4n::lua::gettop(L) >= 2 && x4n::lua::type(L, 2) == LUA_TSTRING)
            param = x4n::lua::tostring(L, 2);
        g_dispatch.raise_event(cpp_event, param);
    }
    return 0;
}

static int proxy_register_lua_bridge(const char* lua_event, const char* cpp_event) {
    if (!g_lua || !lua_event || !cpp_event) return -1;
    if (g_lua_bridges.count(lua_event)) return 0;

    x4n::lua::getfield(g_lua, LUA_GLOBALSINDEX, "RegisterEvent");
    x4n::lua::pushstring(g_lua, lua_event);
    x4n::lua::pushstring(g_lua, cpp_event);
    x4n::lua::pushcclosure(g_lua, bridge_handler, 1);
    int err = x4n::lua::pcall(g_lua, 2, 0, 0);

    if (err == 0) {
        g_lua_bridges[lua_event] = cpp_event;
        if (g_dispatch.log)
            g_dispatch.log(1, (std::string("Lua bridge: registered '") +
                               lua_event + "' -> '" + cpp_event + "'").c_str());
    }
    return err;
}

static int l_raise_lua_event(lua_State* L) {
    const char* name  = x4n::lua::L_checkstring(L, 1);
    const char* param = nullptr;
    if (x4n::lua::gettop(L) >= 2 && x4n::lua::type(L, 2) == LUA_TSTRING)
        param = x4n::lua::tostring(L, 2);
    x4n::lua::pushinteger(L, proxy_raise_lua_event(name, param));
    return 1;
}

static int l_log(lua_State* L) {
    int level = static_cast<int>(x4n::lua::tointeger(L, 1));
    const char* msg = x4n::lua::L_checkstring(L, 2);
    if (g_dispatch.log)
        g_dispatch.log(level, msg);
    return 0;
}

static int l_get_version(lua_State* L) {
    const char* v = g_dispatch.get_version
                        ? g_dispatch.get_version()
                        : "unknown";
    x4n::lua::pushstring(L, v);
    return 1;
}

static int l_get_loaded_extensions(lua_State* L) {
    const char* j = g_dispatch.get_loaded_extensions
                        ? g_dispatch.get_loaded_extensions()
                        : "[]";
    x4n::lua::pushstring(L, j);
    return 1;
}

static int l_reload(lua_State* L) {
    x4n::lua::pushboolean(L, reload_core_guarded() ? 1 : 0);
    return 1;
}

static int l_prepare_reload(lua_State* L) {
    if (g_dispatch.prepare_reload)
        g_dispatch.prepare_reload();
    return 0;
}

// ---------------------------------------------------------------------------
// Per-extension settings — Lua marshalling
// ---------------------------------------------------------------------------
static void push_setting_row(lua_State* L, const SettingInfo& info) {
    x4n::lua::createtable(L, 0, 8);

    x4n::lua::pushstring(L, info.id ? info.id : "");
    x4n::lua::setfield(L, -2, "id");
    x4n::lua::pushstring(L, info.name ? info.name : "");
    x4n::lua::setfield(L, -2, "name");

    const char* type_str = "toggle";
    if      (info.type == X4N_SETTING_DROPDOWN) type_str = "dropdown";
    else if (info.type == X4N_SETTING_SLIDER)   type_str = "slider";
    x4n::lua::pushstring(L, type_str);
    x4n::lua::setfield(L, -2, "type");

    switch (info.type) {
        case X4N_SETTING_TOGGLE:
            x4n::lua::pushboolean(L, info.current_bool);
            x4n::lua::setfield(L, -2, "current");
            x4n::lua::pushboolean(L, info.default_bool);
            x4n::lua::setfield(L, -2, "default");
            break;
        case X4N_SETTING_SLIDER:
            x4n::lua::pushnumber(L, info.current_number);
            x4n::lua::setfield(L, -2, "current");
            x4n::lua::pushnumber(L, info.default_number);
            x4n::lua::setfield(L, -2, "default");
            x4n::lua::pushnumber(L, info.min);
            x4n::lua::setfield(L, -2, "min");
            x4n::lua::pushnumber(L, info.max);
            x4n::lua::setfield(L, -2, "max");
            x4n::lua::pushnumber(L, info.step);
            x4n::lua::setfield(L, -2, "step");
            break;
        case X4N_SETTING_DROPDOWN:
            x4n::lua::pushstring(L, info.current_string ? info.current_string : "");
            x4n::lua::setfield(L, -2, "current");
            x4n::lua::pushstring(L, info.default_string ? info.default_string : "");
            x4n::lua::setfield(L, -2, "default");
            x4n::lua::createtable(L, info.option_count, 0);
            for (int i = 0; i < info.option_count; ++i) {
                x4n::lua::createtable(L, 0, 2);
                x4n::lua::pushstring(L, info.options[i].id ? info.options[i].id : "");
                x4n::lua::setfield(L, -2, "id");
                x4n::lua::pushstring(L, info.options[i].text ? info.options[i].text : "");
                x4n::lua::setfield(L, -2, "text");
                x4n::lua::rawseti(L, -2, i + 1);
            }
            x4n::lua::setfield(L, -2, "options");
            break;
    }
}

static int l_get_extension_settings(lua_State* L) {
    const char* ext_id = x4n::lua::L_checkstring(L, 1);
    const SettingInfo* entries = nullptr;
    int n = 0;
    if (g_dispatch.enumerate_settings)
        n = g_dispatch.enumerate_settings(ext_id, &entries);
    x4n::lua::createtable(L, n, 0);
    for (int i = 0; i < n; ++i) {
        push_setting_row(L, entries[i]);
        x4n::lua::rawseti(L, -2, i + 1);
    }
    return 1;
}

static int l_set_extension_setting(lua_State* L) {
    const char* ext_id = x4n::lua::L_checkstring(L, 1);
    const char* key    = x4n::lua::L_checkstring(L, 2);

    SettingValueC v = {};
    int t = x4n::lua::type(L, 3);
    if (t == LUA_TBOOLEAN) {
        v.type = X4N_SETTING_TOGGLE;
        v.b    = x4n::lua::toboolean(L, 3);
    } else if (t == LUA_TNUMBER) {
        v.type = X4N_SETTING_SLIDER;
        v.d    = static_cast<double>(x4n::lua::tonumber(L, 3));
    } else if (t == LUA_TSTRING) {
        v.type = X4N_SETTING_DROPDOWN;
        v.s    = x4n::lua::tostring(L, 3);
    } else {
        return x4n::lua::L_error(L,
            "set_extension_setting: value must be boolean, number, or string");
    }

    if (g_dispatch.set_extension_setting)
        g_dispatch.set_extension_setting(ext_id, key, &v);
    return 0;
}

#ifdef X4N_WITH_RELOAD
static int l_should_autoreload(lua_State* L) {
    if (!g_autoreload_checked) {
        read_autoreload_setting();
        g_autoreload_checked = true;
    }
    if (!g_autoreload_enabled) {
        x4n::lua::pushboolean(L, 0);
        return 1;
    }
    x4n::lua::pushboolean(L, core_modified_since_last_check() ? 1 : 0);
    return 1;
}
#endif // X4N_WITH_RELOAD

// ---------------------------------------------------------------------------
// Lua C-function exception guard
// ---------------------------------------------------------------------------
template <int (*Fn)(lua_State*)>
static int lua_guarded(lua_State* L) {
    try {
        return Fn(L);
    } catch (...) {
        fprintf(stderr, "X4Native proxy: exception in Lua API call\n");
        return 0;
    }
}

// ---------------------------------------------------------------------------
// Entry point — called by Lua: package.loadlib("...so", "luaopen_x4native")
// ---------------------------------------------------------------------------
extern "C" __attribute__((visibility("default")))
int luaopen_x4native(lua_State* L) {
    g_lua = L;

    if (!x4n::lua::resolve()) {
        fprintf(stderr, "X4Native: FATAL — failed to resolve Lua API\n");
        return 0;
    }

    if (!g_initialized) {
        if (!first_load_guarded())
            return x4n::lua::L_error(L, "X4Native: failed to load core .so");
        g_initialized = true;
    } else {
        if (g_dispatch.set_lua_state)
            g_dispatch.set_lua_state(L);
        g_lua_bridges.clear();
        if (core_needs_reload())
            reload_core_guarded();
    }

    x4n::lua::newtable(L);

    struct { const char* name; lua_CFunction fn; } funcs[] = {
        { "discover_extensions",     lua_guarded<l_discover_extensions>    },
        { "raise_event",             lua_guarded<l_raise_event>            },
        { "raise_lua_event",         lua_guarded<l_raise_lua_event>        },
        { "log",                     lua_guarded<l_log>                    },
        { "get_version",             lua_guarded<l_get_version>            },
        { "get_loaded_extensions",   lua_guarded<l_get_loaded_extensions>  },
        { "reload",                  lua_guarded<l_reload>                 },
        { "prepare_reload",          lua_guarded<l_prepare_reload>         },
        { "get_extension_settings",  lua_guarded<l_get_extension_settings> },
        { "set_extension_setting",   lua_guarded<l_set_extension_setting>  },
#ifdef X4N_WITH_RELOAD
        { "should_autoreload",       lua_guarded<l_should_autoreload>      },
#endif
    };

    for (auto& f : funcs) {
        x4n::lua::pushcfunction(L, f.fn);
        x4n::lua::setfield(L, -2, f.name);
    }

    return 1;
}

// ---------------------------------------------------------------------------
// No DllMain needed on Linux — constructor/destructor attributes if needed
// ---------------------------------------------------------------------------
__attribute__((destructor))
static void proxy_destructor() {
    std::lock_guard<std::recursive_mutex> lock(g_core_mutex);
    if (g_core_shutdown)
        g_core_shutdown();
    if (g_core_module) {
        dlclose(g_core_module);
        g_core_module = nullptr;
    }
}