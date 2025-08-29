cmake_minimum_required(VERSION 3.14) 
project(CppBedrock VERSION 1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Manually specify the include path for yaml-cpp
include_directories("/opt/homebrew/Cellar/yaml-cpp/0.8.0/include")
# Find nlohmann json package
find_package(nlohmann_json REQUIRED)
find_package(OpenSSL REQUIRED)

# Manually specify the library path for yaml-cpp
link_directories("/opt/homebrew/Cellar/yaml-cpp/0.8.0/lib")

include_directories(include)

# Add source files for your project
set(SOURCES
    src/main.cpp
    src/core/state/StateMachine.cpp
    src/core/state/DataSet.cpp
    src/core/state/EntityState.cpp
    src/core/events/Event.cpp
    src/core/events/EventFactory.cpp
    src/core/Entity.cpp
    src/coordination/connections/TcpConnection.cpp
    src/coordination/CoordinationServer.cpp
    src/coordination/CoordinationUnit.cpp
    src/core/TimeKeeper.cpp
    src/core/crypto/OpenSSLCryptoProvider.cpp
    src/core/crypto/CryptoUtils.cpp
    src/core/events/EventFactoryProtocolSpecific.cpp
    src/utils/Benchmark.cpp
)

# Add the library and link libraries
add_library(CppBedrockLib ${SOURCES})
target_link_libraries(CppBedrockLib PUBLIC OpenSSL::Crypto yaml-cpp nlohmann_json::nlohmann_json)
target_include_directories(CppBedrockLib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

# Add the executable and link libraries
add_executable(${PROJECT_NAME} src/main.cpp)
target_link_libraries(${PROJECT_NAME} PRIVATE CppBedrockLib)

# Add integration test executable and link libraries
add_executable(integration_test tests/integration_test.cpp)
target_link_libraries(integration_test PRIVATE CppBedrockLib)
