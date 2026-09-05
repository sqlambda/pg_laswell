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
# sql/bootstrap.sql is installed from phase 3.4, when it exists. It is not
# stubbed here: a placeholder bootstrap script that looked runnable but created
# no trust table would be worse than an absent one, because the failure would
# surface as "the tool cannot verify anything" rather than "you have not
# bootstrapped yet".

option(BUILD_TESTING "Build the test suite" ON)
if(BUILD_TESTING)
    enable_testing()
    find_package(GTest REQUIRED)
    add_executable(pg_laswell_mcp_test src/test_main.cpp src/planner_purity_check.cpp)
    target_compile_definitions(pg_laswell_mcp_test PRIVATE
        PGLASWELL_VERSION="${PROJECT_VERSION}")
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
endif()

message(STATUS "Using compiler: ${CMAKE_CXX_COMPILER} (${CMAKE_CXX_COMPILER_ID})")
message(STATUS "Requested C++ standard: ${CMAKE_CXX_STANDARD}")
message(STATUS "Sanitizer: ${PGLASWELL_SANITIZER}")

set(CPACK_PACKAGE_NAME "pg-laswell")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Lock-aware PostgreSQL migration executor (MCP server)")
set(CPACK_PACKAGE_VENDOR "sqlambda")

# libpqxx is linked statically and baked into the binary for releases; libpq
# remains the only shared runtime dependency we declare. libcrypto is NOT listed
# separately because libpq5 already depends on it — adding it would be a second
# claim on the same package, differently named per distro.
set(CPACK_DEBIAN_PACKAGE_DEPENDS "libpq5")
set(CPACK_DEBIAN_PACKAGE_SECTION "database")
set(CPACK_RPM_PACKAGE_REQUIRES "libpq5")
set(CPACK_RPM_PACKAGE_LICENSE "Apache-2.0")
set(CPACK_RPM_PACKAGE_GROUP "Applications/Databases")

include(CPack)
