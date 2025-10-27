#include <iostream>
#include <string>
#include <cstring>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <chrono>
#include <thread>

int main() {
    const char* socket_path = "/tmp/vss_ipc_socket";
    std::string request = "GET_SCHEMA Vehicle";

    std::cout << "[TEST] Checking schema IPC server...\n";

    // Create UNIX domain socket
    int sock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock < 0) {
        perror("[TEST] socket");
        return 1;
    }

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, socket_path, sizeof(addr.sun_path) - 1);

    // Retry connect a few times in case broker isn't ready yet
    int attempts = 10;
    while (connect(sock, (sockaddr*)&addr, sizeof(addr)) < 0 && attempts-- > 0) {
        std::cout << "[TEST] Waiting for DataBroker IPC server...\n";
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
    }

    if (attempts <= 0) {
        perror("[TEST] connect");
        close(sock);
        return 1;
    }

    // Send schema request
    std::cout << "[TEST] Sending request: " << request << std::endl;
    write(sock, request.c_str(), request.size());

    // Receive response
    char buffer[65536];
    ssize_t n = read(sock, buffer, sizeof(buffer) - 1);
    if (n > 0) {
        buffer[n] = '\0';
        std::cout << "\n✅ [TEST] Schema response received:\n";
        std::cout << buffer << "\n";
    } else {
        std::cerr << "❌ [TEST] No response from DataBroker\n";
    }

    close(sock);
    return 0;
}

