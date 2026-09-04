add_executable(cozip_taco_tests test_taco.c)
target_link_libraries(cozip_taco_tests PRIVATE cozip::cozip)
target_compile_features(cozip_taco_tests PRIVATE c_std_11)
target_compile_definitions(cozip_taco_tests PRIVATE
    COZIP_TEST_DIR="${CMAKE_CURRENT_BINARY_DIR}")

if(MSVC)
    target_compile_options(cozip_taco_tests PRIVATE
        /W4 /permissive- /D_CRT_SECURE_NO_WARNINGS)
else()
    target_compile_options(cozip_taco_tests PRIVATE
        -Wall -Wextra -Wpedantic -Wshadow)
endif()

add_test(NAME cozip_taco_c_api COMMAND cozip_taco_tests)

# Windows does not search the parent build directory for dependent DLLs when
# CTest launches an executable from build/tests.
if(WIN32)
    add_custom_command(TARGET cozip_taco_tests POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:cozip>
            $<TARGET_FILE_DIR:cozip_taco_tests>)
endif()
