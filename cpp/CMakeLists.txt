cmake_minimum_required(VERSION 3.20)
project(pg_laswell_mcp VERSION 0.1.2 LANGUAGES CXX)

# Standard bin/ and share/man/ layout, so `cmake --install` produces a tree that
# can be dropped onto a prefix as-is: the release tarballs are staged this way,
# CPack lays the same tree under /usr for the deb and rpm, and share/man/man1 is
# already on the default MANPATH, so `man pg_laswell_mcp` works with no further
# setup. sql/bootstrap.sql goes to share/pg_laswell/ because a DBA has to run it
# by hand before this binary can do anything (see README, "Bootstrap").
include(GNUInstallDirs)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(PkgConfig REQUIRED)
# ONE libpqxx: 8.0.2, the version CI and every release build from source
# (PQXX_VERSION in .github/workflows/*.yml -- change them together). Not "any
# libpqxx the system has": that is how code written against the system's 7.10
# compiled here and failed in CI, where sql_error::sqlstate() is a string_view.
# A pinned version is written to its full capability, never to the part two
# versions happen to share.
#
# Locally, build it the way CI does and point CMake at it:
#   sh .github/scripts/build-libpqxx.sh 8.0.2 <PQXX_SHA256> $HOME/.local/opt/libpqxx-8.0.2
#   cmake -S cpp -B cpp/build -DCMAKE_PREFIX_PATH=$HOME/.local/opt/libpqxx-8.0.2
set(PGLASWELL_PQXX_VERSION "8.0.2")
pkg_check_modules(PQXX IMPORTED_TARGET libpqxx=${PGLASWELL_PQXX_VERSION})
if(NOT PQXX_FOUND)
    pkg_check_modules(_PQXX_ANY QUIET libpqxx)
    if(_PQXX_ANY_FOUND)
        set(_pqxx_found "found libpqxx ${_PQXX_ANY_VERSION}")
    else()
        set(_pqxx_found "found no libpqxx")
    endif()
    message(FATAL_ERROR
        "pg_laswell builds against libpqxx ${PGLASWELL_PQXX_VERSION} exactly, and "
        "${_pqxx_found}. Build the pinned one the way CI does and point CMake at "
        "it (see BUILD.md):\n"
        "  sh .github/scripts/build-libpqxx.sh ${PGLASWELL_PQXX_VERSION} <PQXX_SHA256 "
        "from .github/workflows/tests.yml> $HOME/.local/opt/libpqxx-${PGLASWELL_PQXX_VERSION}\n"
        "  cmake -S cpp -B <build> -DCMAKE_PREFIX_PATH=$HOME/.local/opt/libpqxx-${PGLASWELL_PQXX_VERSION}")
endif()
# libpqxx's .pc only exposes libpq via Requires.private, which CMake's
# IMPORTED_TARGET does not follow (it only reads the public Libs/Requires).
# When libpqxx is linked statically this leaves every PQ* symbol unresolved
# at final link time, so link libpq explicitly via its own module — this is
# a no-op when libpqxx is a shared library (libpq is already a transitive
# runtime dependency there) and required when it's static.
pkg_check_modules(LIBPQ REQUIRED IMPORTED_TARGET libpq)
find_package(nlohmann_json REQUIRED)
# Ed25519 signature verification. Crypto only, never OpenSSL::SSL: this binary
# does not speak TLS itself (libpq does that). Deliberately not libsodium —
# libcrypto is already loaded into this process because libpq5 depends on
# libssl3t64, so using it adds no new *runtime* package on any platform, only
# libssl-dev at build time. The trade is ~40 lines of EVP glue, once, against a
# permanent packaging obligation on every distro we ship to.
find_package(OpenSSL REQUIRED)

include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/SafetyFlags.cmake)

set(PGLASWELL_LIBS
    nlohmann_json::nlohmann_json
    PkgConfig::PQXX
    PkgConfig::LIBPQ
    OpenSSL::Crypto
)

