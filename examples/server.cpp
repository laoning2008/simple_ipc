#include <simple_ipc/server.hpp>
#include <thread>
#include <csignal>
#include <iostream>
using namespace std::chrono;

const char* server_name = "simple_ipc";
volatile bool stoped = false;

int main(int argc, char** argv) {
    simple::ipc::server_t server{server_name};
    char buf[] = "world";

    server.register_push_receiver(1, [&server, &buf](std::unique_ptr<simple::ipc::packet> req) {
        std::cout << "recv req = " << (char*)req->body().data() << std::endl;
        auto rsp = simple::ipc::build_rsp_packet(req->process_id(), req->cmd(), req->seq(), 0, (uint8_t*)buf, sizeof(buf));
        server.send_packet(std::move(rsp));
    });

    char c = 0;
    std::cin >> c;
    return 0;
}