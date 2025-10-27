cmake_minimum_required(VERSION 3.10)
project(VSS_DDS_Apps LANGUAGES C CXX)

# Make sure we can find your Cyclone DDS install
find_package(CycloneDDS REQUIRED)

# Add the IDL-generated code (shared by both apps)
add_library(vss_topics STATIC
    vss_topics.c
)
target_include_directories(vss_topics PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

# ---------------------------------------------------------------------------
# Terminal 1  (Bridge)
# ---------------------------------------------------------------------------
add_executable(terminal1_dds_bridge
    vss_dds_bridge.cpp
)
target_link_libraries(terminal1_dds_bridge
    vss_topics
    CycloneDDS::ddsc
    pthread
)

# ---------------------------------------------------------------------------
# Terminal 3  (Subscriber)
# ---------------------------------------------------------------------------
add_executable(terminal3_subscriber
    terminal3_subscriber.cpp
)
target_link_libraries(terminal3_subscriber
    vss_topics
    CycloneDDS::ddsc
    pthread
)

add_executable(gen_sub gen_sub.cpp)
target_link_libraries(gen_sub vss_topics CycloneDDS::ddsc pthread)


# Optional: install targets (handy if you want to 'make install')
install(TARGETS terminal1_dds_bridge terminal3_subscriber
        RUNTIME DESTINATION bin)

