\ir create_single_db.sql

\c single

CREATE TABLE PUBLIC."one_Partition" (
  "timeCustom" BIGINT NOT NULL,
  device_id TEXT NOT NULL,
  series_0 DOUBLE PRECISION NULL,
  series_1 DOUBLE PRECISION NULL,
  series_2 DOUBLE PRECISION NULL,
  series_bool BOOLEAN NULL
);
CREATE INDEX ON PUBLIC."one_Partition" (device_id, "timeCustom" DESC NULLS LAST) WHERE device_id IS NOT NULL;
CREATE INDEX ON PUBLIC."one_Partition" ("timeCustom" DESC NULLS LAST, series_0) WHERE series_0 IS NOT NULL;
CREATE INDEX ON PUBLIC."one_Partition" ("timeCustom" DESC NULLS LAST, series_1)  WHERE series_1 IS NOT NULL;
CREATE INDEX ON PUBLIC."one_Partition" ("timeCustom" DESC NULLS LAST, series_2) WHERE series_2 IS NOT NULL;
CREATE INDEX ON PUBLIC."one_Partition" ("timeCustom" DESC NULLS LAST, series_bool) WHERE series_bool IS NOT NULL;

SELECT * FROM create_hypertable('"public"."one_Partition"', 'timeCustom', associated_schema_name=>'one_Partition' );

BEGIN;
\COPY "one_Partition" FROM 'data/ds1_dev1_1.tsv' NULL AS '';
COMMIT;

SELECT _timescaledb_meta_api.close_chunk_end_immediate(c.id)
FROM get_open_partition_for_key((SELECT id FROM _timescaledb_catalog.hypertable WHERE table_name = 'one_Partition'), 'dev1') part
INNER JOIN _timescaledb_catalog.chunk c ON (c.partition_id = part.id);

INSERT INTO "one_Partition"("timeCustom", device_id, series_0, series_1) VALUES
(1257987600000000000, 'dev1', 1.5, 1),
(1257987600000000000, 'dev1', 1.5, 2),
(1257894000000000000, 'dev20', 1.5, 1),
(1257894002000000000, 'dev1', 2.5, 3);

INSERT INTO "one_Partition"("timeCustom", device_id, series_0, series_1) VALUES
(1257894000000000000, 'dev20', 1.5, 2);
