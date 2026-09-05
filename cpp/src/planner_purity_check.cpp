// Compile-time proof that planner.h is pure.
//
// planner.h is a function of (Spec, Observations, ExecutorConfig) and nothing
// else. That is what makes plan determinism testable against a fixed struct
// with no database in the loop -- and it is the sort of property that decays
// the first time someone adds a convenient query "just here". This translation
// unit includes planner.h and NOTHING else, so if pqxx ever arrives through
// that include graph, the build fails here with a message saying why.

#include "planner.h"

#if defined(PQXX_VERSION_MAJOR) || defined(LIBPQXX_H) || defined(LIBPQ_FE_H)
#  error "planner.h has acquired a dependency on libpqxx or libpq. The planner \
must stay a pure function of (Spec, Observations, ExecutorConfig): that is what \
lets plan determinism be tested against a fixed Observations literal with no \
server running. Move whatever needs a connection into catalog.h and pass the \
reading in through Observations."
#endif

namespace pglaswell {
// Referenced by the test suite purely to keep this object file linked in.
bool planner_is_pure() { return true; }
}  // namespace pglaswell
