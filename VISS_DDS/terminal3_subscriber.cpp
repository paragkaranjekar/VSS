#include <iostream>
#include <thread>
#include <chrono>
#include <string>
#include <cstring>
#include "dds/dds.h"

extern "C" {
#include "vss_topics.h"
}

#define SCHEMA_REQ_TOPIC "SchemaRequest"
#define SCHEMA_RES_TOPIC "SchemaResponse"
#define VSS_DATA_TOPIC   "VehicleData"

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

    std::cout << "[DDS] Listening for schema responses...\n";

    while (true)
    {
        SchemaResponse res{};
        void *samples[1] = {&res};
        dds_sample_info_t info{};
        dds_return_t rc = dds_take(reader, samples, &info, 1, 1);

        if (rc > 0 && info.valid_data)
        {
            std::cout << "\n✅ [DDS] Schema Response Received:\n";
            std::cout << res.json << "\n";
            std::cout << "--------------------------------------\n";
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

// -----------------------------------------------------------------------------
// Listen for live VehicleData
// -----------------------------------------------------------------------------
void listenVehicleData(dds_entity_t participant)
{
    dds_entity_t topic = dds_create_topic(participant, &VSSData_desc, VSS_DATA_TOPIC, nullptr, nullptr);
    dds_entity_t reader = dds_create_reader(participant, topic, nullptr, nullptr);

    std::cout << "[DDS] Subscribed to live VehicleData updates.\n";

    while (true)
    {
        VSSData data{};
        void *samples[1] = {&data};
        dds_sample_info_t info{};
        dds_return_t rc = dds_take(reader, samples, &info, 1, 1);

        if (rc > 0 && info.valid_data)
        {
            std::cout << "[DATA] " << data.path
                      << " = " << data.value
                      << " (ts=" << data.timestamp << ")" << std::endl;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

// -----------------------------------------------------------------------------
// MAIN
// -----------------------------------------------------------------------------
int main()
{
    std::cout << "[Terminal 3] Starting DDS Subscriber...\n";

    // Create participant
    dds_entity_t participant = dds_create_participant(DDS_DOMAIN_DEFAULT, nullptr, nullptr);
    if (participant < 0)
    {
        std::cerr << "Failed to create DDS participant.\n";
        return 1;
    }

    // Start data listener threads
    std::thread dataThread(listenVehicleData, participant);
    std::thread schemaThread(listenSchemaResponse, participant);

    // Wait for discovery
    std::cout << "[DDS] Waiting 3 seconds for DDS discovery...\n";
    std::this_thread::sleep_for(std::chrono::seconds(3));

    // Send schema request (example: Powertrain)
    sendSchemaRequest(participant, "Vehicle.Powertrain");

    dataThread.join();
    schemaThread.join();

    dds_delete(participant);
    return 0;
}

