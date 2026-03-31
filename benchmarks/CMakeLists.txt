add_executable(bench_native bench_native.cpp)
target_link_libraries(bench_native PRIVATE fastpdf2png_lib)
target_include_directories(bench_native PRIVATE ${CMAKE_SOURCE_DIR}/include)

# Don't auto-run benchmarks in CTest
