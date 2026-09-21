#pragma once
// The Citus module's parse surface, included by spec.h.
//
// Split from plan.h deliberately: spec.h needs the parsers and must NOT pull in
// anything the planner uses, and planner.h needs the planners and nothing here.
#include "modules/citus/parse.h"
#include "modules/citus/observer.h"
