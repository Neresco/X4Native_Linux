#include "../runtime/control.cpp"
#include <iostream>
#include <stdexcept>
static int raised=0;
static int raise_lua(const char* event,const char*) {
    if (std::string(event)!="x4_host.save_stop") throw std::runtime_error("unexpected event");
    ++raised; return 0;
}
static void log(int,const char*) {}
static void check(bool value,const char* message) { if(!value) throw std::runtime_error(message); }
int main() {
    X4NativeAPI context{}; context.raise_lua_event=raise_lua; context.log=log; api=&context;
    directory=std::filesystem::current_path()/"control-test-state";
    check(!std::filesystem::exists(directory),"preserve failed fixture");
    std::filesystem::create_directory(directory);
    frame(nullptr,nullptr,nullptr); check(raised==0,"stop fired without request");
    std::ofstream(directory/"stop-requested")<<"stop\n";
    last_poll={}; frame(nullptr,nullptr,nullptr); frame(nullptr,nullptr,nullptr);
    check(raised==1,"stop event missing/duplicated");
    check(!std::filesystem::exists(directory/"save-complete"),"request incorrectly acknowledged a save");
    saved(nullptr,nullptr,nullptr);
    check(std::filesystem::exists(directory/"save-complete"),"save event not acknowledged");
    std::filesystem::remove(directory/"stop-requested");
    std::filesystem::remove(directory/"save-complete"); std::filesystem::remove(directory);
    std::cout<<"PASS: game-thread stop handoff and event-driven save acknowledgement\n";
}
