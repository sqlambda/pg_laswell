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

# Warnings are errors for anyone developing this, and for CI. The one exception
# is a build from a release tarball: that happens on someone else's machine, with
# a compiler possibly newer than any this project has seen, and a diagnostic
# that compiler invented must not lock a user out of a release that was clean
# when it was cut. So the default follows the source -- ON in a git checkout,
# OFF without one -- and the warnings themselves stay on either way. CI, the
# release workflow and the FreeBSD build script pass it explicitly: a build
# that is green only because a default turned it off has not been checked.
if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/../../.git")
    set(_pglaswell_werror_default ON)
else()
    set(_pglaswell_werror_default OFF)
endif()
option(PGLASWELL_WERROR
    "Treat warnings as errors (default: ON in a git checkout, OFF in a release tarball)"
    ${_pglaswell_werror_default})
message(STATUS "pg_laswell warnings as errors: ${PGLASWELL_WERROR}")

find_program(VALGRIND_EXECUTABLE valgrind)

# Captured HERE, at include time. CMAKE_CURRENT_LIST_DIR is dynamically scoped:
# read inside a function it is the directory of the file that CALLED the
# function, not of this one, which resolved the exclusions file to a path that
# does not exist -- and an unreadable list produced an EMPTY gtest filter, which
# runs zero tests and reports success. A silent no-op is the worst outcome for
# a safety check, so the path is fixed at include time and its absence is fatal.
set(PGLASWELL_CMAKE_DIR "${CMAKE_CURRENT_LIST_DIR}")

# Binary hardening, probed once here rather than per target (each probe is a
# compile). Probed, not assumed, because this project builds on three systems
# and the flags are not all everywhere: -fcf-protection is x86 only, and ld64
# on macOS refuses -z. A flag the toolchain cannot take is dropped; what
# actually reached the binary is then ASSERTED, by test/hardening-check.sh --
# requested flags are an intention, the ELF headers are the fact.
include(CheckCXXCompilerFlag)
include(CheckLinkerFlag)
include(CheckPIESupported)
check_pie_supported(OUTPUT_VARIABLE _pglaswell_pie_msg LANGUAGES CXX)
if(NOT CMAKE_CXX_LINK_PIE_SUPPORTED)
    message(WARNING "PIE is not supported by this toolchain: ${_pglaswell_pie_msg}")
endif()
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Probed UNDER -Werror, whatever PGLASWELL_WERROR says. Apple clang accepts
# -fstack-clash-protection with only "argument unused during compilation" -- a
# warning, so a plain probe says yes, and a build with -Werror then fails on
# every file. Found on the first macOS build. A flag the compiler ignores is
# not hardening either way, so it is dropped in both cases.
set(_pglaswell_hardening_compile_flags "")
set(_pglaswell_saved_required_flags "${CMAKE_REQUIRED_FLAGS}")
set(CMAKE_REQUIRED_FLAGS "${CMAKE_REQUIRED_FLAGS} -Werror")
foreach(_flag -fstack-protector-strong -fstack-clash-protection -fcf-protection)
    string(MAKE_C_IDENTIFIER "PGLASWELL_HAS_WERROR${_flag}" _var)
    check_cxx_compiler_flag(${_flag} ${_var})
    if(${_var})
        list(APPEND _pglaswell_hardening_compile_flags ${_flag})
    endif()
endforeach()
set(CMAKE_REQUIRED_FLAGS "${_pglaswell_saved_required_flags}")
set(_pglaswell_hardening_link_flags "")
foreach(_flag -Wl,-z,relro -Wl,-z,now -Wl,-z,noexecstack)
    string(MAKE_C_IDENTIFIER "PGLASWELL_HAS${_flag}" _var)
    check_linker_flag(CXX ${_flag} ${_var})
    if(${_var})
        list(APPEND _pglaswell_hardening_link_flags ${_flag})
    endif()
endforeach()

# Always on: warnings as errors, hardened standard library, hardened binary.
function(pglaswell_harden target)
    target_compile_options(${target} PRIVATE
        -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion
        -Wuninitialized -Wshadow
    )
    if(PGLASWELL_WERROR)
        target_compile_options(${target} PRIVATE -Werror)
    endif()
    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        # ~zero-cost bounds/precondition checks in libstdc++ (e.g. vector::operator[]).
        target_compile_definitions(${target} PRIVATE _GLIBCXX_ASSERTIONS)
    elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        target_compile_definitions(${target} PRIVATE _LIBCPP_HARDENING_MODE=_LIBCPP_HARDENING_MODE_FAST)
    endif()

    # _FORTIFY_SOURCE needs optimisation (glibc emits a #warning at -O0, fatal
    # under -Werror) and fights the sanitizers' interceptors, so it is applied
    # only to optimised builds without one. -U first, so a packager's own
    # -D_FORTIFY_SOURCE=2 does not trip a redefinition warning. Level 3 on
    # Linux, where glibc has it; 2 elsewhere, which every libc here understands.
    if(PGLASWELL_SANITIZER STREQUAL "NONE")
        if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
            set(_fortify 3)
        else()
            set(_fortify 2)
        endif()
        target_compile_options(${target} PRIVATE
            $<$<CONFIG:Release,RelWithDebInfo,MinSizeRel>:-U_FORTIFY_SOURCE>
            $<$<CONFIG:Release,RelWithDebInfo,MinSizeRel>:-D_FORTIFY_SOURCE=${_fortify}>)
    endif()
    target_compile_options(${target} PRIVATE ${_pglaswell_hardening_compile_flags})
    target_link_options(${target} PRIVATE ${_pglaswell_hardening_link_flags})
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
