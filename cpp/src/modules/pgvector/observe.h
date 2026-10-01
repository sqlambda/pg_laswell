#pragma once
// pgvector readings. Gated on the extension being installed in THIS database:
// pgvector is per database, and its types and operator classes exist only
// where CREATE EXTENSION vector ran.
// Included inside namespace pglaswell by catalog.h.

inline const char* kpgvectorPresentSql =
    "SELECT EXISTS (SELECT 1 FROM pg_extension WHERE extname = 'vector')";

// The operator classes of its two index methods, read rather than listed:
// which type each serves and whether it is the default is the server's to say,
// and pgvector has added classes between releases (halfvec, sparsevec and bit
// arrived in 0.7.0). Measured on 0.8.6: 14 for hnsw, 7 for ivfflat, and the
// only default is ivfflat's vector_l2_ops.
//
// The methods' capabilities come from pg_indexam_has_property for the same
// reason. Measured: hnsw refuses unique ("access method \"hnsw\" does not
// support unique indexes") and multicolumn indexes, which is what can_unique
// and can_multi_col say.
inline const char* kpgvectorObservationSql = R"SQL(
SELECT JSONB_BUILD_OBJECT(
  'version', (SELECT extversion FROM pg_extension WHERE extname = 'vector'),
  'opclasses', COALESCE((
     SELECT JSONB_AGG(JSONB_BUILD_OBJECT(
              'method', a.amname, 'opclass', c.opcname,
              'type', t.typname, 'default', c.opcdefault)
            ORDER BY a.amname, c.opcname)
       FROM pg_opclass c
       JOIN pg_am a ON a.oid = c.opcmethod
       JOIN pg_type t ON t.oid = c.opcintype
      WHERE a.amname IN ('hnsw', 'ivfflat')), '[]'::jsonb),
  'methods', COALESCE((
     SELECT JSONB_OBJECT_AGG(a.amname, JSONB_BUILD_OBJECT(
              'can_unique', pg_indexam_has_property(a.oid, 'can_unique'),
              'can_multi_col', pg_indexam_has_property(a.oid, 'can_multi_col')))
       FROM pg_am a WHERE a.amname IN ('hnsw', 'ivfflat')), '{}'::jsonb))
)SQL";

// Nothing is read where pgvector is not installed in this database.
inline const char* kpgvectorAbsentSql = nullptr;
