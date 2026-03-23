# HolyDuck — DuckDB Storage Engine for MariaDB

*Bringing divine analytical powers to Maria.*

**HolyDuck** (`ha_duckdb`) is a MariaDB storage engine plugin that embeds [DuckDB](https://duckdb.org/) as a first-class storage engine. Create tables with `ENGINE=DUCKDB` and query them with standard SQL — DuckDB handles the heavy lifting.

## Why

MariaDB is beloved. DuckDB is a miracle of analytical performance. Maria deserved a blessing.

HolyDuck is that blessing — it embeds DuckDB directly inside MariaDB so you can:

- Run analytical queries (GROUP BY, aggregations, window functions) at DuckDB speed
- Mix DuckDB and InnoDB tables in the same query — DuckDB for analytics, InnoDB for OLTP
- Use CTEs to pre-aggregate DuckDB data, then join the small result against InnoDB tables

## Quick Example

```sql
-- Analytical table in DuckDB
CREATE TABLE metrics (ts DATETIME, sensor_id INT, val DOUBLE) ENGINE=DUCKDB;

-- Lookup table in InnoDB
CREATE TABLE sensors (id INT PRIMARY KEY, name VARCHAR(64)) ENGINE=InnoDB;

-- Mixed-engine query — DuckDB aggregation pushed down, joined against InnoDB
WITH agg AS (
    SELECT sensor_id, AVG(val) AS avg_val
    FROM metrics
    GROUP BY sensor_id
)
SELECT s.name, a.avg_val
FROM sensors s
JOIN agg a ON s.id = a.sensor_id;
```

EXPLAIN confirms the CTE runs entirely inside DuckDB (`PUSHED DERIVED`), returning a small result for MariaDB to join. See [TECHNICAL.md](TECHNICAL.md) for query optimization patterns and pushdown details.

## HolyDuck vs ColumnStore

MariaDB's ColumnStore is the official MariaDB analytical storage engine in this space. Here's how our humble contribution compares:

|                           | HolyDuck                          | ColumnStore                          |
| ------------------------- | --------------------------------- | ------------------------------------ |
| Concurrent writers        | Single writer (DuckDB limitation) | Parallel ingestion                   |
| High availability         | None — local file only            | MariaDB replication + clustering     |
| Multi-server              | Single node                       | Distributed across nodes             |
| Deployment                | Drop in a `.so` file              | Full cluster infrastructure          |
| Query speed (single node) | DuckDB — extremely fast           | Fast, but more overhead              |
| Setup complexity          | Minutes                           | Hours to days                        |
| ETL tooling               | Standard SQL                      | Bulk loaders, S3, enterprise tooling |

**HolyDuck's sweet spot:** single-node analytics alongside InnoDB. Big overnight ETL jobs, exploratory data analysis during the day, million-to-billion row scans that return small aggregated results — all without standing up any infrastructure.

One big benefit, if you can live with single writers, is you now have a sharable database with a single source of truth. Also, if you are like me and leave db connections stranded in various places while doing R/Quarto development you'll find yourself spending much less time managing connections in your code.

**ColumnStore's sweet spot:** when you've outgrown a single node and need HA, replication, and multi-server distribution.

If your workload fits on one machine, HolyDuck will likely be faster and infinitely simpler to run.

## DuckDB Limitations

DuckDB does not handle more than one write connection at a time.  HolyDuck does, but the entire engine blocks per write operation.  That is, you and your colleagues can open and access DuckDB tables and queries against them, if anyone start any change operation you will block all other writers.  

HoldyDuck is much more team friendly in a sense than raw DuckDB, but it's a gift, not magic.  

## Installation

Pre-built binaries are available on the [Releases](https://github.com/eriksquires/HolyDuck/releases) page for Ubuntu 22.04, Oracle Linux 8, and Oracle Linux 9.

### 1. Find your plugin directory

```sql
SELECT @@plugin_dir;
```

### 2. Copy the files

Download `ha_duckdb-<distro>.so` and `duckdb_mariadb_compat.sql` from the release page,
then copy both into your plugin directory:

```bash
cp ha_duckdb-ubuntu22.so /path/to/plugin_dir/ha_duckdb.so
cp duckdb_mariadb_compat.sql /path/to/plugin_dir/duckdb_mariadb_compat.sql
```

### 3. Install the plugin

```sql
INSTALL PLUGIN duckdb SONAME 'ha_duckdb.so';
```

### 4. Verify

```sql
SELECT ENGINE, SUPPORT, COMMENT FROM information_schema.ENGINES WHERE ENGINE='DUCKDB';
```

You should see `SUPPORT: YES`. You're ready to create DuckDB tables.

## Building from Source

See [BUILDING.md](BUILDING.md) for the full build workflow — Docker base images, MariaDB source setup, cmake configuration, and iterative development cycle.

## License

This project is dual-licensed:

- **[GPL v2](LICENSE)** — free to use under the terms of the GNU General Public License v2.
  Any derivative works must also be GPL v2.
- **[Commercial License](LICENSE-COMMERCIAL)** — for use in proprietary or closed-source
  products without GPL obligations. Contact via [GitHub](https://github.com/eriksquires).
