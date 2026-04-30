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
