# ── test targets ─────────────────────────────────────────────────────────────

function(add_puzzpool_test name)
    add_executable(${name} ${name}.cpp)
    target_link_libraries(${name} PRIVATE puzzpool_core Catch2::Catch2WithMain)
    target_compile_features(${name} PRIVATE cxx_std_20)
    target_include_directories(${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    add_test(NAME ${name} COMMAND ${name})
endfunction()

add_puzzpool_test(test_hex_bigint)
add_puzzpool_test(test_permutation)
add_puzzpool_test(test_submission)
add_puzzpool_test(test_allocator)
add_puzzpool_test(test_vchunk)
add_puzzpool_test(test_handler_validation)
add_puzzpool_test(test_config)
add_puzzpool_test(test_puzzle_status)
add_puzzpool_test(test_visualization)
add_puzzpool_test(test_hash_utils)
add_puzzpool_test(test_http_client)
add_puzzpool_test(test_log_redaction)
add_puzzpool_test(test_auth)
add_puzzpool_test(test_auth_routes)

# ── Shell-based supply-chain guard test ──────────────────────────────────────
# tests/test_check_node_version_age.sh exercises scripts/check-node-version-age.sh
# with mocked `node --version` and `curl`, so it makes no network calls. It is
# registered here so `ctest` runs it as part of the ordinary suite instead of
# requiring a separate shell invocation.

find_program(PUZZPOOL_BASH_EXECUTABLE bash)
find_program(PUZZPOOL_NODE_EXECUTABLE node)
find_program(PUZZPOOL_CURL_EXECUTABLE curl)

if(PUZZPOOL_BASH_EXECUTABLE AND PUZZPOOL_NODE_EXECUTABLE)
    add_test(NAME test_check_node_version_age
        COMMAND "${PUZZPOOL_BASH_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/test_check_node_version_age.sh"
    )
    set_tests_properties(test_check_node_version_age PROPERTIES
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    )
else()
    message(WARNING
        "Skipping test_check_node_version_age: bash and node are both required "
        "(bash='${PUZZPOOL_BASH_EXECUTABLE}', node='${PUZZPOOL_NODE_EXECUTABLE}')."
    )
endif()

# ── Route-wiring smoke test ──────────────────────────────────────────────────
# Starts the real server and checks that every admin route is behind the guard
# and that /api/v1/auth/* is reachable and fails closed. The unit suites cover
# the authorization decision; only this covers main.cpp's wiring of it.

if(PUZZPOOL_BASH_EXECUTABLE AND PUZZPOOL_CURL_EXECUTABLE)
    add_test(NAME test_admin_routes_smoke
        COMMAND "${PUZZPOOL_BASH_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/test_admin_routes_smoke.sh"
                "$<TARGET_FILE:puzzpool>"
    )
    set_tests_properties(test_admin_routes_smoke PROPERTIES
        # Binds a fixed local port, so it must not overlap another test run.
        RUN_SERIAL TRUE
        TIMEOUT 120
    )
else()
    message(WARNING
        "Skipping test_admin_routes_smoke: bash and curl are both required "
        "(bash='${PUZZPOOL_BASH_EXECUTABLE}', curl='${PUZZPOOL_CURL_EXECUTABLE}')."
    )
endif()
