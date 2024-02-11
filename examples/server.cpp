#include <simple_ipc/server.hpp>
#include <thread>
#include <csignal>
#include <iostream>
using namespace std::chrono;

const char* server_name = "simple_ipc";
volatile bool stoped = false;

int main(int argc, char** argv) {
    simple::ipc::server_t server{server_name};
    uint8_t body[6] = {'w', 'o', 'r', 'l', 'd', '\0'};

    server.register_request_processor(1, [&server, &body](std::unique_ptr<simple::ipc::packet> req) {
        std::cout << "recv req = " << (char*)req->body().data() << std::endl;
        auto rsp = simple::ipc::build_rsp_packet(req->connection_id(), req->cmd(), req->seq(), 0, (uint8_t*)body, sizeof(body));
        server.send_packet(std::move(rsp));
    });

    char c = 0;
    std::cin >> c;
    return 0;
}