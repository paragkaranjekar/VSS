#include <iostream>
#include <cstring>
#include <fstream>
#include <sstream>
#include <ctime>
#include <vector>
#include <map>
#include <algorithm>
#include <sqlite3.h>
#include "include/mappings.h"
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <chrono>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include "include/cJSON.h"
#include <thread>
#include <sys/un.h>
#include <sys/socket.h>
#include <unistd.h>
using namespace std;


// ------------------- SQLite Database Handle -------------------
sqlite3* db = nullptr;

// ------------------- Thread-safety -------------------
mutex bufferMutex;   // protects buffer
mutex dbMutex;       // protects sqlite operations and nodeMap loads

// ------------------- Data Structures -------------------
enum DataType : uint8_t { DT_UINT8, DT_UINT16, DT_FLOAT };

// Raw SOME/IP (or similar) data frame
typedef struct {
    uint16_t service_id;
    uint16_t event_id;
    size_t payload_len;
    uint8_t payload[256];
    uint64_t timestamp_ms;
} RawData;

// TimeSeries point for buffering
struct TimeSeriesPoint {
    uint64_t timestamp;
    double value;
    string unit;
};


// ------------------- Node Map -------------------
// Maps full_path -> node_id (preloaded from database)
map<string, int> nodeMap;

// ------------------- Buffered Timeseries -------------------
// Stores recent data points for each node (in-memory buffer)
map<int, vector<TimeSeriesPoint>> buffer;

// ------------------- Helpers -------------------
inline uint16_t be16(uint16_t v) { return __builtin_bswap16(v); }

inline DataType dtypeToEnum(const char* dtype) {
    if (strcmp(dtype, "uint8") == 0) return DT_UINT8;
    if (strcmp(dtype, "uint16") == 0) return DT_UINT16;
    if (strcmp(dtype, "float") == 0) return DT_FLOAT;
    return DT_UINT8;
}

// Compute elapsed milliseconds between timespec
double elapsed_ms(const struct timespec &start, const struct timespec &end) {
    return (end.tv_sec - start.tv_sec) * 1000.0 + (end.tv_nsec - start.tv_nsec) / 1e6;
}

// ------------------- IPC -------------------
void sendDataOverIPC(const string &data) {
    const char* socket_path = "/tmp/vss_ipc_data";
    int sock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock < 0) return;

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, socket_path, sizeof(addr.sun_path) - 1);

    if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) == 0) {
        send(sock, data.c_str(), data.size(), 0);
    }

    close(sock);
}

// ------------------- Load Node IDs -------------------
// Load all nodes from pre-existing database into nodeMap
bool loadNodeIds() {
    lock_guard<mutex> lock(dbMutex);
    nodeMap.clear();

    const char* sql = "SELECT id, full_path FROM nodes;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        cerr << "Failed to prepare node query: " << sqlite3_errmsg(db) << endl;
        return false;
    }
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        int id = sqlite3_column_int(stmt, 0);
        const unsigned char* text = sqlite3_column_text(stmt, 1);
        string path = text ? reinterpret_cast<const char*>(text) : string();
        nodeMap[path] = id;
    }
    sqlite3_finalize(stmt);
    return true;
}

// ------------------- Buffering -------------------
void bufferTimeseries(int nodeId, uint64_t timestamp, double value, const string &unit) {
    lock_guard<mutex> lock(bufferMutex);
    buffer[nodeId].push_back({timestamp, value, unit});
    cout << nodeId << " " << value << endl;
}

