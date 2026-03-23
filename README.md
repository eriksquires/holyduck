# HolyDuck — DuckDB Storage Engine for MariaDB

*Bringing divine analytical powers to Maria.*

**HolyDuck** (`ha_duckdb`) is a MariaDB storage engine plugin that embeds [DuckDB](https://duckdb.org/) as a first-class storage engine next to InnoDB. Create tables with `ENGINE=DUCKDB` and query them with standard SQL — DuckDB handles the heavy lifting.

## Why

MariaDB is beloved. DuckDB is a miracle of analytical performance. Maria deserved a blessing.

HolyDuck is that blessing — it embeds DuckDB directly inside MariaDB so you can:

- Run analytical queries (GROUP BY, aggregations, window functions) at DuckDB speed
- Mix DuckDB and InnoDB tables in the same query — DuckDB for analytics

HolyDuck is an extremely easy to install, easy to use OLAP engine that lives inside your existing MariaDB data infrastructure.  The speed of a parallel column store database with the convenience of MariaDB with incredibly simple installation. 

We are particularly proud of the performance of mixed-engine joins.  The pain point for any mixed-engine database is joins on tables that come from multiple engines/storage systems.  With a little care in your SQL query and the favor of HolyDuck you can avoid the penalty of bringing large datasets out to MariaDB for row by row joining.

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

EXPLAIN confirms the CTE runs entirely inside DuckDB (`PUSHED DERIVED`), returning a small result for MariaDB to join. See [WRITING_SQL.md](WRITING_SQL.md) for query optimization patterns and pushdown details. For architecture and internals see [INTERNALS.md](INTERNALS.md).

## HolyDuck vs ColumnStore

MariaDB's ColumnStore is the official MariaDB analytical storage engine in this space. Here's how our humble contribution compares:

|                           | HolyDuck                          | ColumnStore                          |
| ------------------------- | --------------------------------- | ------------------------------------ |
| Architecture              | In-process — runs inside mysqld   | Separate process / service           |
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

HolyDuck tables are native DuckDB tables that live and grow in a DuckDB database — no external connections or translations occur.

## HolyDuck vs. Remote Scanning

HolyDuck is the opposite of using Duck's remote scanning features.  While remote scanning works with any table on a remote server, HolyDuck only takes responsibility for tables in DuckDB. 

## DuckDB Limitations

HoldyDuck is much more team friendly in a sense than DuckDB alone but it's a gift, not magic. 

DuckDB does not handle more than one write connection at a time.  HolyDuck does, but the entire engine blocks per write operation.  That is, you and your colleagues can open and access the host MariaDB instance and query all the tables you normally would inside and outside of DuckDB but any DuckDB change operations will block all other DuckDB writers.   This naturally promotes a pattern of using DuckDB for scanning very big tables to create smaller InnoDB which need more frequent edits/refactoring.  

Another important limitation is that MariaDB enforces SQL correctness.  You can't run anything against DuckDB that MariaDB won't allow. 

## Installation

Pre-built binaries are available on the [Releases](https://github.com/eriksquires/HolyDuck/releases) page for Ubuntu 22.04, Oracle Linux 8, and Oracle Linux 9.

### 1. Find your plugin directory

```sql
SELECT @@plugin_dir;
```

### 2. Install libduckdb.so

HolyDuck v0.2.0 requires DuckDB v1.5.0. Download `libduckdb-linux-amd64.zip` from the
[DuckDB releases page](https://github.com/duckdb/duckdb/releases/tag/v1.5.0), then:

```bash
unzip libduckdb-linux-amd64.zip libduckdb.so
cp libduckdb.so /usr/lib/libduckdb.so
ldconfig
```

### 3. Copy the plugin files

Download `ha_duckdb-<distro>.so` and `holyduck_duckdb_extensions.sql` from the [HolyDuck releases page](https://github.com/eriksquires/HolyDuck/releases),
then copy both into your plugin directory:

```bash
cp ha_duckdb-ubuntu22.so /path/to/plugin_dir/ha_duckdb.so
cp holyduck_duckdb_extensions.sql /path/to/plugin_dir/holyduck_duckdb_extensions.sql
```

### 4. Install the plugin

```sql
INSTALL PLUGIN duckdb SONAME 'ha_duckdb-<distro>.so';
```

### 5. Verify

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

## Acknowledgements

Clearly this project being just a shim between two data stores could not exist without the hard work and excellent contributions of the [DuckDB](https://duckdb.org/) and [MariaDB Foundation](https://mariadb.org/) teams and all those who have contributed before.  
