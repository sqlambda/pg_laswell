# SafetyFlags.cmake — static hygiene (always on) + dynamic bug detection (opt-in).
#
# Warnings and hardened-stdlib defines catch what the compiler can prove at
# compile time: uninitialized reads, narrowing conversions, shadowing. They
# cannot see use-after-free, double-free, data races, or signed overflow —
# those only exist at runtime, on whichever execution path a test happens to
# take. Sanitizers and Valgrind cover that dynamic axis; pglaswell_harden()
# below is unconditional, pglaswell_apply_sanitizers() is selected per build
# via PGLASWELL_SANITIZER, and pglaswell_add_valgrind_test() gives every test
# binary a Valgrind-wrapped ctest entry for free when sanitizers are off.
#
# Adapted from pg_licht's file of the same name. One difference in emphasis
# worth naming: THREAD matters far more here. pg_licht is a single-threaded
# getline loop and TSAN is belt-and-braces for it; this project runs a worker
# thread per job plus one process-wide observer thread against a shared
# JobRegistry, so TSAN is a gate rather than a nicety.

set(PGLASWELL_SANITIZER "NONE" CACHE STRING
    "Sanitizer to build with: NONE, ADDRESS, UNDEFINED, ADDRESS_UNDEFINED, THREAD")
set_property(CACHE PGLASWELL_SANITIZER PROPERTY STRINGS
    NONE ADDRESS UNDEFINED ADDRESS_UNDEFINED THREAD)

find_program(VALGRIND_EXECUTABLE valgrind)

# Always-on static hygiene: warnings-as-errors + hardened standard library.
function(pglaswell_harden target)
    target_compile_options(${target} PRIVATE
        -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion
        -Wuninitialized -Wshadow -Werror
    )
    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        # ~zero-cost bounds/precondition checks in libstdc++ (e.g. vector::operator[]).
        target_compile_definitions(${target} PRIVATE _GLIBCXX_ASSERTIONS)
    elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        target_compile_definitions(${target} PRIVATE _LIBCPP_HARDENING_MODE=_LIBCPP_HARDENING_MODE_FAST)
    endif()
endfunction()

# Opt-in dynamic bug detection, selected via -DPGLASWELL_SANITIZER=<value>.
function(pglaswell_apply_sanitizers target)
    if(PGLASWELL_SANITIZER STREQUAL "NONE")
        return()
    elseif(PGLASWELL_SANITIZER STREQUAL "ADDRESS")
        set(_flags -fsanitize=address)
    elseif(PGLASWELL_SANITIZER STREQUAL "UNDEFINED")
        set(_flags -fsanitize=undefined)
    elseif(PGLASWELL_SANITIZER STREQUAL "ADDRESS_UNDEFINED")
        set(_flags -fsanitize=address,undefined)
    elseif(PGLASWELL_SANITIZER STREQUAL "THREAD")
        set(_flags -fsanitize=thread)
    else()
        message(FATAL_ERROR "Unknown PGLASWELL_SANITIZER: ${PGLASWELL_SANITIZER}")
    endif()

    # Tests that assert on WALL-CLOCK RATIOS need to know. An instrumented build
    # measures the sanitizer as much as it measures PostgreSQL, so a ratio that
    # holds on a plain build is noise here -- and a test that fails for that
    # reason teaches people to ignore the sanitizer jobs.
    target_compile_definitions(${target} PRIVATE PGLASWELL_SANITIZER_ACTIVE=1)
    target_compile_options(${target} PRIVATE -g -fno-omit-frame-pointer ${_flags})
    target_link_options(${target} PRIVATE ${_flags})
endfunction()

# Registers a Valgrind-wrapped ctest entry for `target`. Skipped when a
# sanitizer is active (an instrumented binary run under Valgrind produces
# instrumentation conflicts, not real findings) or when Valgrind is absent.
function(pglaswell_add_valgrind_test target)
    if(NOT VALGRIND_EXECUTABLE)
        message(STATUS "Valgrind not found — skipping ${target}_valgrind test")
        return()
    endif()
    if(NOT PGLASWELL_SANITIZER STREQUAL "NONE")
        message(STATUS "PGLASWELL_SANITIZER=${PGLASWELL_SANITIZER} active — skipping ${target}_valgrind test")
        return()
    endif()
    add_test(NAME ${target}_valgrind
        COMMAND ${VALGRIND_EXECUTABLE} --error-exitcode=99 --leak-check=full --track-origins=yes
                $<TARGET_FILE:${target}>
    )
endfunction()
