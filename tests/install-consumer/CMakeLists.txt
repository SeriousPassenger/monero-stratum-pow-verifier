cmake_minimum_required(VERSION 3.16)
project(mspv-install-consumer LANGUAGES C CXX)

find_package(monero-stratum-pow-verifier 0.1.0 EXACT CONFIG REQUIRED)

add_executable(consumer_c main.c)
target_compile_features(consumer_c PRIVATE c_std_99)
target_link_libraries(consumer_c PRIVATE mspv::verifier)

add_executable(consumer_cpp main.cpp)
target_compile_features(consumer_cpp PRIVATE cxx_std_17)
target_link_libraries(consumer_cpp PRIVATE mspv::verifier)