# Two binaries, one core. pg_laswell_mcp is for AUTHORING a migration -- an
# agent plans it, reads the reasoning and decides. pg_laswell APPLIES one on a
# release pipeline, where there is nobody to ask. deploy.h explains why there is
# nothing to ask: pacing is measured, ordering is declared in the signed spec,
# and the repository's concurrency grouping is already the conservative one.
# --------------------------------------------------------------------------
# Vendor modules. Empty by default: the core binary is PostgreSQL-only, because
# a kind that is compiled in and fails on every server without some extension is
# within touching distance of the "recognized but unsupported" state spec.h
# opens by forbidding. See src/modules/README.md.
#
#   cmake -S cpp -B cpp/build -DPGLASWELL_MODULES=citus
# --------------------------------------------------------------------------
set(PGLASWELL_MODULES "" CACHE STRING "Vendor modules to compile in, e.g. citus")
set(_mod_names "")

set(_gen_dir ${CMAKE_CURRENT_BINARY_DIR}/generated/modules)
file(MAKE_DIRECTORY ${_gen_dir})
set(_kinds_inc "// Generated from PGLASWELL_MODULES. Do not edit.\n")
set(_heads_inc "// Generated from PGLASWELL_MODULES. Do not edit.\n")
set(_keys_inc  "// Generated from PGLASWELL_MODULES. Do not edit.\n")
set(_plan_inc  "// Generated from PGLASWELL_MODULES. Do not edit.\n")
set(_obs_inc   "// Generated from PGLASWELL_MODULES. Do not edit.\n")
set(_obs_head  "// Generated from PGLASWELL_MODULES. Do not edit.\n")
set(_proj_inc  "// Generated from PGLASWELL_MODULES. Do not edit.\n")
set(_proj_head "// Generated from PGLASWELL_MODULES. Do not edit.\n")
set(_test_inc  "// Generated from PGLASWELL_MODULES. Do not edit.\n")
set(_topo_inc  "// Generated from PGLASWELL_MODULES. Do not edit.\n")
set(_guard_inc "// Generated from PGLASWELL_MODULES. Do not edit.\n")
set(_guard_head "// Generated from PGLASWELL_MODULES. Do not edit.\n")
set(_confine_inc  "// Generated from PGLASWELL_MODULES. Do not edit.\n")
set(_confine_head "// Generated from PGLASWELL_MODULES. Do not edit.\n")
set(_ptest_inc "// Generated from PGLASWELL_MODULES. Do not edit.\n")
set(_body_inc  "// Generated from PGLASWELL_MODULES. Do not edit.\n")
foreach(_mod IN LISTS PGLASWELL_MODULES)
    if(NOT EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/src/modules/${_mod}/kinds.inc)
        message(FATAL_ERROR
            "PGLASWELL_MODULES names '${_mod}', but src/modules/${_mod}/kinds.inc "
            "does not exist. A misspelled module must not silently produce a "
            "binary missing the kinds someone asked for.")
    endif()
    string(APPEND _kinds_inc "#include \"modules/${_mod}/kinds.inc\"\n")
    string(APPEND _heads_inc "#include \"modules/${_mod}/module.h\"\n")
    string(APPEND _plan_inc "#include \"modules/${_mod}/plan.h\"\n")
    string(APPEND _obs_inc  "PGLASWELL_OBSERVE(\"${_mod}\", k${_mod}PresentSql, k${_mod}ObservationSql)\n")
    string(APPEND _obs_head "#include \"modules/${_mod}/observe.h\"\n")
    # keys.inc is optional: a module whose kinds plan against relations only,
    # and add no conflict edge, does not need one.
    if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/src/modules/${_mod}/keys.inc)
        string(APPEND _keys_inc "#include \"modules/${_mod}/keys.inc\"\n")
    endif()
    # project.inc is optional too: a kind whose effect no LATER step in the same
    # plan can observe does not need to project anything.
    #
    # PGLASWELL_MODULE_SLOT is defined HERE rather than by the module, so that
    # the extension slot a projection writes is not the module's to choose. The
    # macro in planner.h uses it to hand the function its own subtree and
    # nothing else; a module cannot name "core" or another module's slot,
    # because it never spells the name at all.
    if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/src/modules/${_mod}/project.inc)
        if(NOT EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/src/modules/${_mod}/project.h)
            message(FATAL_ERROR
                "src/modules/${_mod}/project.inc exists but project.h does not. "
                "The arms in project.inc name functions, and the functions live "
                "in project.h -- said here rather than as a missing-header error "
                "several hundred lines into planner.h.")
        endif()
        string(APPEND _proj_inc "#define PGLASWELL_MODULE_SLOT \"${_mod}\"\n")
        string(APPEND _proj_inc "#include \"modules/${_mod}/project.inc\"\n")
        string(APPEND _proj_inc "#undef PGLASWELL_MODULE_SLOT\n")
        string(APPEND _proj_head "#include \"modules/${_mod}/project.h\"\n")
    endif()
    if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/src/modules/${_mod}/tests.inc)
        string(APPEND _test_inc "#include \"modules/${_mod}/tests.inc\"\n")
        string(APPEND _body_inc "#include \"modules/${_mod}/tests.inc\"\n")
    endif()
    # observer.h is optional: a module whose vendor does not change where
    # contention is visible does not need one.
    # guard.h is optional: a module with no topology-wide precondition needs none.
    if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/src/modules/${_mod}/tests_planner.inc)
        string(APPEND _ptest_inc "#include \"modules/${_mod}/tests_planner.inc\"\n")
    endif()
    # confine.h is optional: a module whose vendor places no restriction on how
    # row-locking batches may be shaped needs none. It answers ONE question for
    # core -- must such batches on this table be confined to single values of a
    # column, and which? -- and core decides everything that follows from the
    # answer. A reading core interprets, never SQL core emits.
    if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/src/modules/${_mod}/confine.h)
        string(APPEND _confine_head "#include \"modules/${_mod}/confine.h\"\n")
        string(APPEND _confine_inc
            "PGLASWELL_CONFINEMENT(\"${_mod}\", ${_mod}_required_confinement)\n")
    endif()
    if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/src/modules/${_mod}/guard.h)
        string(APPEND _guard_head "#include \"modules/${_mod}/guard.h\"\n")
        string(APPEND _guard_inc "PGLASWELL_PLAN_GUARD(${_mod}_plan_refusals)\n")
    endif()
    if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/src/modules/${_mod}/observer.h)
        string(APPEND _topo_inc
            "PGLASWELL_TOPOLOGY(\"${_mod}\", k${_mod}ObserverAppliesSql, k${_mod}AnyWaitersSql, k${_mod}ObserverSql)\n")
    endif()
    list(APPEND _mod_names "${_mod}")
    message(STATUS "pg_laswell module: ${_mod}")
