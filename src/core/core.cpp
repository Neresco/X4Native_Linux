// ---------------------------------------------------------------------------
// x4native_core.so — Core Entry Point (Linux)
// ---------------------------------------------------------------------------

#include "logger.h"
#include "event_system.h"
#include "extension_manager.h"
#include "game_api.h"
#include "hook_manager.h"
#include "settings_manager.h"
#include "version.h"
#include "x4native_defs.h"

#include <x4_game_func_table.h>
#include <x4_game_offsets.h>
#include <x4_manual_types.h>
#ifdef _WIN32
#include <MinHook.h>
#else
#include "common/minhook_shim.h"
#endif

#include <array>
#include <cstdio>
#include <string>
#include <cmath>
#include <csignal>
#include <dlfcn.h>

// ---------------------------------------------------------------------------
// Module-level state
// ---------------------------------------------------------------------------
X4GameOffsets s_offsets = {
    .frame_game_time       = nullptr,
    .frame_raw_time        = nullptr,
    .frame_real_time       = nullptr,
    .frame_speed_mult      = nullptr,
    .component_registry    = nullptr,
    .faction_registry      = nullptr,
    .game_universe         = nullptr,
    .session_seed          = nullptr,
    .construction_plan_db  = nullptr,
    .macro_registry        = nullptr,
    .radar_event_vtable    = nullptr,

    .vtable_get_class_type   = X4_VTABLE_GET_CLASS_TYPE / 8,
    .vtable_get_class_id     = X4_VTABLE_GET_CLASS_ID / 8,
    .vtable_is_class_id      = X4_VTABLE_IS_CLASS_ID / 8,
    .vtable_is_derived_class = X4_VTABLE_IS_DERIVED_CLASS / 8,
    .vtable_get_id_code      = X4_VTABLE_GET_ID_CODE / 8,
    .vtable_set_world_xform  = X4_VTABLE_SET_WORLD_XFORM / 8,
    .vtable_set_position     = X4_VTABLE_SET_POSITION / 8,
    .vtable_destroy          = X4_VTABLE_DESTROY / 8,
    .vtable_get_faction_id   = X4_VTABLE_GET_FACTION_ID / 8,

    .enginectx_frame_counter = X4_ENGINECTX_OFFSET_FRAME_COUNTER,
    .enginectx_fps_timer     = X4_ENGINECTX_OFFSET_FPS_TIMER,
    .enginectx_fps           = X4_ENGINECTX_OFFSET_FPS,

    .component_id            = X4_COMPONENT_OFFSET_ID,
    .component_definition    = X4_COMPONENT_OFFSET_DEFINITION,
    .component_parent        = X4_COMPONENT_OFFSET_PARENT,
    .component_children      = X4_COMPONENT_OFFSET_CHILDREN,
    .component_exists        = X4_COMPONENT_OFFSET_EXISTS,
    .component_combined_seed = X4_COMPONENT_OFFSET_COMBINED_SEED,

    .container_spawntime         = X4_CONTAINER_OFFSET_SPAWNTIME,
    .space_has_sunlight          = X4_SPACE_OFFSET_HAS_SUNLIGHT,
    .space_sunlight              = X4_SPACE_OFFSET_SUNLIGHT,
    .game_universe_galaxy_offset = X4_GAME_UNIVERSE_GALAXY_OFFSET,

    .object_owner_faction_ptr    = X4_OBJECT_OFFSET_OWNER_FACTION_PTR,
    .object_known_read           = X4_OBJECT_OFFSET_KNOWN_READ,
    .object_known_to_all         = X4_OBJECT_OFFSET_KNOWN_TO_ALL,
    .object_known_factions_arr   = X4_OBJECT_OFFSET_KNOWN_FACTIONS_ARR,
    .object_known_factions_cap   = X4_OBJECT_OFFSET_KNOWN_FACTIONS_CAP,
    .object_known_factions_count = X4_OBJECT_OFFSET_KNOWN_FACTIONS_COUNT,
    .object_liveview_local       = X4_OBJECT_OFFSET_LIVEVIEW_LOCAL,
    .object_liveview_monitor     = X4_OBJECT_OFFSET_LIVEVIEW_MONITOR,
    .object_masstraffic_queue    = X4_OBJECT_OFFSET_MASSTRAFFIC_QUEUE,
    .object_radar_visible        = X4_OBJECT_OFFSET_RADAR_VISIBLE,
    .object_forced_radar_visible = X4_OBJECT_OFFSET_FORCED_RADAR_VISIBLE,

    .space_owner_faction_ptr     = X4_SPACE_OFFSET_OWNER_FACTION_PTR,
    .space_known_read            = X4_SPACE_OFFSET_KNOWN_READ,
    .space_known_to_all          = X4_SPACE_OFFSET_KNOWN_TO_ALL,
    .space_known_factions_arr    = X4_SPACE_OFFSET_KNOWN_FACTIONS_ARR,
    .space_known_factions_cap    = X4_SPACE_OFFSET_KNOWN_FACTIONS_CAP,
    .space_known_factions_count  = X4_SPACE_OFFSET_KNOWN_FACTIONS_COUNT,

    .sector_resarea_vec_begin    = X4_SECTOR_RESAREA_VEC_BEGIN,
    .sector_resarea_vec_end      = X4_SECTOR_RESAREA_VEC_END,

    .macrodata_connections_begin   = X4_MACRODATA_OFFSET_CONNECTIONS_BEGIN,
    .macrodata_connections_end     = X4_MACRODATA_OFFSET_CONNECTIONS_END,
    .connection_entry_size         = X4_CONNECTION_ENTRY_SIZE,
    .connection_offset_hash        = X4_CONNECTION_OFFSET_HASH,
    .connection_offset_name        = X4_CONNECTION_OFFSET_NAME,
    .macrodefaults_room_conn_begin = X4_MACRODEFAULTS_OFFSET_ROOM_CONNECTIONS_BEGIN,
    .macrodefaults_room_conn_end   = X4_MACRODEFAULTS_OFFSET_ROOM_CONNECTIONS_END,

    .radar_event_entity_id = X4_RADAR_EVENT_OFFSET_ENTITY_ID,
    .radar_event_visible   = X4_RADAR_EVENT_OFFSET_VISIBLE,

    .room_roomtype   = X4_ROOM_OFFSET_ROOMTYPE,
    .room_name       = X4_ROOM_OFFSET_NAME,
    .room_private    = X4_ROOM_OFFSET_PRIVATE,
    .room_persistent = X4_ROOM_OFFSET_PERSISTENT,
};

