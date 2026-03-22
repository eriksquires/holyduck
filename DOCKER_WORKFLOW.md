# Docker Development Workflow

## The One Command You Need

```bash
./scripts/deploy.sh
```

This script handles the full lifecycle: build on host → copy into container →
restart MariaDB → wait for ready → verify plugin loaded.  Run it after every
code change.

## Setup (first time only)

### 1. Start the test container

```bash
./scripts/docker-run.sh ubuntu
```

The container is named `duckdb-plugin-test`.  It bind-mounts:
- `./data/` → `/var/lib/mysql` (MariaDB data, persists across restarts)
- `./lib/`  → `/plugin-lib`   (libduckdb.so, read-only)
- `./build/` → `/plugin-build` (built .so, read-only — for reference)

### 2. Install the plugin (first deploy only)

```bash
./scripts/deploy.sh
```

On first run this builds the plugin, copies it to `/usr/lib/mysql/plugin/` inside
the container, restarts MariaDB, and installs the plugin via `INSTALL PLUGIN`.
Subsequent runs skip the install step if the plugin is already registered.

## Day-to-Day Cycle

```
1. Edit  →  src/ha_duckdb.cc  (or ha_duckdb.h, duckdb_mariadb_compat.sql)
2. Run   →  ./scripts/deploy.sh
3. Test  →  docker exec -it duckdb-plugin-test mariadb -uroot -ptestpass
```

## Connecting

```bash
# Interactive shell
docker exec -it duckdb-plugin-test mariadb -uroot -ptestpass

# One-off query
docker exec duckdb-plugin-test mariadb -uroot -ptestpass --ssl=0 -e "SHOW ENGINES;"

# DuckDB CLI (inspect .duckdb files directly)
docker exec duckdb-plugin-test duckdb /var/lib/mysql/\#duckdb/global.duckdb
```

## Inspecting DuckDB State

```bash
# List all tables
docker exec duckdb-plugin-test duckdb /var/lib/mysql/\#duckdb/global.duckdb \
    "SHOW TABLES;"

# List indexes
docker exec duckdb-plugin-test duckdb /var/lib/mysql/\#duckdb/global.duckdb \
    "SELECT index_name, table_name, sql FROM duckdb_indexes();"
```

## Checking Errors

```bash
# MariaDB error log (container hostname changes on recreate — use wildcard)
docker exec duckdb-plugin-test bash -c "tail -30 /var/lib/mysql/*.err"
```

## Updating Compatibility Macros Only

`sql/duckdb_mariadb_compat.sql` is a deployment artifact — it is copied to the
plugin directory at build time and loaded by the plugin at startup.  To update
macros without recompiling:

```bash
./scripts/deploy.sh   # redeploys the .sql file and restarts MariaDB
```

## Build Configuration

The host-side build is a standalone CMake project under `src/`, pre-configured
in `build/`.  Key paths (set in `build/CMakeCache.txt`):

| Variable | Value |
|---|---|
| `MARIADB_SOURCE_DIR` | `/home/erik/shared/mariadb/mariadb-11.8.3-git` |
| `MARIADB_BUILD_DIR` | `/home/erik/shared/mariadb/mariadb-11.8.3-git/build` |
| `DUCKDB_DIR` | `<plugin-root>/lib` |

To reconfigure (e.g. for a different MariaDB version):
```bash
cd build
cmake ../src -DMARIADB_SOURCE_DIR=... -DMARIADB_BUILD_DIR=...
```
