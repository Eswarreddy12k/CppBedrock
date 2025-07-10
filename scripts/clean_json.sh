#!/bin/bash
# filepath: /Users/eswar/Downloads/CppBedrock/scripts/clean_json.sh

BUILD_DIR="/Users/eswar/Downloads/CppBedrock/build"

find "$BUILD_DIR" -type f -name "*.json" -exec rm -v {} \;