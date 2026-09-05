# Changes

## 0.1.0 (unreleased)

First skeleton. Nothing is applied to a database yet.

- MCP server over stdio, JSON-RPC 2.0, protocol revisions 2024-11-05 through
  2025-11-25. `initialize`, `ping`, `tools/list` and `tools/call` answer;
  notifications correctly produce no response.
- Tools are declared in **one** `ToolDef` vector, from which `tools/list` and
  dispatch are both derived. pg_licht keeps four parallel structures per tool
  synchronised only by tests; that cost is deliberately not inherited.
- `handle_request()` returns its response rather than writing it, so the
  transport is testable without redirecting `std::cout`, and so exactly one
  function may write to stdout while worker threads are running.
- Build: C++23, warnings-as-errors, hardened standard library, one
  `PGLASWELL_SANITIZER` cache variable, auto-registered Valgrind ctest entry,
  `mandoc -Tlint` of the man page as a ctest entry. Verified on GCC 14.2 and
  Clang 22.
- Database tests skip without `DATABASE_URL`; `PGLASWELL_REQUIRE_DATABASE=1`
  turns the skip into a failure. This differs from pg_licht, whose suite refuses
  to run at all without one, because pg_laswell's pure layers are the ones most
  worth running on a machine with no PostgreSQL.
- `cpp/test/spikes/` — the Phase 0 experiments that settled the design against
  PostgreSQL 18.6. Four of them changed it.
