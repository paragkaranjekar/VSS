#include <iostream>
#include <cstring>
#include <thread>
#include <chrono>
#include "dds/dds.h"
#include <nlohmann/json.hpp>

extern "C" {
#include "vss_topics.h"
}

using json = nlohmann::json;

#define VSS_DATA_TOPIC "VehicleData"
#define SCHEMA_REQ_TOPIC "SchemaRequest"
#define SCHEMA_RES_TOPIC "SchemaResponse"

// -----------------------------------------------------------------------------
// Helper: check if path belongs to the requested subtree
// -----------------------------------------------------------------------------
bool isMatchingPath(const std::string &path, const std::string &filterPath)
{
    return path.rfind(filterPath, 0) == 0;  // prefix match
}

// -----------------------------------------------------------------------------
// Subscribe to VehicleData and filter by given path
// -----------------------------------------------------------------------------
void listenFilteredData(dds_entity_t participant, const std::string &filterPath)
{
    dds_entity_t topic = dds_create_topic(
        participant, &VSSData_desc, VSS_DATA_TOPIC, nullptr, nullptr);
    dds_entity_t reader = dds_create_reader(participant, topic, nullptr, nullptr);

    std::cout << "[DDS] Subscribed to VehicleData (filter: " << filterPath << ")\n";

    while (true)
    {
        VSSData data{};
        void *samples[1] = {&data};
        dds_sample_info_t info{};
        dds_return_t rc = dds_take(reader, samples, &info, 1, 1);

        if (rc > 0 && info.valid_data)
        {
            try {
                json j = json::parse(data.payload);

                std::string path = j.value("path", "unknown");
                double value = j.value("value", 0.0);
                std::string timestamp = j.value("timestamp", "N/A");

                if (isMatchingPath(path, filterPath))
                {
                    std::cout << "[DATA] " << path
                              << " = " << value
                              << " (timestamp: " << timestamp << ")\n";
                }
            }
            catch (const std::exception &e) {
                std::cerr << "[Error] Failed to parse JSON payload: " << e.what()
                          << "\nRaw payload: " << data.payload << "\n";
            }
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

// -----------------------------------------------------------------------------
// Send schema request
// -----------------------------------------------------------------------------
void sendSchemaRequest(dds_entity_t participant, const std::string &path)
{
    dds_entity_t topic = dds_create_topic(participant, &SchemaRequest_desc, SCHEMA_REQ_TOPIC, nullptr, nullptr);
    dds_entity_t writer = dds_create_writer(participant, topic, nullptr, nullptr);

    SchemaRequest req{};
    strncpy(req.path, path.c_str(), sizeof(req.path) - 1);

    std::cout << "[DDS] Sending schema request for: " << req.path << std::endl;
    dds_write(writer, &req);
}

// -----------------------------------------------------------------------------
// Listen for schema responses
// -----------------------------------------------------------------------------
void listenSchemaResponse(dds_entity_t participant)
{
    dds_entity_t topic = dds_create_topic(participant, &SchemaResponse_desc, SCHEMA_RES_TOPIC, nullptr, nullptr);
    dds_entity_t reader = dds_create_reader(participant, topic, nullptr, nullptr);

    SchemaResponse res{};
    void *samples[1] = {&res};
    dds_sample_info_t info{};

    std::cout << "[DDS] Waiting for schema response..." << std::endl;

    for (int i = 0; i < 50; ++i)  // wait ~5s max
    {
        dds_return_t rc = dds_take(reader, samples, &info, 1, 1);
        if (rc > 0 && info.valid_data)
        {
            std::cout << "\n✅ [DDS] Received Schema Response:\n";
            std::cout << res.json << "\n";
            std::cout << "--------------------------------------\n";
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    std::cout << "❌ [DDS] No schema response received (timeout)\n";
}

// -----------------------------------------------------------------------------
// MAIN
// -----------------------------------------------------------------------------
int main(int argc, char **argv)
{
    std::string filterPath;

    if (argc > 1)
        filterPath = argv[1];
    else
        filterPath = "Vehicle.Chassis";  // default filter if none provided

    std::cout << "[Generic DDS Subscriber] Starting...\n";
    std::cout << "[DDS] Filter Path: " << filterPath << "\n";

    dds_entity_t participant = dds_create_participant(DDS_DOMAIN_DEFAULT, nullptr, nullptr);
    if (participant < 0)
    {
        std::cerr << "Failed to create DDS participant.\n";
        return 1;
    }
    
    sendSchemaRequest(participant, filterPath);
    listenSchemaResponse(participant);

    listenFilteredData(participant, filterPath);

    dds_delete(participant);
    return 0;
}

