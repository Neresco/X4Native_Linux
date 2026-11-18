-- GPU-free host startup and read-only simulation observations.
-- Uses the 9.00 game UI's own NewGame/Helper callback route; no input injection.
local ffi = require("ffi")
local C = ffi.C
local settings = GPUFreeProbeSettings or { graphics = "baseline", start_mode = "game" }
local declarations = {
    "double GetCurrentGameTime(void);",
    "bool IsGamePaused(void);",
    "const char* GetAAOption(bool useconfig);",
    "void SetAAOption(const char* mode);",
    "void SetVolumetricFogOption(int32_t setting);",
    "int32_t GetVolumetricFogOption(void);",
    "typedef struct { int x; int y; } GPUFreeResolution;",
    "GPUFreeResolution GetRenderResolutionOption(void);",
    "const char* GetUpscalingOption(bool useconfig);",
    "bool IsUpscalingOptionSupported(const char* mode);",
    "void SetUpscalingOption(const char* mode);",
    "void SaveUpscalingOption(void);",
    "bool HasFrameRateLimit(void);",
    "uint32_t GetFrameRateTarget(void);",
    "void SetFrameRateLimit(bool limited);",
    "void SetFrameRateTarget(uint32_t fps);",
    "typedef struct { double fps; double moveTime; double renderTime; double gpuTime; } GPUFreeFPS;",
    "GPUFreeFPS GetFPS(void);",
    "bool IsPlayerValid(void);",
    "uint32_t GetNumAllFactionShips(const char* factionid);",
    "uint32_t GetNumAllFactionStations(const char* factionid);",
    "uint32_t GetAllFactionShips(uint64_t* result, uint32_t resultlen, const char* factionid);",
    "bool IsValidComponent(uint64_t componentid);",
    "uint64_t GetContextByClass(uint64_t componentid, const char* classname, bool includeself);",
    "typedef struct { float x; float y; float z; float yaw; float pitch; float roll; } GPUFreePosRot;",
    "GPUFreePosRot GetObjectPositionInSector(uint64_t objectid);",
}
for _, declaration in ipairs(declarations) do
    -- The normal UI may already have registered the same function.
    pcall(ffi.cdef, declaration)
end

local function log(message)
    DebugError("[GPUFREE] " .. message)
end

local world_ready = false
local last_positions = {}
local function graphics_snapshot()
    local resolution = GetResolutionOption()
    local render = C.GetRenderResolutionOption()
    return string.format("resolution=%dx%d render=%dx%d preset=%s shadows=%s ssao=%s fog=%s aa=%s upscaling=%s limited=%s target=%d",
        resolution.width, resolution.height, render.x, render.y, tostring(GetGfxQualityOption()),
        tostring(GetShadowOption()), tostring(GetSSAOOption()),
        tostring(C.GetVolumetricFogOption()), ffi.string(C.GetAAOption(false)), ffi.string(C.GetUpscalingOption(false)),
        tostring(C.HasFrameRateLimit()), tonumber(C.GetFrameRateTarget()))
