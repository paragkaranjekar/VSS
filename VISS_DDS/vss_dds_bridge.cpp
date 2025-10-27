#include <iostream>
#include <thread>
#include <chrono>
#include <cstring>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <iomanip>
#include <sstream>
#include <chrono>
#include <string>
#include <iostream>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

// Include the IDL-generated types (from `idlc -l c vss_topics.idl`)
extern "C" {
#include "vss_topics.h"
}

#include "dds/dds.h"

// -----------------------------------------------------------------------------
// DDS Topic Names
// -----------------------------------------------------------------------------
#define SCHEMA_REQ_TOPIC "SchemaRequest"
#define SCHEMA_RES_TOPIC "SchemaResponse"
#define VSS_DATA_TOPIC   "VehicleData"

// -----------------------------------------------------------------------------
// IPC Helpers
// -----------------------------------------------------------------------------
std::string ipc_request(const std::string &request)
{

    const char *socket_path = "/tmp/vss_ipc_socket";
    int sock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock < 0) {
        perror("socket");
        return "{}";
    }

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, socket_path, sizeof(addr.sun_path) - 1);

    int attempts = 10;
	while (connect(sock, (sockaddr *)&addr, sizeof(addr)) < 0 && attempts-- > 0) {
	    std::this_thread::sleep_for(std::chrono::milliseconds(200));
	}
	if (attempts <= 0) {
	    perror("connect");
	    close(sock);
	    return "{}";
	}


    write(sock, request.c_str(), request.size());
    char buffer[65536];
    ssize_t n = read(sock, buffer, sizeof(buffer) - 1);
    close(sock);

    if (n > 0) {
        buffer[n] = '\0';
        return std::string(buffer);
    }
    return "{}";
}

// -----------------------------------------------------------------------------
// DDS Schema Request Handler
// -----------------------------------------------------------------------------
void dds_schema_handler(dds_entity_t participant)
{
    // Create SchemaRequest topic + reader
    dds_entity_t req_topic = dds_create_topic(
        participant, &SchemaRequest_desc, SCHEMA_REQ_TOPIC, nullptr, nullptr);
    dds_entity_t req_reader = dds_create_reader(participant, req_topic, nullptr, nullptr);

    // Create SchemaResponse topic + writer
    dds_entity_t res_topic = dds_create_topic(
        participant, &SchemaResponse_desc, SCHEMA_RES_TOPIC, nullptr, nullptr);
    dds_entity_t res_writer = dds_create_writer(participant, res_topic, nullptr, nullptr);

    std::cout << "[DDS] Waiting for schema requests...\n";

    while (true) {
        SchemaRequest req;
        void *samples[1] = {&req};
        dds_sample_info_t info;
        dds_return_t rc = dds_take(req_reader, samples, &info, 1, 1);

        if (rc > 0 && info.valid_data) {
            std::cout << "[DDS] Schema request for: " << req.path << std::endl;

            // Ask schema from DataBroker via IPC
            std::string json = ipc_request("GET_SCHEMA " + std::string(req.path));

            SchemaResponse res{};
            strncpy(res.json, json.c_str(), sizeof(res.json) - 1);
            dds_write(res_writer, &res);

            std::cout << "[DDS] Sent schema response.\n";
            //std::cout << res.json;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

// Helper: Convert current time → ISO 8601 UTC string
std::string getISO8601Timestamp() {
    using namespace std::chrono;
    auto now = system_clock::now();
    auto now_time_t = system_clock::to_time_t(now);
    auto now_ms = duration_cast<milliseconds>(now.time_since_epoch()) % 1000;

    std::ostringstream oss;
    oss << std::put_time(gmtime(&now_time_t), "%Y-%m-%dT%H:%M:%S");
    oss << '.' << std::setfill('0') << std::setw(3) << now_ms.count() << "Z";
    return oss.str();
}

// -----------------------------------------------------------------------------
// IPC Data Receiver → DDS Publisher (VISS Transport JSON compliant)
// -----------------------------------------------------------------------------
void ipc_data_listener(dds_entity_t participant)
{
    const char *socket_path = "/tmp/vss_ipc_data";
    unlink(socket_path);  // Remove stale socket if any

    int serverSock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (serverSock < 0) {
        perror("[IPC] socket");
        return;
    }

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, socket_path, sizeof(addr.sun_path) - 1);

    if (bind(serverSock, (sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("[IPC] bind");
        close(serverSock);
        return;
    }

    if (listen(serverSock, 5) < 0) {
        perror("[IPC] listen");
        close(serverSock);
        return;
    }

    std::cout << "[IPC] Listening for data from DataBroker at " << socket_path << "\n";

    // Create DDS topic and writer for VehicleData
    dds_entity_t data_topic = dds_create_topic(
        participant, &VSSData_desc, "VehicleData", nullptr, nullptr);
    dds_entity_t data_writer = dds_create_writer(participant, data_topic, nullptr, nullptr);

    // Main loop: accept and forward messages
    while (true) {
        int clientSock = accept(serverSock, nullptr, nullptr);
        if (clientSock < 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            continue;
        }

        char buffer[1024];
        ssize_t n = read(clientSock, buffer, sizeof(buffer) - 1);
        if (n > 0) {
            buffer[n] = '\0';
            std::string message(buffer);

            // Expected format from broker: "<VSS_path> <value>"
            size_t spacePos = message.find(' ');
            if (spacePos != std::string::npos) {
                std::string path = message.substr(0, spacePos);
                double value = atof(message.substr(spacePos + 1).c_str());

                // Build ISO timestamp and VISS transport JSON
                std::string isoTime = getISO8601Timestamp();
                std::ostringstream vissJson;
                vissJson << "{"
                         << "\"path\":\"" << path << "\","
                         << "\"value\":" << value << ","
                         << "\"timestamp\":\"" << isoTime << "\""
                         << "}";

                // Create DDS message
                VSSData msg{};
                strncpy(msg.payload, vissJson.str().c_str(), sizeof(msg.payload) - 1);

                // Publish over DDS
                dds_write(data_writer, &msg);

                // Log to console
                std::cout << "[IPC->DDS] " << vissJson.str() << std::endl;
            }
        }
        close(clientSock);
    }

    close(serverSock);
}

// -----------------------------------------------------------------------------
// MAIN
// -----------------------------------------------------------------------------
int main()
{
    std::cout << "[Terminal 1] Starting VSS DDS Bridge...\n";

    dds_entity_t participant = dds_create_participant(DDS_DOMAIN_DEFAULT, nullptr, nullptr);
    if (participant < 0) {
        std::cerr << "Failed to create DDS participant.\n";
        return 1;
    }

    std::thread ipcThread(ipc_data_listener, participant);
    std::thread schemaThread(dds_schema_handler, participant);

    ipcThread.join();
    schemaThread.join();

    dds_delete(participant);
    return 0;
}