static std::string g_ext_root;
static std::string g_game_version;
static std::string g_version_string;
static raise_lua_event_fn g_raise_lua_event = nullptr;
static register_lua_bridge_fn g_register_lua_bridge = nullptr;

static stash_set_fn    g_stash_set    = nullptr;
static stash_get_fn    g_stash_get    = nullptr;
static stash_remove_fn g_stash_remove = nullptr;
static stash_clear_fn  g_stash_clear  = nullptr;

get_lua_property_fn     g_get_lua_property     = nullptr;
get_lua_property_str_fn g_get_lua_property_str = nullptr;

// ---------------------------------------------------------------------------
// Resolve pointer fields in s_offsets
// ---------------------------------------------------------------------------
static void resolve_offset_pointers(uintptr_t base) {
    s_offsets.frame_game_time      = reinterpret_cast<double*>(base + X4_RVA_FRAME_GAME_TIME);
    s_offsets.frame_raw_time       = reinterpret_cast<double*>(base + X4_RVA_FRAME_RAW_TIME);
    s_offsets.frame_real_time      = reinterpret_cast<double*>(base + X4_RVA_FRAME_REAL_TIME);
    s_offsets.frame_speed_mult     = reinterpret_cast<double*>(base + X4_RVA_FRAME_SPEED_MULT);
    s_offsets.component_registry   = reinterpret_cast<void*>(base + X4_RVA_COMPONENT_REGISTRY);
    s_offsets.faction_registry     = reinterpret_cast<void*>(base + X4_RVA_FACTION_REGISTRY);
    s_offsets.game_universe        = reinterpret_cast<void*>(base + X4_RVA_GAME_UNIVERSE);
    s_offsets.session_seed         = reinterpret_cast<uint64_t*>(base + X4_RVA_SESSION_SEED);
    s_offsets.construction_plan_db = reinterpret_cast<void*>(base + X4_RVA_CONSTRUCTION_PLAN_DB);
    s_offsets.macro_registry       = reinterpret_cast<void*>(base + X4_RVA_MACRO_REGISTRY);
    s_offsets.radar_event_vtable   = reinterpret_cast<void*>(base + X4_RADAR_EVENT_VTABLE_RVA);
}