endforeach()
# Written through configure_file's copy_if_different semantics, so an unchanged
# module list does not force a rebuild of every translation unit.
file(WRITE ${_gen_dir}/enabled_planners.h.tmp "${_plan_inc}")
configure_file(${_gen_dir}/enabled_planners.h.tmp ${_gen_dir}/enabled_planners.h COPYONLY)
file(WRITE ${_gen_dir}/enabled_observer_headers.h.tmp "${_obs_head}")
configure_file(${_gen_dir}/enabled_observer_headers.h.tmp ${_gen_dir}/enabled_observer_headers.h COPYONLY)
file(WRITE ${_gen_dir}/enabled_observers.h.tmp "${_obs_inc}")
configure_file(${_gen_dir}/enabled_observers.h.tmp ${_gen_dir}/enabled_observers.h COPYONLY)
file(WRITE ${_gen_dir}/enabled_kind_bodies.h.tmp "${_body_inc}")
configure_file(${_gen_dir}/enabled_kind_bodies.h.tmp ${_gen_dir}/enabled_kind_bodies.h COPYONLY)
file(WRITE ${_gen_dir}/enabled_planner_tests.h.tmp "${_ptest_inc}")
configure_file(${_gen_dir}/enabled_planner_tests.h.tmp ${_gen_dir}/enabled_planner_tests.h COPYONLY)
file(WRITE ${_gen_dir}/enabled_confinement_headers.h.tmp "${_confine_head}")
configure_file(${_gen_dir}/enabled_confinement_headers.h.tmp ${_gen_dir}/enabled_confinement_headers.h COPYONLY)
file(WRITE ${_gen_dir}/enabled_confinements.h.tmp "${_confine_inc}")
configure_file(${_gen_dir}/enabled_confinements.h.tmp ${_gen_dir}/enabled_confinements.h COPYONLY)
file(WRITE ${_gen_dir}/enabled_guard_headers.h.tmp "${_guard_head}")
configure_file(${_gen_dir}/enabled_guard_headers.h.tmp ${_gen_dir}/enabled_guard_headers.h COPYONLY)
file(WRITE ${_gen_dir}/enabled_guards.h.tmp "${_guard_inc}")
configure_file(${_gen_dir}/enabled_guards.h.tmp ${_gen_dir}/enabled_guards.h COPYONLY)
file(WRITE ${_gen_dir}/enabled_topologies.h.tmp "${_topo_inc}")
configure_file(${_gen_dir}/enabled_topologies.h.tmp ${_gen_dir}/enabled_topologies.h COPYONLY)
file(WRITE ${_gen_dir}/enabled_tests.inc.tmp "${_test_inc}")
configure_file(${_gen_dir}/enabled_tests.inc.tmp ${_gen_dir}/enabled_tests.inc COPYONLY)
file(WRITE ${_gen_dir}/enabled_project.inc.tmp "${_proj_inc}")
configure_file(${_gen_dir}/enabled_project.inc.tmp ${_gen_dir}/enabled_project.inc COPYONLY)
file(WRITE ${_gen_dir}/enabled_project_headers.h.tmp "${_proj_head}")
configure_file(${_gen_dir}/enabled_project_headers.h.tmp ${_gen_dir}/enabled_project_headers.h COPYONLY)
file(WRITE ${_gen_dir}/enabled_keys.inc.tmp "${_keys_inc}")
configure_file(${_gen_dir}/enabled_keys.inc.tmp ${_gen_dir}/enabled_keys.inc COPYONLY)
file(WRITE ${_gen_dir}/enabled_kinds.inc.tmp "${_kinds_inc}")
file(WRITE ${_gen_dir}/enabled_modules.h.tmp "${_heads_inc}")
configure_file(${_gen_dir}/enabled_kinds.inc.tmp ${_gen_dir}/enabled_kinds.inc COPYONLY)
configure_file(${_gen_dir}/enabled_modules.h.tmp ${_gen_dir}/enabled_modules.h COPYONLY)