// Flush buffer to database in a single transaction
// Insert only the last 3 points per node into DB
void flushBuffer() {
    // Copy buffer under lock then release quickly to avoid blocking producers
    map<int, vector<TimeSeriesPoint>> localCopy;
    {
        lock_guard<mutex> lock(bufferMutex);
        if (buffer.empty()) {
            // nothing to do
            return;
        }
        localCopy.swap(buffer); // move out
    }

    lock_guard<mutex> dblock(dbMutex);
    sqlite3_exec(db, "BEGIN TRANSACTION;", nullptr, nullptr, nullptr);

    for (auto &pair : localCopy) {
        int nodeId = pair.first;
        auto &points = pair.second;

        // keep only last 3 by timestamp
        if (points.size() > 3) {
            sort(points.begin(), points.end(),
                 [](const TimeSeriesPoint &a, const TimeSeriesPoint &b){ return a.timestamp < b.timestamp; });
            points = vector<TimeSeriesPoint>(points.end()-3, points.end());
        }

        for (auto &pt : points) {
            const char* insertSQL =
                "INSERT INTO timeseries (node_id, vss_path, timestamp_ms, value, unit) "
                "VALUES (?, ?, ?, ?, ?);";
            sqlite3_stmt* stmt = nullptr;
            if (sqlite3_prepare_v2(db, insertSQL, -1, &stmt, nullptr) == SQLITE_OK) {
                sqlite3_bind_int(stmt, 1, nodeId);

                // Find path for nodeId
                string path = "<unknown>";
                for (const auto &p : nodeMap) {
                    if (p.second == nodeId) {
                        path = p.first;
                        break;
                    }
                }
                sqlite3_bind_text(stmt, 2, path.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_int64(stmt, 3, (sqlite3_int64)pt.timestamp);
                sqlite3_bind_double(stmt, 4, pt.value);
                sqlite3_bind_text(stmt, 5, pt.unit.c_str(), -1, SQLITE_TRANSIENT);

                if (sqlite3_step(stmt) == SQLITE_DONE) {
                    // optional logging:
                    cout << "[Inserted] NodeID=" << nodeId << " Path=" << path << " Timestamp=" << pt.timestamp << " Value=" << pt.value << " Unit=" << pt.unit << endl;
                }
            }
            sqlite3_finalize(stmt);
        }
    }

    sqlite3_exec(db, "COMMIT;", nullptr, nullptr, nullptr);
}

// ------------------- Mapping Lookup -------------------
const MappingEntry* findMapping(uint16_t service_id, uint16_t event_id) {
    int low = 0, high = MAPPING_TABLE_SIZE - 1;
    while (low <= high) {
        int mid = (low + high) >> 1;
        auto sid = mappingTable[mid].service_id;
        auto eid = mappingTable[mid].method_id;
        if ((sid < service_id) || (sid == service_id && eid < event_id)) {
            low = mid + 1;
        } else if ((sid > service_id) || (sid == service_id && eid > event_id)) {
            high = mid - 1;
        } else {
            return &mappingTable[mid]; // Found mapping
        }
    }
    return nullptr; // Not found
}

// ------------------- Process Raw Data -------------------
string processRawData(const RawData &data) {
    string output;
    const MappingEntry* m = findMapping(data.service_id, data.event_id);
    if (!m) {
        output += "No mapping for SID=0x" + to_string(data.service_id) +
                  " EID=0x" + to_string(data.event_id) + "\n";
        return output;
    }

    for (size_t i = 0; i < m->num_signals; ++i) {
        const MappingSignal &sig = m->signals[i];
        double value = 0;

        // Decode based on datatype
        switch(dtypeToEnum(sig.dtype)) {
            case DT_UINT8:
                value = data.payload[sig.offset];
                break;
            case DT_UINT16: {
                uint16_t raw = 0;
                memcpy(&raw, &data.payload[sig.offset], sizeof(raw));
                value = be16(raw);
                break;
            }
            case DT_FLOAT: {
                float raw = 0.0f;
                memcpy(&raw, &data.payload[sig.offset], sizeof(raw));
                value = raw;
                break;
            }
        }

        value *= sig.scaling;

        output += string(sig.vss_path) + " = " + to_string(value) + " " + sig.unit + "\n";

        // Buffer instead of inserting directly
        {
            // read nodeMap without locking DB for long; nodeMap is updated only at init so this is fine
            auto it = nodeMap.find(sig.vss_path);
            if (it != nodeMap.end()) {
                bufferTimeseries(it->second, data.timestamp_ms, value, sig.unit);
            }
        }
    }

    return output;
}

// ------------------- Parse Line (for local testing only) -------------------
bool parseLine(const string& line, RawData& data) {
    if (line.empty() || line[0] == '#') return false;

    stringstream ss(line);
    string sid_str, eid_str, payload_str, ts_str;
    getline(ss, sid_str, ',');
    getline(ss, eid_str, ',');
    getline(ss, payload_str, ',');
    getline(ss, ts_str, ',');

    data.service_id = stoi(sid_str, nullptr, 16);
    data.event_id = stoi(eid_str, nullptr, 16);
    data.timestamp_ms = stoull(ts_str);

    stringstream ps(payload_str);
    string byte_str;
    size_t idx = 0;
    while (ps >> byte_str && idx < sizeof(data.payload)) {
        data.payload[idx++] = stoi(byte_str, nullptr, 16);
    }
    data.payload_len = idx;

    return true;
}

// ------------------- API: Input -------------------
// This is the function Adaptive AUTOSAR middleware should call when a SOME/IP request arrives
void Middleware_someipreq(const RawData &data_in) {
    struct timespec t_start, t_send_end;
    clock_gettime(CLOCK_MONOTONIC, &t_start);

    // We copy input locally to avoid problems if caller reuses buffer
    RawData data = data_in;

    string vssData = processRawData(data);

    clock_gettime(CLOCK_MONOTONIC, &t_send_end);
    sendDataOverIPC(vssData);

    cout << "IPC Time : " << elapsed_ms(t_start, t_send_end) << " ms\n";
}

// ------------------- API: Output -------------------
// Return a thread-safe copy of latest buffered values.
map<int, vector<TimeSeriesPoint>> middleware_databroker_latest() {
    lock_guard<mutex> lock(bufferMutex);
    return buffer; // returns a copy
}

// Query historical data from DB for a nodeId (most recent first)
vector<TimeSeriesPoint> middleware_databroker_timeseries(
    const string& vssPath, size_t limit = 100) 
{
    vector<TimeSeriesPoint> result;
    lock_guard<mutex> dblock(dbMutex);

    const char* sql =
        "SELECT t.timestamp_ms, t.value, t.unit "
        "FROM timeseries t "
        "JOIN nodes n ON t.node_id = n.id "
        "WHERE n.full_path LIKE ? || '%' "
        "ORDER BY t.timestamp_ms DESC "
        "LIMIT ?;";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, vssPath.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(stmt, 2, (int)limit);

        while (sqlite3_step(stmt) == SQLITE_ROW) {
            uint64_t ts = (uint64_t)sqlite3_column_int64(stmt, 0);
            double val = sqlite3_column_double(stmt, 1);
            const unsigned char* unitText = sqlite3_column_text(stmt, 2);
            string unit = unitText ? reinterpret_cast<const char*>(unitText) : "";
            result.push_back({ts, val, unit});
        }
    } else {
        cerr << "Failed prepare timeseries query: " << sqlite3_errmsg(db) << endl;
    }

    sqlite3_finalize(stmt);
    return result;
}

