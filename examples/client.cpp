#include <simple_ipc/client.hpp>
#include <thread>
#include <csignal>
#include <iostream>
using namespace std::chrono;

const char* server_name = "simple_ipc";
volatile bool stoped = false;

void signal_handler(int signal) {
    stoped = true;
}

int main(int argc, char** argv) {
    std::signal(SIGINT, signal_handler);

    simple::ipc::client_t client{server_name};
    auto pid = getpid();
    char buf[] = "hello";

    while(!stoped) {
        auto pack = simple::ipc::build_req_packet(pid, 1, (uint8_t*)buf, sizeof(buf));
        client.send_packet(std::move(pack), [](std::unique_ptr<simple::ipc::packet> pack) {
            std::cout << "recv rsp = " << (char*)pack->body().data() << std::endl;
        });

        std::this_thread::sleep_for(100ms);
    }
    return 0;
}
