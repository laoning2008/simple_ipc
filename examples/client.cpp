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
    uint8_t body[] = {'h', 'e', 'l', 'l', 'o', '\0'};

    while(!stoped) {
        auto pack = simple::ipc::build_req_packet(pid, 1, body, sizeof(body));
        client.send_packet(std::move(pack), [](std::unique_ptr<simple::ipc::packet> rsp) {
            std::cout << "recv rsp = " << (char*)rsp->body().data() << std::endl;
        }, 1);

        std::this_thread::sleep_for(1ms);
    }
    return 0;
}
