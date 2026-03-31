include(FetchContent)
FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG v1.15.2
)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

# Unit tests
add_executable(test_png_writer unit/test_png_writer.cpp)
target_link_libraries(test_png_writer PRIVATE GTest::gtest_main fastpdf2png_lib)
target_include_directories(test_png_writer PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/third_party/stb)
fp2p_apply_simd(test_png_writer)

# Integration tests (need a PDF fixture)
add_executable(test_roundtrip integration/test_roundtrip.cpp)
target_link_libraries(test_roundtrip PRIVATE GTest::gtest_main fastpdf2png_lib)
target_include_directories(test_roundtrip PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/third_party/stb)

add_executable(test_production integration/test_production.cpp)
target_link_libraries(test_production PRIVATE GTest::gtest_main fastpdf2png_lib)
target_include_directories(test_production PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/third_party/stb)

include(GoogleTest)
gtest_discover_tests(test_png_writer)

# Only discover integration tests if fixture exists
if(EXISTS "${CMAKE_SOURCE_DIR}/bench_sample.pdf")
    gtest_discover_tests(test_roundtrip PROPERTIES ENVIRONMENT "TEST_PDF=${CMAKE_SOURCE_DIR}/bench_sample.pdf")
    gtest_discover_tests(test_production PROPERTIES ENVIRONMENT "TEST_PDF=${CMAKE_SOURCE_DIR}/bench_sample.pdf")
endif()

# Python tests
find_package(Python3 COMPONENTS Interpreter)
if(Python3_FOUND)
    add_test(NAME python_tests
        COMMAND ${Python3_EXECUTABLE} -m pytest ${CMAKE_SOURCE_DIR}/tests/python/ -v
    )
    set_tests_properties(python_tests PROPERTIES
        ENVIRONMENT "FASTPDF2PNG_BIN=$<TARGET_FILE:fastpdf2png_bin>"
        TIMEOUT 120
    )
endif()
