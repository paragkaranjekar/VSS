// Auto-generated mapping header (binary search, no dynamic allocation)
#pragma once

#include <stdint.h>
#include <stddef.h>

typedef struct {
    const char* vss_path;
    size_t offset;
    const char* dtype;
    double scaling;
    const char* unit;
} MappingSignal;

typedef struct {
    uint16_t service_id;
    uint16_t method_id;
    size_t num_signals;
    const MappingSignal* signals;
} MappingEntry;

static const MappingSignal mappingSignals[8] = {
    {"Vehicle.Chassis.Axle.Row1.Wheel.Left.Tire.Pressure", 0, "uint16", 0.1, "kPa"},
    {"Vehicle.Chassis.Axle.Row1.Wheel.Right.Tire.Pressure", 2, "uint16", 0.1, "kPa"},
    {"Vehicle.Chassis.Axle.Row2.Wheel.Left.Tire.Pressure", 4, "uint16", 0.1, "kPa"},
    {"Vehicle.Chassis.Axle.Row2.Wheel.Right.Tire.Pressure", 6, "uint16", 0.1, "kPa"},
    {"Vehicle.Chassis.Brake.PedalPosition", 0, "uint8", 0.5, "percent"},
    {"Vehicle.Powertrain.TractionBattery.StateOfCharge.Current", 0, "uint8", 1, "percent"},
    {"Vehicle.Powertrain.TractionBattery.StateOfCharge.CurrentEnergy", 1, "uint16", 0.1, "kWh"},
    {"Vehicle.Powertrain.TractionBattery.StateOfCharge.Displayed", 3, "uint8", 1, "percent"},
};

static const MappingEntry mappingTable[3] = {
    { 0x1234, 0x0001, 4, &mappingSignals[0] },
    { 0x1234, 0x5678, 1, &mappingSignals[4] },
    { 0x5678, 0x0002, 3, &mappingSignals[5] },
};

#define MAPPING_TABLE_SIZE 3
