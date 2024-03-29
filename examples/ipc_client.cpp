#include <simple_ipc/ipc/ipc_client.hpp>
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

    simple::ipc::ipc_client_t client{server_name};
    uint8_t body[] = {'h', 'e', 'l', 'l', 'o', '\0'};

    while(!stoped) {
        auto pack = simple::ipc::build_req_packet(0, 1, body, sizeof(body));
        client.send_packet(std::move(pack), [](std::unique_ptr<simple::ipc::packet> rsp) {
            if (rsp) {
                std::cout << "recv rsp = " << (char*)rsp->body().data() << std::endl;
            } else {
                std::cout << "failed the recv rsp" << std::endl;
            }
        }, 3);

        std::this_thread::sleep_for(1ms);
    }
    return 0;
}
