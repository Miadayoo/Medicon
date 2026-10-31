#include <medicon/sdk/detail/ReconnectDispatch.hpp>
#include <array>
#include <cstdint>
#include <iostream>
#include <stdexcept>

namespace {
using namespace medicon::sdk::detail;
void check(bool value,const char* text) {if(!value) throw std::runtime_error(text);}
std::array<std::uintptr_t,32> table{},wrong_table{};
std::uintptr_t stack=0,handler=0;
std::uint8_t valid=1;
SceneStackOwner supplied{};
void* client_expected=nullptr;
void* factory_expected=nullptr;
unsigned acquired=0,released=0,retried=0;
void* get_stack(void* client,SceneStackOwner* out) {
    check(client==client_expected,"getter must receive current primary client");
    *out=supplied;++acquired;return out;
}
void release_stack(SceneStackOwner* owner) {++released;*owner={};}
void retry(void* request_handler,void* factory,SceneStackOwner* owner) {
    check(request_handler==&handler && factory==factory_expected,"retry context arguments changed");
    check(owner->object==reinterpret_cast<std::uintptr_t>(&stack),"retry received handler instead of SceneStack");
    check(owner->control==123 && owner->validity==reinterpret_cast<std::uintptr_t>(&valid),"owner/control was copied incorrectly");
    ++retried;*owner={}; // Native retry consumes its owner; caller must not release twice.
}
void run() {
    int client=0,factory=0;
    client_expected=&client;factory_expected=&factory;
    table[25]=456;wrong_table[25]=789;
    stack=reinterpret_cast<std::uintptr_t>(table.data());handler=reinterpret_cast<std::uintptr_t>(wrong_table.data());
    const ReconnectCalls calls{get_stack,release_stack,retry};
    const auto invoke=[&]{return dispatch_reconnect(calls,&client,&handler,&factory,
        reinterpret_cast<std::uintptr_t>(&stack),reinterpret_cast<std::uintptr_t>(table.data()),456);};
    supplied={reinterpret_cast<std::uintptr_t>(&valid),123,reinterpret_cast<std::uintptr_t>(&stack)};
    check(invoke() && acquired==1 && retried==1 && released==0,"valid SceneStack owner was not consumed exactly once");
    // Regression: both native owners have identical sizes and valid controls.
    // The former +C70 getter returned this handler owner and crashed pushScene.
    supplied.object=reinterpret_cast<std::uintptr_t>(&handler);
    check(!invoke() && retried==1 && released==1,"handler owner reached native retry");
    supplied.object=reinterpret_cast<std::uintptr_t>(&stack);valid=0;
    check(!invoke() && retried==1 && released==2,"dead SceneStack reached native retry");
    valid=1;stack=reinterpret_cast<std::uintptr_t>(wrong_table.data());
    check(!invoke() && retried==1 && released==3,"wrong SceneStack vtable reached native retry");
    stack=reinterpret_cast<std::uintptr_t>(table.data());table[25]=789;
    check(!invoke() && retried==1 && released==4,"changed pushScene implementation reached native retry");
    table[25]=456;supplied.validity=1;
    check(!invoke() && retried==1 && released==5,"unreadable owner flag reached native retry");
    const auto before=acquired;
    check(!dispatch_reconnect(calls,nullptr,&handler,&factory,reinterpret_cast<std::uintptr_t>(&stack),
        reinterpret_cast<std::uintptr_t>(table.data()),456) && acquired==before,"missing client invoked native getter");
}
}
int main() {
    try {run();std::cout<<"Reconnect SceneStack type, ownership and stale-context checks passed.\n";return 0;}
    catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
