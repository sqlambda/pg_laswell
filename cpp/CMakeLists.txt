cmake_minimum_required(VERSION 3.20)
project(pg_laswell_mcp VERSION 0.1.0 LANGUAGES CXX)

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
pkg_check_modules(PQXX REQUIRED IMPORTED_TARGET libpqxx)
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

add_executable(pg_laswell_mcp src/main.cpp)
target_compile_definitions(pg_laswell_mcp PRIVATE PGLASWELL_VERSION="${PROJECT_VERSION}")
target_link_libraries(pg_laswell_mcp PRIVATE ${PGLASWELL_LIBS})
pglaswell_harden(pg_laswell_mcp)
pglaswell_apply_sanitizers(pg_laswell_mcp)

install(TARGETS pg_laswell_mcp RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
# The man page is checked in rather than generated: the release containers carry
# no doc toolchain, and the release build runs `--target pg_laswell_mcp` rather
# than `all`, so a generator target would never run and CPack would then fail on
# a missing install file.
install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/man/pg_laswell_mcp.1
        DESTINATION ${CMAKE_INSTALL_MANDIR}/man1)
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
        add_test(NAME manpage_lint
                 COMMAND ${MANDOC_EXECUTABLE} -Tlint -Wwarning
                         ${CMAKE_CURRENT_SOURCE_DIR}/man/pg_laswell_mcp.1)
    else()
        message(STATUS "mandoc not found: skipping the manpage_lint test")
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
