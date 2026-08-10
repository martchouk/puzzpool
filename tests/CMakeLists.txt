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
add_puzzpool_test(test_session)
add_puzzpool_test(test_admin_auth)

# ── Admin route guard regression (end to end) ────────────────────────────────
# This one is not a Catch2 target on purpose. The admin route wiring lives in
# src/main.cpp, which is outside puzzpool_core and therefore invisible to every
# target above: a route that loses its adminGuard() line leaves the whole Catch2
# suite green while being publicly reachable. The script drives the real server
# binary over HTTP instead. RUN_SERIAL because it binds a fixed port.
add_test(NAME test_admin_routes_guarded
         COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/test_admin_routes_guarded.sh)
set_tests_properties(test_admin_routes_guarded PROPERTIES
    ENVIRONMENT "SERVER_BIN=$<TARGET_FILE:puzzpool>"
    RUN_SERIAL TRUE)