// ------------------- Background Flush Thread -------------------
atomic<bool> g_runFlushThread{true};
void flushThreadFunc(unsigned int period_seconds = 2) {
    while (g_runFlushThread.load()) {
        this_thread::sleep_for(chrono::seconds(period_seconds));
        auto t0 = chrono::steady_clock::now();
        flushBuffer();
        auto t1 = chrono::steady_clock::now();
        auto ms = chrono::duration_cast<chrono::milliseconds>(t1 - t0).count();
        cout << "Buffer Flush Time: " << ms << " ms\n";
    }
}




std::string getSchemaAsJson(const std::string& pathPrefix) {
    std::lock_guard<std::mutex> lock(dbMutex);

    if (!db) {
        std::cerr << "[SchemaAPI] Database not initialized\n";
        return "{}";
    }

    // Build query using actual DB schema
    std::string query =
        "SELECT DISTINCT n.full_path, t.unit "
        "FROM timeseries t "
        "JOIN nodes n ON t.node_id = n.id "
        "WHERE n.full_path LIKE '" + pathPrefix + "%'";

    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(db, query.c_str(), -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        std::cerr << "[SchemaAPI] DB query failed: " << sqlite3_errmsg(db) << "\n";
        return "{}";
    }

    cJSON* root = cJSON_CreateObject();

    // Parse prefix into base structure (e.g., Vehicle.Powertrain)
    std::stringstream prefixStream(pathPrefix);
    std::string segment;
    cJSON* parent = root;
    std::vector<std::string> prefixParts;

    while (std::getline(prefixStream, segment, '.')) {
        if (!segment.empty()) prefixParts.push_back(segment);
    }

    for (const auto& seg : prefixParts) {
        cJSON* next = cJSON_GetObjectItem(parent, seg.c_str());
        if (!next) {
            next = cJSON_CreateObject();
            cJSON_AddItemToObject(parent, seg.c_str(), next);
        }
        parent = next;
    }

    // Walk through results and build hierarchy
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        const char* fullPath = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        const char* unit     = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));

        if (!fullPath) continue;

        std::string relPath(fullPath);
        if (relPath.rfind(pathPrefix, 0) == 0)
            relPath = relPath.substr(pathPrefix.size());
        else
            continue;

        if (!relPath.empty() && relPath[0] == '.')
            relPath.erase(0, 1);

        std::stringstream ss(relPath);
        std::string seg;
        cJSON* node = parent;

        while (std::getline(ss, seg, '.')) {
            if (seg.empty()) continue;

            cJSON* next = cJSON_GetObjectItem(node, seg.c_str());
            if (!next) {
                next = cJSON_CreateObject();
                cJSON_AddItemToObject(node, seg.c_str(), next);
            }
            node = next;
        }

        // Add metadata
        if (unit) cJSON_AddStringToObject(node, "unit", unit);
    }

    sqlite3_finalize(stmt);

    char* jsonStr = cJSON_PrintUnformatted(root);
    std::string result = jsonStr ? jsonStr : "{}";
    cJSON_free(jsonStr);
    cJSON_Delete(root);

    return result;
}
void handleClientRequest(int clientSock) {
    char buffer[512];
    ssize_t n = read(clientSock, buffer, sizeof(buffer) - 1);
    if (n <= 0) {
        close(clientSock);
        return;
    }
    buffer[n] = '\0';
    std::string request(buffer);

    if (request.rfind("GET_SCHEMA", 0) == 0) {
        std::string prefix = request.substr(strlen("GET_SCHEMA"));
        // trim leading spaces
        prefix.erase(0, prefix.find_first_not_of(" \t"));
        std::string json = getSchemaAsJson(prefix);
        write(clientSock, json.c_str(), json.size());
    } else {
        std::string msg = "ERROR: Unknown command\n";
        write(clientSock, msg.c_str(), msg.size());
    }

    close(clientSock);
}