add_executable(pg_laswell src/deploy_main.cpp)
add_executable(pg_laswell_mcp src/main.cpp)
string(JOIN "," _mod_list ${_mod_names})
# The applying binary's module set, so a plan can record which capabilities
# produced it. "" means PostgreSQL-only.
add_compile_definitions(PGLASWELL_MODULE_SET="${_mod_list}")
target_compile_definitions(pg_laswell_mcp PRIVATE PGLASWELL_VERSION="${PROJECT_VERSION}")
target_compile_definitions(pg_laswell PRIVATE PGLASWELL_VERSION="${PROJECT_VERSION}")
target_link_libraries(pg_laswell_mcp PRIVATE ${PGLASWELL_LIBS})
target_link_libraries(pg_laswell PRIVATE ${PGLASWELL_LIBS})
# The generated module includes sit beside src/ on the include path, so core
# says #include "modules/enabled_kinds.inc" and never learns where it came from.
foreach(_t pg_laswell pg_laswell_mcp)
    target_include_directories(${_t} PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src ${CMAKE_CURRENT_BINARY_DIR}/generated)
endforeach()
pglaswell_harden(pg_laswell_mcp)
pglaswell_apply_sanitizers(pg_laswell_mcp)

install(TARGETS pg_laswell pg_laswell_mcp RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
# The man page is checked in rather than generated: the release containers carry
# no doc toolchain, and the release build runs `--target pg_laswell_mcp` rather
# than `all`, so a generator target would never run and CPack would then fail on
# a missing install file.
install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/man/pg_laswell.1
              ${CMAKE_CURRENT_SOURCE_DIR}/man/pg_laswell_mcp.1
        DESTINATION ${CMAKE_INSTALL_MANDIR}/man1)
# A module's page ships only with a binary that HAS the module, for the same
# reason --version names the set: a page documenting citus_distribute_table
# installed beside a binary that refuses it sends a reader to write a
# specification that cannot run. Section 7, because it describes a vocabulary
# rather than a command.
foreach(_mod IN LISTS PGLASWELL_MODULES)
    if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/man/pg_laswell_${_mod}.7)
        install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/man/pg_laswell_${_mod}.7
                DESTINATION ${CMAKE_INSTALL_MANDIR}/man7)
    else()
        message(FATAL_ERROR
            "module '${_mod}' is enabled but man/pg_laswell_${_mod}.7 does not "
            "exist. A module's kinds are absent from a stock build, so they "
            "cannot be documented in the core page -- they need a page of "
            "their own. tools/check-manual.py enforces the same rule.")
    endif()
