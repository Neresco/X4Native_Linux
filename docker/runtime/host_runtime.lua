-- Game mutation stays on the UI lifecycle, after definitive universe readiness.
local ffi=require("ffi")
pcall(ffi.cdef,"bool IsPlayerValid(void);")
local settings=X4HostSettings
local ready, saving, stopping=false, false, false
local function log(message) DebugError("[X4HOST] "..message) end
local function request_save()
    if not ready or saving or not ffi.C.IsPlayerValid() or not IsSavingPossible() then return false end
    saving=true
    local ok=pcall(SaveGame,settings.slot,"X4 multiplayer host")
    if not ok then saving=false; log("save request failed"); return false end
    log("save requested")
    return true
end
local function autosave()
    request_save()
    Helper.addDelayedOneTimeCallbackOnUpdate(autosave,true,getElapsedTime()+settings.autosave)
end
RegisterEvent("gpu_free_probe.universe_ready",function()
    if ready then return end
    ready=true
    Helper.addDelayedOneTimeCallbackOnUpdate(autosave,true,getElapsedTime()+30)
end)
local function save_for_stop()
    if not stopping then return end
    if not request_save() then
        Helper.addDelayedOneTimeCallbackOnUpdate(save_for_stop,true,getElapsedTime()+1)
    end
end
RegisterEvent("x4_host.save_stop",function()
    stopping=true
    Helper.addDelayedOneTimeCallbackOnUpdate(save_for_stop,true,getElapsedTime()+.1)
end)
RegisterEvent("x4native.game_saved",function()
    if not saving then return end
    saving=false
    log("save completed")
    local api=_G.__X4NATIVE_API
    if stopping and api then api.raise_event("x4_host.save_complete") end
end)
