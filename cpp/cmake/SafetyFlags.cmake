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

# Captured HERE, at include time. CMAKE_CURRENT_LIST_DIR is dynamically scoped:
# read inside a function it is the directory of the file that CALLED the
# function, not of this one, which resolved the exclusions file to a path that
# does not exist -- and an unreadable list produced an EMPTY gtest filter, which
# runs zero tests and reports success. A silent no-op is the worst outcome for
# a safety check, so the path is fixed at include time and its absence is fatal.
set(PGLASWELL_CMAKE_DIR "${CMAKE_CURRENT_LIST_DIR}")

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
    # The exclusions come from a file both this and tests.yml read, so the two
    # cannot disagree about what Valgrind covers. See the file for why each is
    # there.
    set(_excl "${PGLASWELL_CMAKE_DIR}/../test/valgrind-slow-tests.txt")
    if(NOT EXISTS "${_excl}")
        message(FATAL_ERROR "missing ${_excl}: without it the Valgrind filter is "
                            "empty, which runs no tests and reports success")
    endif()
    set(_filter "")
    file(STRINGS "${_excl}" _lines)
    foreach(_l IN LISTS _lines)
        string(STRIP "${_l}" _l)
        if(_l AND NOT _l MATCHES "^#")
            if(_filter)
                set(_filter "${_filter}:${_l}")
            else()
                set(_filter "-${_l}")
            endif()
        endif()
    endforeach()
    if(NOT _filter)
        message(FATAL_ERROR "${_excl} lists no tests; an empty filter runs none")
    endif()
    message(STATUS "Valgrind excludes: ${_filter}")
    add_test(NAME ${target}_valgrind
        COMMAND ${VALGRIND_EXECUTABLE} --error-exitcode=99 --leak-check=full --track-origins=yes
                $<TARGET_FILE:${target}> "--gtest_filter=${_filter}"
    )
endfunction()
