#pragma once

// What the planner measured, as plain data.
//
// This lives in its own header, apart from catalog.h which produces it,
// precisely so that planner.h can consume it WITHOUT pulling in pqxx. That
// separation is the purity invariant: plan_migration() must be testable
// against a fixed Observations literal with no server in the loop, and
// planner_purity_check.cpp asserts at compile time that pqxx never arrives
// through this path.

#include <string>

#include <nlohmann/json.hpp>

namespace pglaswell {

using json = nlohmann::json;

// What was measured, and how much to trust it. Carried alongside every number
// the planner branches on, so a decision can be re-read later and checked.
struct Observations {
  int server_version = 0;
  json tables = json::object();   // "schema.table" -> measurements
  // Everything that is not a relation: schemas, extensions, types, functions,
  // sequences, triggers. Keyed "kind:name" -- "type:public.mood",
  // "function:public.f(integer)", "extension:pg_trgm", "schema:reporting" --
  // because the namespaces do not overlap and a single map keeps the planner's
  // lookups uniform. Each entry carries at least `exists` and, for anything
  // droppable, `depended_on_by`.
  json objects = json::object();
  json server = json::object();   // max_connections, headroom, activity
  json gathered_at = json();

  const json& table(const std::string& qualified) const {
    static const json kEmpty = json::object();
    const auto it = tables.find(qualified);
    return it == tables.end() ? kEmpty : *it;
  }
  const json& object(const std::string& key) const {
    static const json kEmpty = json::object();
    const auto it = objects.find(key);
    return it == objects.end() ? kEmpty : *it;
  }
  bool has_table(const std::string& qualified) const {
    const auto& t = table(qualified);
    return t.value("exists", false);
  }
};

}  // namespace pglaswell