// ---------------------------------------------------------------------------
// Native frame tick hook
// ---------------------------------------------------------------------------
static uintptr_t g_x4_base = 0;
static void*     g_frame_tick_trampoline = nullptr;

using FrameTickFn = void(__attribute__((fastcall))*)(void*, bool);

static void __attribute__((fastcall)) frame_tick_detour(void* engineCtx, bool isSuspended) {
    double raw_time_before = *s_offsets.frame_raw_time;

    reinterpret_cast<FrameTickFn>(g_frame_tick_trampoline)(engineCtx, isSuspended);

    double raw_time_after = *s_offsets.frame_raw_time;
    double delta = raw_time_after - raw_time_before;
    if (delta < 0.0) delta = 0.0;

    X4NativeFrameUpdate update{};
    update.delta            = delta;
    update.game_time        = *s_offsets.frame_game_time;
    update.real_time        = *s_offsets.frame_real_time;
    update.fps              = *(float*)((uintptr_t)engineCtx + s_offsets.enginectx_fps);
    update.speed_multiplier = (float)*s_offsets.frame_speed_mult;
    update.is_suspended     = isSuspended;
    update.frame_counter    = *(int*)((uintptr_t)engineCtx + s_offsets.enginectx_frame_counter);

    auto* table = x4n::GameAPI::table();
    update.game_paused = (table && table->IsGamePaused) ? table->IsGamePaused() : false;

    x4n::ExtensionManager::tick();
    x4n::ExtensionManager::flush_pending_reloads();

    x4n::EventSystem::fire("on_native_frame_update", &update);
}

static bool install_frame_tick_hook() {
    void* target = x4n::GameAPI::get_internal("X4_FrameTick");
    if (!target) {
        x4n::Logger::warn("Native frame hook: X4_FrameTick not resolved");
        return false;
    }

    // Linux: get base address of main executable
    Dl_info info;
    if (dladdr((void*)&frame_tick_detour, &info)) {
        g_x4_base = reinterpret_cast<uintptr_t>(info.dli_fbase);
    }
    if (!g_x4_base) return false;

    MH_STATUS status = MH_CreateHook(target, reinterpret_cast<void*>(&frame_tick_detour), &g_frame_tick_trampoline);
    if (status != MH_OK) {
        x4n::Logger::error("Native frame hook: MH_CreateHook failed: {}", MH_StatusToString(status));
        return false;
    }

    status = MH_EnableHook(target);
    if (status != MH_OK) {
        x4n::Logger::error("Native frame hook: MH_EnableHook failed: {}", MH_StatusToString(status));
        MH_RemoveHook(target);
        return false;
    }

    x4n::Logger::info("Native frame hook installed (on_native_frame_update)");
    return true;
}

static void remove_frame_tick_hook() {
    void* target = x4n::GameAPI::get_internal("X4_FrameTick");
    if (target && g_frame_tick_trampoline) {
        MH_DisableHook(target);
        MH_RemoveHook(target);
        g_frame_tick_trampoline = nullptr;
    }
}

