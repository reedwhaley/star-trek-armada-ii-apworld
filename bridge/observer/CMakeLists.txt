cmake_minimum_required(VERSION 3.20)
project(armada2_observer LANGUAGES CXX)

add_library(armada2_observer SHARED observer.cpp)
target_compile_features(armada2_observer PRIVATE cxx_std_20)
target_compile_definitions(armada2_observer PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(armada2_observer PRIVATE bcrypt)
set_target_properties(armada2_observer PROPERTIES OUTPUT_NAME "armada2_observer")
