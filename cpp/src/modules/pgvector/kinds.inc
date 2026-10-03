// pgvector intent kinds: none.
//
// pgvector adds types and index methods, and every one of them is reached
// through a CORE kind: a vector(n) column is a create_table or add_column type,
// and an HNSW or IVFFlat index is a create_index with a method, an operator
// class and storage parameters (`with`). A kind of its own would be a second
// way to create an index, which is the cost this project refuses to pay twice.
//
// What the module adds is what core cannot know: which of those indexes
// pgvector will refuse, said before a concurrent build of a large table starts
// rather than after. See guard.h.