// ---------------------------------------------------------------------------
// Radar visibility hook (OBSOLETE)
// ---------------------------------------------------------------------------
static void* g_radar_event_trampoline = nullptr;

using RadarEventBuildFn = void*(__attribute__((fastcall))*)(void*);

static void* __attribute__((fastcall)) radar_event_detour(void* property_data) {
    void* event = reinterpret_cast<RadarEventBuildFn>(g_radar_event_trampoline)(property_data);
    if (!event) return event;

    auto addr = reinterpret_cast<uintptr_t>(event);
    X4RadarChangedEvent payload{};
    payload.entity_id = *reinterpret_cast<uint64_t*>(addr + s_offsets.radar_event_entity_id);
    payload.visible   = *reinterpret_cast<uint8_t*>(addr + s_offsets.radar_event_visible);

    x4n::EventSystem::fire("on_radar_changed", &payload);
    return event;
}

static bool install_radar_visibility_hook() {
    void* target = x4n::GameAPI::get_internal("RadarVisibilityChanged_BuildEvent");
    if (!target) {
        x4n::Logger::warn("Radar visibility hook: not resolved");
        return false;
    }

    MH_STATUS status = MH_CreateHook(target, reinterpret_cast<void*>(&radar_event_detour), &g_radar_event_trampoline);
    if (status != MH_OK) {
        x4n::Logger::error("Radar visibility hook: MH_CreateHook failed: {}", MH_StatusToString(status));
        return false;
    }

    status = MH_EnableHook(target);
    if (status != MH_OK) {
        x4n::Logger::error("Radar visibility hook: MH_EnableHook failed: {}", MH_StatusToString(status));
        MH_RemoveHook(target);
        return false;
    }

    x4n::Logger::info("Radar visibility hook installed (on_radar_changed)");
    return true;
}

static void remove_radar_visibility_hook() {
    void* target = x4n::GameAPI::get_internal("RadarVisibilityChanged_BuildEvent");
    if (target && g_radar_event_trampoline) {
        MH_DisableHook(target);
        MH_RemoveHook(target);
        g_radar_event_trampoline = nullptr;
    }
}

// ---------------------------------------------------------------------------
// MD event dispatch hook
// ---------------------------------------------------------------------------
static void* g_md_event_trampoline = nullptr;

static void __attribute__((fastcall)) md_event_dispatch_detour(
    void* event_source, void* event_object, double timestamp, char immediate)
{
    uint32_t type_id = UINT32_MAX;

    if (event_object) {
        // Read type ID from vtable[1]
        volatile uint32_t safe_type = UINT32_MAX;
        __builtin_prefetch(event_object);
        if (__builtin_expect(!__builtin_expect(0, 0), 1)) {
            auto vtable = *reinterpret_cast<void***>(event_object);
            auto fn_bytes = reinterpret_cast<uint8_t*>(vtable[1]);
            if (fn_bytes[0] == 0xB8 && fn_bytes[5] == 0xC3) {
                safe_type = *reinterpret_cast<uint32_t*>(fn_bytes + 1);
            }
        }
        type_id = safe_type;
    }

    if (type_id < x4n::EventSystem::MAX_MD_TYPE) {
        uint64_t source_id = event_source ?
            *reinterpret_cast<uint64_t*>(reinterpret_cast<uintptr_t>(event_source) + 8) : 0;
        X4MdEvent payload{type_id, source_id, timestamp, event_object};
        x4n::EventSystem::md_fire_before(type_id, &payload);
    }

    using OrigFn = void(__attribute__((fastcall))*)(void*, void*, double, char);
    reinterpret_cast<OrigFn>(g_md_event_trampoline)(
        event_source, event_object, timestamp, immediate);

    if (type_id < x4n::EventSystem::MAX_MD_TYPE) {
        uint64_t source_id = event_source ?
            *reinterpret_cast<uint64_t*>(reinterpret_cast<uintptr_t>(event_source) + 8) : 0;
        X4MdEvent payload{type_id, source_id, timestamp, event_object};
        x4n::EventSystem::md_fire_after(type_id, &payload);
    }
}

