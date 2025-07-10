#!/bin/bash

set -e

CONFIGS=(
    "config.entities.byzantine.yaml"
    "config.entities.normal.yaml"
)

SCENARIOS=(1 2 3 4)

# 2. Test scenario 1 with all configs
for CONFIG in "${CONFIGS[@]}"; do
    echo "=============================================="
    echo "Using config: $CONFIG"
    cp "config/$CONFIG" config/config.entities.yaml

    echo "Cleaning JSON files..."
    ./scripts/clean_json.sh

    echo "Starting CppBedrock nodes for config $CONFIG..."
    cd build
    ./CppBedrock &
    BEDROCK_PID=$!
    cd ..

    echo "Waiting for nodes to initialize..."
    sleep 5

    echo "Running integration_test with scenario 1 and config $CONFIG..."
    cd build
    ./integration_test 1
    cd ..

    echo "Stopping CppBedrock nodes for config $CONFIG..."
    kill $BEDROCK_PID
    sleep 2
    pkill -f CppBedrock || true

    echo "Config $CONFIG complete."
    echo "=============================================="
done

# 1. Test all scenarios with the current config
for SCENARIO in "${SCENARIOS[@]}"; do
    echo "=============================================="
    echo "Cleaning JSON files for scenario $SCENARIO..."
    ./scripts/clean_json.sh

    echo "Starting CppBedrock nodes for scenario $SCENARIO..."
    cd build
    ./CppBedrock &
    BEDROCK_PID=$!
    cd ..

    echo "Waiting for nodes to initialize..."
    sleep 5

    echo "Running integration_test with scenario $SCENARIO..."
    cd build
    ./integration_test $SCENARIO
    cd ..

    echo "Stopping CppBedrock nodes for scenario $SCENARIO..."
    kill $BEDROCK_PID
    sleep 2
    pkill -f CppBedrock || true

    echo "Scenario $SCENARIO complete."
    echo "=============================================="
done


echo "Automation complete."