end
Helper.addDelayedOneTimeCallbackOnUpdate(function()
    local ok, result = pcall(function()
        if settings.graphics == "low" then
            -- Avoid reapplying graphics changes on every UI reload.
            if GetGfxQualityOption() ~= 1 then SetGfxQualityOption(1) end
            if GetShadowOption() ~= 0 then SetShadowOption(0) end
            if GetSSAOOption() ~= 0 then SetSSAOOption(0) end
            if C.GetVolumetricFogOption() ~= 0 then C.SetVolumetricFogOption(0) end
            if ffi.string(C.GetAAOption(false)) ~= "none" then C.SetAAOption("none") end
            if settings.upscaling and ffi.string(C.GetUpscalingOption(false)) ~= settings.upscaling
                    and C.IsUpscalingOptionSupported(settings.upscaling) then
                C.SetUpscalingOption(settings.upscaling)
                C.SaveUpscalingOption()
            end
            if settings.width and settings.height then
                local resolution = GetResolutionOption()
                if resolution.width ~= settings.width or resolution.height ~= settings.height then
                    SetResolutionOption(settings.width, settings.height)
                    SaveResolutionOption()
                end
            end
            if settings.frame_rate then
                C.SetFrameRateLimit(true)
                C.SetFrameRateTarget(settings.frame_rate)
            end
        end
        return graphics_snapshot()
    end)
    log(ok and ("graphics " .. result) or ("graphics_error=" .. tostring(result)))
end, true, getElapsedTime() + 1)
RegisterEvent("gpu_free_probe.universe_ready", function()
    world_ready = true
    log("universe_ready")
    local ok, result = pcall(graphics_snapshot)
    log(ok and ("graphics_ready " .. result) or ("graphics_error=" .. tostring(result)))
    if settings.start_mode == "host_existing" then
        Helper.addDelayedOneTimeCallbackOnUpdate(function()
            local api = _G.__X4NATIVE_API
            if api and api.raise_event then
                log("requesting_native_host_after_universe_ready")
                api.raise_event("x4mp_host_request")
            else
                log("native_host_error=framework_api_unavailable")
            end
        end, true, getElapsedTime() + 1)
    end
end)

local function observe()
    local ok, message = pcall(function()
        local valid = C.IsPlayerValid()
        if not valid then
            return "sample player_valid=false"
        end
        local ships = tonumber(C.GetNumAllFactionShips("argon"))
        local stations = tonumber(C.GetNumAllFactionStations("argon"))
        local fps = C.GetFPS()
        local moved, compared = 0, 0
        local max_distance = 0
        if world_ready then
            local ids = ffi.new("uint64_t[64]")
            local count = math.min(tonumber(C.GetAllFactionShips(ids, 64, "argon")), 64)
            for index = 0, count - 1 do
                local id = ids[index]
                if C.IsValidComponent(id) then
                    local sector = C.GetContextByClass(id, "sector", false)
                    if tonumber(sector) ~= 0 then
                        local position = C.GetObjectPositionInSector(id)
                        local key = tostring(id)
                        local previous = last_positions[key]
                        if previous and previous.sector == tostring(sector) then
                            compared = compared + 1
                            local dx, dy, dz = position.x - previous.x, position.y - previous.y, position.z - previous.z
                            local distance = math.sqrt(dx * dx + dy * dy + dz * dz)
                            if distance > 1 then moved = moved + 1 end
                            max_distance = math.max(max_distance, distance)
                        end
                        last_positions[key] = {sector = tostring(sector), x = position.x, y = position.y, z = position.z}
                    end
                end
            end
        end
        return string.format(
            "sample game_time=%.3f paused=%s world_ready=%s argon_ships=%d argon_stations=%d compared=%d moved=%d max_distance=%.3f elapsed=%.3f fps=%.3f move_ms=%.3f render_ms=%.3f gpu_ms=%.3f",
            tonumber(C.GetCurrentGameTime()), tostring(C.IsGamePaused()), tostring(world_ready),
            ships, stations, compared, moved, max_distance, getElapsedTime(), fps.fps, fps.moveTime, fps.renderTime, fps.gpuTime)
    end)
    log(ok and message or ("observation_error=" .. tostring(message)))
    Helper.addDelayedOneTimeCallbackOnUpdate(observe, true, getElapsedTime() + 10)
end

log("ui_loaded; disposable gamestart=x4ep1_gamestart_trade")
Helper.addDelayedOneTimeCallbackOnUpdate(function()
    if (settings.start_mode == "game" or settings.start_mode == "host_existing") and not C.IsPlayerValid() then
        log("requesting_new_game")
        NewGame(settings.gamestart or "x4ep1_gamestart_trade")
    end
end, true, getElapsedTime() + 20)
Helper.addDelayedOneTimeCallbackOnUpdate(observe, true, getElapsedTime() + 25)