static bool install_md_event_hook() {
    auto* table = x4n::GameAPI::table();
    void* target = table ? reinterpret_cast<void*>(table->EventQueue_InsertOrDispatch) : nullptr;
    if (!target) {
        x4n::Logger::warn("MD event hook: EventQueue_InsertOrDispatch not resolved");
        return false;
    }

    MH_STATUS status = MH_CreateHook(target, reinterpret_cast<void*>(&md_event_dispatch_detour), &g_md_event_trampoline);
    if (status != MH_OK) {
        x4n::Logger::error("MD event hook: MH_CreateHook failed: {}", MH_StatusToString(status));
        return false;
    }

    status = MH_EnableHook(target);
    if (status != MH_OK) {
        x4n::Logger::error("MD event hook: MH_EnableHook failed: {}", MH_StatusToString(status));
        MH_RemoveHook(target);
        return false;
    }

    x4n::Logger::info("MD event hook installed (on_md_before/on_md_after, {} type slots)",
                      x4n::EventSystem::MAX_MD_TYPE);
    return true;
}

static void remove_md_event_hook() {
    auto* table = x4n::GameAPI::table();
    void* target = table ? reinterpret_cast<void*>(table->EventQueue_InsertOrDispatch) : nullptr;
    if (target && g_md_event_trampoline) {
        MH_DisableHook(target);
        MH_RemoveHook(target);
        g_md_event_trampoline = nullptr;
    }
}

// ---------------------------------------------------------------------------
// Dispatch implementations
// ---------------------------------------------------------------------------
static void impl_discover_extensions() {
    x4n::ExtensionManager::discover();
    x4n::ExtensionManager::load_all();
}

static void impl_raise_event(const char* event_name, const char* param) {
    x4n::EventSystem::fire(event_name, const_cast<char*>(param));
}

static const char* impl_get_version() {
    return g_version_string.c_str();
}

static const char* impl_get_loaded_extensions() {
    return x4n::ExtensionManager::loaded_extensions_json();
}

static void impl_set_lua_state(void* /*L*/) {
    x4n::Logger::info("Lua state updated (UI reload)");
}

static void impl_log(int level, const char* message) {
    auto lv = static_cast<x4n::LogLevel>(level);
    x4n::Logger::write(lv, message);
}

static void impl_prepare_reload() {
    x4n::Logger::info("Preparing for core hot-reload...");
    x4n::EventSystem::fire("on_before_reload");
    x4n::ExtensionManager::shutdown();
    remove_md_event_hook();
    remove_radar_visibility_hook();
    remove_frame_tick_hook();
    x4n::HookManager::remove_all();
}

static void impl_shutdown() {
    x4n::Logger::info("Core shutting down...");
    x4n::ExtensionManager::shutdown();
    x4n::SettingsManager::shutdown();
    remove_md_event_hook();
    remove_radar_visibility_hook();
    remove_frame_tick_hook();
    x4n::HookManager::shutdown();
    x4n::GameAPI::shutdown();
    x4n::EventSystem::shutdown();
    x4n::Logger::shutdown();
}

static int impl_enumerate_settings(const char* ext_id, const SettingInfo** out) {
    if (!ext_id) { if (out) *out = nullptr; return 0; }
    return x4n::SettingsManager::enumerate(ext_id, out);
}

static void impl_set_extension_setting(const char* ext_id, const char* key,
                                       const SettingValueC* value) {
    if (!ext_id || !key || !value) return;
    x4n::SettingsManager::set_from_abi(ext_id, key, *value);
}

