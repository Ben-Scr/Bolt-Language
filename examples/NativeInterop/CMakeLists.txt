cmake_minimum_required(VERSION 3.21)
project(BoltInteropExample LANGUAGES C CXX)
add_library(example STATIC ffi.c)
set_target_properties(example PROPERTIES ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
