set(COZIP_TESTS cozip_taco_tests:test_taco.c:cozip_taco_c_api)
if(COZIP_READER)
    list(APPEND COZIP_TESTS cozip_read_tests:test_read.c:cozip_read_c_api)
endif()

foreach(test_spec IN LISTS COZIP_TESTS)
    string(REPLACE ":" ";" test_parts "${test_spec}")
    list(GET test_parts 0 test_target)
    list(GET test_parts 1 test_source)
    list(GET test_parts 2 test_name)

    add_executable(${test_target} ${test_source})
    target_link_libraries(${test_target} PRIVATE cozip::cozip)
    target_compile_features(${test_target} PRIVATE c_std_11)
    target_compile_definitions(${test_target} PRIVATE
        COZIP_TEST_DIR="${CMAKE_CURRENT_BINARY_DIR}")

    if(MSVC)
        target_compile_options(${test_target} PRIVATE
            /W4 /permissive- /D_CRT_SECURE_NO_WARNINGS)
    else()
        target_compile_options(${test_target} PRIVATE
            -Wall -Wextra -Wpedantic -Wshadow)
    endif()

    add_test(NAME ${test_name} COMMAND ${test_target})

    # Windows does not search the parent build directory for dependent DLLs
    # when CTest launches an executable from build/tests.
    if(WIN32)
        add_custom_command(TARGET ${test_target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:cozip>
                $<TARGET_FILE_DIR:${test_target}>)
    endif()
endforeach()