// ---------------------------------------------------------------------------
// core_init implementation
// ---------------------------------------------------------------------------
static int core_init_impl(CoreInitContext* ctx) {
    g_ext_root             = ctx->ext_root;
    g_get_lua_property     = ctx->get_lua_property;
    g_get_lua_property_str = ctx->get_lua_property_str;

    x4n::Logger::init(g_ext_root);
    x4n::Logger::info("X4Native core v" X4_GAME_VERSION_LABEL " initializing...");
    x4n::Logger::info("Extension root: {}", g_ext_root);

    x4n::EventSystem::init();

    g_game_version  = x4n::Version::detect();
    g_version_string = std::string(X4_GAME_VERSION_LABEL) +
                       " (game: " + g_game_version + ")";

    const std::string& detected_build = x4n::Version::build();
    const std::string  expected_build = std::to_string(X4_GAME_TYPES_BUILD);
    const bool rva_safe_mode =
        !detected_build.empty() && detected_build != expected_build;
    if (rva_safe_mode) {
        x4n::Logger::error("GAME VERSION MISMATCH: running build {} but this "
                           "X4Native was compiled for build {} ({}). RVA-dependent "
                           "features disabled.",
                           detected_build, expected_build, X4_GAME_VERSION_LABEL);
    }

    x4n::GameAPI::init();
    if (!rva_safe_mode)
        x4n::GameAPI::load_internal_db(g_ext_root, X4_GAME_VERSION_LABEL);

    x4n::Logger::open_files();

    if (!rva_safe_mode)
        resolve_offset_pointers(x4n::GameAPI::exe_base());

    x4n::HookManager::init();

    if (!rva_safe_mode) {
        install_frame_tick_hook();
        install_radar_visibility_hook();
        install_md_event_hook();
    }

    g_raise_lua_event = ctx->raise_lua_event;
    g_register_lua_bridge = ctx->register_lua_bridge;
    g_stash_set    = ctx->stash_set;
    g_stash_get    = ctx->stash_get;
    g_stash_remove = ctx->stash_remove;
    g_stash_clear  = ctx->stash_clear;
    x4n::ExtensionManager::init(g_ext_root, g_game_version,
                                g_raise_lua_event, g_register_lua_bridge,
                                g_stash_set, g_stash_get,
                                g_stash_remove, g_stash_clear);

    ctx->dispatch->discover_extensions    = impl_discover_extensions;
    ctx->dispatch->raise_event            = impl_raise_event;
    ctx->dispatch->get_version            = impl_get_version;
    ctx->dispatch->get_loaded_extensions  = impl_get_loaded_extensions;
    ctx->dispatch->set_lua_state          = impl_set_lua_state;
    ctx->dispatch->prepare_reload         = impl_prepare_reload;
    ctx->dispatch->shutdown               = impl_shutdown;
    ctx->dispatch->log                    = impl_log;
    ctx->dispatch->enumerate_settings     = impl_enumerate_settings;
    ctx->dispatch->set_extension_setting  = impl_set_extension_setting;

    x4n::Logger::info("Core initialized successfully");
    return 0;
}

// ---------------------------------------------------------------------------
// Exported entry points
// ---------------------------------------------------------------------------
static void report_boundary_exception(const char* where, const char* what) {
    char buf[256];
    snprintf(buf, sizeof(buf), "X4Native core: exception in %s: %s\n",
             where, what ? what : "(unknown)");
    fwrite(buf, 1, strlen(buf), stderr);
}

extern "C" __attribute__((visibility("default")))
int core_init(CoreInitContext* ctx) {
    try {
        return core_init_impl(ctx);
    } catch (const std::exception& e) {
        report_boundary_exception("core_init", e.what());
        return 1;
    } catch (...) {
        report_boundary_exception("core_init", nullptr);
        return 1;
    }
}

extern "C" __attribute__((visibility("default")))
void core_shutdown() {
    try {
        impl_shutdown();
    } catch (const std::exception& e) {
        report_boundary_exception("core_shutdown", e.what());
    } catch (...) {
        report_boundary_exception("core_shutdown", nullptr);
    }
}

// ---------------------------------------------------------------------------
// No DllMain on Linux
// ---------------------------------------------------------------------------