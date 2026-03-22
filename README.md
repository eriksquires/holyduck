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

## Build Requirements

- Linux x86-64
- Docker
- MariaDB 11.8.3 source tree (cloned by `fetch-deps.sh`)
- DuckDB v1.5.0 (downloaded by `fetch-deps.sh`)

## Getting Started

### 1. Fetch dependencies

```bash
./scripts/fetch-deps.sh
export MARIADB_SRC_DIR=<path printed by fetch-deps.sh>
```

This clones the MariaDB 11.8.3 source tree and downloads `libduckdb.so`. Both versions are configurable:

```bash
MARIADB_VERSION=11.8.3 DUCKDB_VERSION=v1.5.0 ./scripts/fetch-deps.sh
```

### 2. Build the Docker base image

```bash
./scripts/build-base.sh ubuntu     # Ubuntu 22.04
./scripts/build-base.sh oracle8    # Oracle Linux 8
./scripts/build-base.sh oracle9    # Oracle Linux 9
```

### 3. Start a container

```bash
./scripts/docker-run.sh ubuntu
```

### 4. Build MariaDB and configure the plugin (first time only, ~20-30 min)

Note that this is not optional. Required header files are built by this step, so even if you don't really want to build the whole DB you have to, but only once per target OS/DB version.

```bash
./scripts/cmake-setup.sh duckdb-plugin-dev-ubuntu
```

This builds MariaDB from source inside the container and configures the plugin's cmake. Build artifacts persist via the bind-mounted source directory — subsequent runs skip the MariaDB build.

### 5. Build and deploy the plugin

```bash
./scripts/deploy.sh duckdb-plugin-dev-ubuntu
```

Builds `ha_duckdb.so`, copies it into the container, restarts MariaDB, installs the plugin, and verifies it loaded. Run this after every code change.

### 6. Connect and test

```bash
docker exec -it duckdb-plugin-dev-ubuntu mariadb -uroot -ptestpass
```

```sql
CREATE TABLE t (id INT, val DOUBLE) ENGINE=DUCKDB;
INSERT INTO t VALUES (1, 10.5), (2, 20.0), (3, 15.5);
SELECT AVG(val) FROM t;
```

## Pre-built Binaries

Pre-built `.so` files for each distro are available on the [Releases](https://github.com/eriksquires/HolyDuck/releases) page. Download the appropriate file, place it in your MariaDB plugin directory, and:

```sql
INSTALL PLUGIN duckdb SONAME 'ha_duckdb.so';
```

## Development Workflow

```bash
# Edit source
vim src/ha_duckdb.cc

# Build, deploy, restart MariaDB, verify — one command
./scripts/deploy.sh duckdb-plugin-dev-ubuntu

# Connect
docker exec -it duckdb-plugin-dev-ubuntu mariadb -uroot -ptestpass
```

## License

This project is dual-licensed:

- **[GPL v2](LICENSE)** — free to use under the terms of the GNU General Public License v2.
  Any derivative works must also be GPL v2.
- **[Commercial License](LICENSE-COMMERCIAL)** — for use in proprietary or closed-source
  products without GPL obligations. Contact via [GitHub](https://github.com/eriksquires).
