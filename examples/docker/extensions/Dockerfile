# PostgreSQL with the two extensions the pg_cron and pgvector modules read:
# packages from PGDG, which the official image already has configured. Pinned
# to the major the project targets; the extension versions are whatever PGDG
# ships for it, and the modules read them rather than assume them.
ARG PG_MAJOR=18
FROM postgres:${PG_MAJOR}
ARG PG_MAJOR
RUN apt-get update \
 && apt-get install -y --no-install-recommends \
      postgresql-${PG_MAJOR}-cron postgresql-${PG_MAJOR}-pgvector \
 && rm -rf /var/lib/apt/lists/*
