local events, queue, now, saves, acknowledgements={}, {}, 0, 0, 0
local possible=true
package.loaded.ffi={C={IsPlayerValid=function()return true end},cdef=function()end}
X4HostSettings={slot="save_001",autosave=300}
function RegisterEvent(name,fn)events[name]=fn end
function DebugError()end
function getElapsedTime()return now end
function IsSavingPossible()return possible end
function SaveGame(slot)assert(slot=="save_001");saves=saves+1 end
__X4NATIVE_API={raise_event=function(name)assert(name=="x4_host.save_complete");acknowledgements=acknowledgements+1 end}
Helper={addDelayedOneTimeCallbackOnUpdate=function(fn,_,at)queue[#queue+1]={fn=fn,at=at}end}
local function tick()
    table.sort(queue,function(a,b)return a.at<b.at end)
    local callback=table.remove(queue,1);assert(callback);now=callback.at;callback.fn()
end
dofile("docker/runtime/host_runtime.lua")
assert(#queue==0 and saves==0,"save requested before definitive readiness")
events["gpu_free_probe.universe_ready"]();tick();assert(saves==1)
events["x4native.game_saved"]();assert(acknowledgements==0,"ordinary autosave acknowledged stop")
possible=false;events["x4_host.save_stop"]();tick()
assert(saves==1 and acknowledgements==0,"unavailable save bypassed guard")
possible=true;tick();assert(saves==2 and acknowledgements==0)
events["x4native.game_saved"]();assert(acknowledgements==1)
print("PASS: readiness-gated autosave, retry and real save-event acknowledgement")