endforeach()
# The bootstrap script ships alongside the binary because the binary must never
# run it: if pg_laswell created laswell.trusted_key its role would own it, and
# an owner can INSERT, so the migrating role could grant itself trust.
install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/../sql/bootstrap.sql
        DESTINATION ${CMAKE_INSTALL_DATADIR}/pg_laswell)

option(BUILD_TESTING "Build the test suite" ON)
if(BUILD_TESTING)
    enable_testing()
    find_package(GTest REQUIRED)
    add_executable(pg_laswell_mcp_test src/test_main.cpp src/planner_purity_check.cpp)
    target_include_directories(pg_laswell_mcp_test PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src ${CMAKE_CURRENT_BINARY_DIR}/generated)
    target_compile_definitions(pg_laswell_mcp_test PRIVATE
        PGLASWELL_VERSION="${PROJECT_VERSION}"
        # The shipped bootstrap script, so a test runs the file that actually
        # ships rather than a reimplementation of it that can drift.
        PGLASWELL_BOOTSTRAP_SQL="${CMAKE_CURRENT_SOURCE_DIR}/../sql/bootstrap.sql")
    target_link_libraries(pg_laswell_mcp_test PRIVATE
        GTest::gtest GTest::gtest_main ${PGLASWELL_LIBS})
    pglaswell_harden(pg_laswell_mcp_test)
    pglaswell_apply_sanitizers(pg_laswell_mcp_test)

    add_test(NAME mcp_test COMMAND pg_laswell_mcp_test)
    pglaswell_add_valgrind_test(pg_laswell_mcp_test)

    # Since the man page is hand-written, lint it here rather than trusting
    # review. -Wwarning (not -Wall) is deliberate: it fails on real WARNINGs but
    # ignores STYLE notes such as "referenced manual not found", which fire on
    # any machine without PostgreSQL's own man pages installed.
    find_program(MANDOC_EXECUTABLE mandoc)
    if(MANDOC_EXECUTABLE)
        set(_man_pages
            ${CMAKE_CURRENT_SOURCE_DIR}/man/pg_laswell.1
            ${CMAKE_CURRENT_SOURCE_DIR}/man/pg_laswell_mcp.1)
        # Every module page, enabled or not: the pages are checked in, so they
        # are lintable whatever this build carries.
        file(GLOB _mod_man ${CMAKE_CURRENT_SOURCE_DIR}/man/pg_laswell_*.7)
        list(APPEND _man_pages ${_mod_man})
        add_test(NAME manpage_lint
                 COMMAND ${MANDOC_EXECUTABLE} -Tlint -Wwarning ${_man_pages})
    else()
        message(STATUS "mandoc not found: skipping the manpage_lint test")
    endif()
    # The man pages are hand-written, so they can drift from the kinds the
    # source implements -- and had: the core list named 46 of 83, and no page
    # mentioned a module. Linting proves the mdoc parses, not that it is true.
    find_program(PYTHON3_EXECUTABLE NAMES python3 python)
    if(PYTHON3_EXECUTABLE)
        add_test(NAME manpage_kinds
                 COMMAND ${PYTHON3_EXECUTABLE}
                         ${CMAKE_CURRENT_SOURCE_DIR}/../tools/check-manual.py)
    else()
        message(STATUS "python3 not found: skipping the manpage_kinds test")
    endif()

    # What a fixed Observations literal cannot see: the Citus module's refusals
    # are unit-tested against hand-written readings, which proves the DECISION
    # and nothing about whether Citus can plan and run the statement the planner
    # emits. That gap cost a defect -- a correct decision whose statement the
    # dry run could not verify -- so the seam gets a test against a real
    # cluster. It SKIPs loudly without one rather than passing quietly.
    if(PGLASWELL_MODULES)
        add_test(NAME citus_tests
                 COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/test/citus-tests.sh
                         $<TARGET_FILE:pg_laswell> $<TARGET_FILE:pg_laswell_mcp>)
    endif()

    # The --call contract, at the process level. The exit code is the whole
    # point of that mode and gtest cannot see it: the suite observes return
    # values, not process status. Runs without a database too, covering the
    # flags that need none.
    # One bug shape has appeared five times in this project: iterating the
    # temporary that .value() returns. -Wdangling-reference has caught it every
    # time so far, which is luck rather than a guarantee -- it is a heuristic,
    # and it is not in every compiler. This makes it a build failure instead.
    add_test(NAME no_dangling_items
             COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/test/no-dangling-items.sh
                     ${CMAKE_CURRENT_SOURCE_DIR}/src)

    # The intent kinds that need a second cluster: subscriptions, foreign data,
    # and default privileges, which only affect objects created LATER. The
    # script builds two clusters with initdb under a temp directory and throws
    # them away, so it touches no existing server -- and SKIPs cleanly when
    # initdb is not installed, since that is a missing tool rather than a
    # failing behaviour.
    # Renders a plan from a spec and a fixed observation file. Links the pure
    # headers only -- no libpq, no database -- which is what makes the golden
    # output deterministic and is itself a check that planner.h stays pure.
    add_executable(render_plan ${CMAKE_CURRENT_SOURCE_DIR}/test/render_plan.cpp)
    target_include_directories(render_plan PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src ${CMAKE_CURRENT_BINARY_DIR}/generated)
    target_link_libraries(render_plan PRIVATE nlohmann_json::nlohmann_json
                                              OpenSSL::Crypto)
    target_compile_definitions(render_plan PRIVATE PGLASWELL_VERSION="${PROJECT_VERSION}")
    pglaswell_harden(render_plan)

    add_test(NAME plans
             COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/test/plan-tests.sh
                     $<TARGET_FILE:render_plan>
                     ${CMAKE_CURRENT_SOURCE_DIR}/test/plans)

    add_test(NAME replication
             COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/test/replication-tests.sh)

    # DATABASE_URL is read by the script at RUN time, not baked in here.
    # $ENV{} is evaluated during configure, so a conninfo captured then
    # outlives the database it names: the suite went on "passing" against a
    # dropped spike database because every check that expects a non-zero exit
    # is satisfied by a connection failure. ctest passes its own environment
    # through, so late binding costs nothing and the script tests what the
    # operator actually pointed it at.
    add_test(NAME call_mode
             COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/test/call-mode.sh
                     $<TARGET_FILE:pg_laswell_mcp>)