void startIpcServer() {
    const char* socket_path = "/tmp/vss_ipc_socket";
    unlink(socket_path);

    int serverSock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (serverSock < 0) {
        perror("socket");
        return;
    }

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, socket_path, sizeof(addr.sun_path) - 1);

    if (bind(serverSock, (sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(serverSock);
        return;
    }

    listen(serverSock, 5);
    std::cout << "VSS DataBroker IPC server running at " << socket_path << std::endl;

    while (true) {
        int clientSock = accept(serverSock, nullptr, nullptr);
        if (clientSock >= 0) {
            std::thread(handleClientRequest, clientSock).detach();
        }
    }

    close(serverSock);
}

void handleSchemaClientRequest(int clientSock) {
    char buffer[512];
    ssize_t n = read(clientSock, buffer, sizeof(buffer) - 1);
    if (n <= 0) {
        close(clientSock);
        return;
    }
    buffer[n] = '\0';
    std::string request(buffer);

    if (request.rfind("GET_SCHEMA", 0) == 0) {
        std::string prefix = request.substr(strlen("GET_SCHEMA"));
        prefix.erase(0, prefix.find_first_not_of(" \t"));
        std::string json = getSchemaAsJson(prefix);
        write(clientSock, json.c_str(), json.size());
    } else {
        std::string msg = "ERROR: Unknown command\n";
        write(clientSock, msg.c_str(), msg.size());
    }

    close(clientSock);
}

std::string handleSchemaRequest(const std::string &path)
{
    // Load schema JSON from DB for the given path
    std::string json = getSchemaAsJson(path);
    return json.empty() ? "{}" : json;
}

// This replaces the continuous thread server.
void startSchemaIpcServer()
{
    const char* socket_path = "/tmp/vss_ipc_socket";
    unlink(socket_path);

    int serverSock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (serverSock < 0) {
        perror("socket");
        return;
    }

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, socket_path, sizeof(addr.sun_path) - 1);

    if (bind(serverSock, (sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(serverSock);
        return;
    }

    if (listen(serverSock, 1) < 0) {
        perror("listen");
        close(serverSock);
        return;
    }

    std::cout << "[SchemaAPI] Waiting for one-time request...\n";
    int clientSock = accept(serverSock, nullptr, nullptr);
    if (clientSock < 0) {
        perror("accept");
        close(serverSock);
        return;
    }

    // Receive schema path
    char buffer[256];
    ssize_t n = read(clientSock, buffer, sizeof(buffer)-1);
    if (n > 0) {
        buffer[n] = '\0';
        std::string req(buffer);
        std::string path;
        if (req.rfind("GET_SCHEMA", 0) == 0) {
            path = req.substr(strlen("GET_SCHEMA "));
            std::string json = handleSchemaRequest(path);
            send(clientSock, json.c_str(), json.size(), 0);
        }
    }

    close(clientSock);
    close(serverSock);
    std::cout << "[SchemaAPI] Request served, server closed.\n";
}

#include <poll.h>

// Unified IPC event loop handling both data and schema sockets
void unifiedIpcServer()
{
    const char* data_path = "/tmp/vss_ipc_data";
    const char* schema_path = "/tmp/vss_ipc_socket";

    unlink(data_path);
    unlink(schema_path);

    // Create listening sockets for both data and schema
    int dataSock = socket(AF_UNIX, SOCK_STREAM, 0);
    int schemaSock = socket(AF_UNIX, SOCK_STREAM, 0);

    if (dataSock < 0 || schemaSock < 0) {
        perror("[IPC] socket");
        return;
    }

    // Common sockaddr setup
    sockaddr_un data_addr{}, schema_addr{};
    data_addr.sun_family = AF_UNIX;
    schema_addr.sun_family = AF_UNIX;
    strncpy(data_addr.sun_path, data_path, sizeof(data_addr.sun_path) - 1);
    strncpy(schema_addr.sun_path, schema_path, sizeof(schema_addr.sun_path) - 1);

    // Bind both sockets
    if (bind(dataSock, (sockaddr*)&data_addr, sizeof(data_addr)) < 0) {
        perror("[IPC] bind data");
        close(dataSock); close(schemaSock); return;
    }
    if (bind(schemaSock, (sockaddr*)&schema_addr, sizeof(schema_addr)) < 0) {
        perror("[IPC] bind schema");
        close(dataSock); close(schemaSock); return;
    }

    listen(dataSock, 8);
    listen(schemaSock, 4);

    std::cout << "[IPC] Unified server running:\n"
              << "   - Data Socket: " << data_path << "\n"
              << "   - Schema Socket: " << schema_path << std::endl;

    // Poll both sockets for activity
    struct pollfd fds[2];
    fds[0].fd = dataSock;
    fds[0].events = POLLIN;
    fds[1].fd = schemaSock;
    fds[1].events = POLLIN;

    while (true) {
        int ret = poll(fds, 2, -1);  // wait indefinitely
        if (ret < 0) {
            if (errno == EINTR) continue;
            perror("[IPC] poll");
            break;
        }

        // Handle new data connection
        if (fds[0].revents & POLLIN) {
            int client = accept(dataSock, nullptr, nullptr);
            if (client >= 0) {
                /*std::thread([client]() {
                    char buffer[1024];
                    ssize_t n = read(client, buffer, sizeof(buffer) - 1);
                    if (n > 0) {
                        buffer[n] = '\0';
                        std::string message(buffer);
                        std::cout << "[IPC] Received data: " << message << std::endl;
                        // Forward to DDS bridge or other consumers if needed
                        sendDataOverIPC(message);
                    }
                    close(client);
                }).detach();*/
            }
        }

        // Handle new schema connection
        if (fds[1].revents & POLLIN) {
            int client = accept(schemaSock, nullptr, nullptr);
            if (client >= 0) {
                std::thread([client]() {
                    char buffer[512];
                    ssize_t n = read(client, buffer, sizeof(buffer) - 1);
                    if (n <= 0) {
                        close(client);
                        return;
                    }
                    buffer[n] = '\0';
                    std::string request(buffer);

                    if (request.rfind("GET_SCHEMA", 0) == 0) {
                        std::string prefix = request.substr(strlen("GET_SCHEMA"));
                        prefix.erase(0, prefix.find_first_not_of(" \t"));
                        std::string json = getSchemaAsJson(prefix);
                        write(client, json.c_str(), json.size());
                        std::cout << "[SchemaAPI] Served schema for: " << prefix << std::endl;
                    } else {
                        std::string msg = "ERROR: Unknown command\n";
                        write(client, msg.c_str(), msg.size());
                    }
                    close(client);
                }).detach();
            }
        }
    }

    close(dataSock);
    close(schemaSock);
}


// ------------------- Main -------------------
// Example main that initializes DB, loads node IDs and starts background flush thread.
// In production, remove the simulation loop and call Middleware_someipreq() from your middleware.
int main() {
    cout << "Initializing (DB + mapping load)...\n";
    struct timespec t_start, t_end;
    clock_gettime(CLOCK_MONOTONIC, &t_start);

    if (sqlite3_open("vss_hierarchical.db", &db) != SQLITE_OK) {
        cerr << "Failed to open DB: " << sqlite3_errmsg(db) << endl;
        return 1;
    }

    if (!loadNodeIds()) {
        cerr << "Failed to load node IDs.\n";
        sqlite3_close(db);
        return 1;
    }

    clock_gettime(CLOCK_MONOTONIC, &t_end);
    cout << "Loaded " << nodeMap.size() << " nodes.\n";
    cout << " Database Load Time : " << elapsed_ms(t_start, t_end) << " ms\n\n";

    
    // Start background flush thread
    thread flushThread(flushThreadFunc, 2u);
    thread ipcThread(unifiedIpcServer);
    // ------------------------------
    // LOCAL TEST LOOP (optional)
    // If input.txt exists, we will feed lines to Middleware_someipreq() as a simple test harness.
    // Production AUTOSAR integration should not rely on this loop; instead call Middleware_someipreq()
    // directly from the SOME/IP request handler.
    // ------------------------------
    ifstream infile("input.txt");
    if (infile.good()) {
        cerr << "input.txt found. Running local test harness reading file and sending to Middleware_someipreq().\n";
        string line;
        while (getline(infile, line)) {
            RawData data;
            if (parseLine(line, data)) {
                Middleware_someipreq(data);
            }

            // Sleep small amount to emulate incoming messages
            this_thread::sleep_for(chrono::milliseconds(1000));
        }
        infile.close();
    } else {
        cerr << "input.txt not found\n";
    }


    // Shutdown
    ipcThread.join();
    g_runFlushThread.store(false);
    if (flushThread.joinable()) flushThread.join();
    // Final flush (synchronous)
    flushBuffer();

	
    // Example: fetch last 5 samples for Vehicle.Speed using middleware_databroker_timeseries
    /*
    auto points = middleware_databroker_timeseries("Vehicle.Chassis.Axle.Row1.Wheel.Right.Tire.Pressure", 50);
    for (auto& p : points) {
        cout << p.timestamp << " | " << p.value << " " << p.unit << endl;
    }

    //Example: fetch the latest entries
 
    auto latest = middleware_databroker_latest();
    for (const auto& entry : latest) {
        int nodeId = entry.first;
        const vector<TimeSeriesPoint>& points = entry.second;

        cout << "Node ID: " << nodeId << endl;
        for (const auto& p : points) {
            cout << "  " << p.timestamp << " | " << p.value << " " << p.unit << endl;
        }
    }
    */



    sqlite3_close(db);
    cout << "Shutdown complete.\n";
    return 0;
}

