#pragma once
// Citus's answer to the one question core asks about row locking:
//
//   must row-locking batches on this table be confined to single values of a
//   column, and which?
//
// NO namespace of its own: included INSIDE namespace pglaswell.
//
// This is the whole of what the module contributes to how a backfill walks a
// distributed table, and it is a READING, not SQL. Core reads the answer and
// emits its own statements -- which is the shape the module contract permits,
// and the reason the same specification plans byte-identically on every build
// for a table this function says nothing about.
//
// The answer, measured on Citus 13:
//
//   - a DISTRIBUTED table (partmethod 'h' and the legacy range/append methods):
//     yes, its distribution column. FOR UPDATE is refused on a multi-shard
//     query, and the only confinement Citus pushes down is an equality on that
//     column -- a hash-range predicate is not pushed down, reads every shard,
//     and is refused just the same.
//   - a REFERENCE table (partmethod 'n'): no. One placement per node, and a walk
//     over one runs as it does on plain PostgreSQL -- measured, 500 rows, none
//     left.
//   - a LOCAL table in a Citus database, or no Citus at all: no.
//
// Takes the observations and the table, returns a column name or "". It cannot
// reach the plan, and cannot say anything about any other table.
inline std::string citus_required_confinement(const Observations& obs,
                                              const std::string& qualified) {
  const auto& citus = obs.extension("citus");
  if (citus.empty()) return {};
  const auto tables = citus.value("tables", json::object());
  const auto it = tables.find(qualified);
  if (it == tables.end()) return {};
  if (it->value("partmethod", "") == "n") return {};
  return it->value("distribution_column", "");
}