endif()

message(STATUS "Using compiler: ${CMAKE_CXX_COMPILER} (${CMAKE_CXX_COMPILER_ID})")
message(STATUS "Requested C++ standard: ${CMAKE_CXX_STANDARD}")
message(STATUS "Sanitizer: ${PGLASWELL_SANITIZER}")

set(CPACK_PACKAGE_NAME "pg-laswell")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Lock-aware PostgreSQL migration executor (MCP server)")
set(CPACK_PACKAGE_VENDOR "sqlambda")
# Mandatory for the DEB generator, which fails the whole package without it.
set(CPACK_PACKAGE_CONTACT "https://github.com/sqlambda/pg_laswell")
set(CPACK_PACKAGE_HOMEPAGE_URL "https://github.com/sqlambda/pg_laswell")

# Dependencies are DERIVED from the binary, never listed by hand.
#
# The hand-written list said "libpq5" and was wrong in both directions. It
# missed libpqxx-7.10, which the binary links directly -- measured in a clean
# debian:13-slim container, the package installed without complaint and the
# binary then died with "libpqxx-7.10.so: cannot open shared object file". It
# also missed libstdc++6 (>= 14), which C++23 needs and which no amount of
# reading the source would have suggested. And it named libpq5 redundantly:
# dpkg-shlibdeps drops it because libpqxx-7.10 already depends on it.
#
# A hardcoded list cannot be right for long -- it is a claim about the link
# line maintained in a different file from the link line. SHLIBDEPS runs
# dpkg-shlibdeps over what was actually built, so adding a library to
# target_link_libraries updates the package by itself.
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
set(CPACK_DEBIAN_PACKAGE_SECTION "database")
# No explicit Requires for RPM: rpmbuild generates them from the ELF sonames
# the same way, and the previous value was a DEBIAN package name ("libpq5",
# which is "libpq" on Fedora and RHEL) -- an explicit Requires naming a package
# that does not exist makes the RPM uninstallable rather than under-specified.
set(CPACK_RPM_PACKAGE_LICENSE "Apache-2.0")
set(CPACK_RPM_PACKAGE_GROUP "Applications/Databases")

include(CPack)
