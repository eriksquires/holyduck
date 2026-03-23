# Building HolyDuck from Source

## Requirements as Tested

- Linux x86-64
- Docker
- MariaDB 11.8.3 source tree (cloned by `fetch-deps.sh`)
- DuckDB v1.5.0 (downloaded by `fetch-deps.sh`)

## Steps

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

Note that this is not optional. Required header files are built by this step, so even if you
don't really want to build the whole DB you have to — but only once per target OS/DB version.

```bash
./scripts/cmake-setup.sh duckdb-plugin-dev-ubuntu
```

Build artifacts persist via the bind-mounted source directory, so subsequent runs skip the
MariaDB build entirely.

### 5. Build and deploy the plugin

```bash
./scripts/deploy.sh duckdb-plugin-dev-ubuntu
```

Builds `ha_duckdb.so`, copies it into the container, restarts MariaDB, installs the plugin,
and verifies it loaded. Run this after every code change.

### 6. Connect and test

```bash
docker exec -it duckdb-plugin-dev-ubuntu mariadb -uroot -ptestpass
```

```sql
CREATE TABLE t (id INT, val DOUBLE) ENGINE=DUCKDB;
INSERT INTO t VALUES (1, 10.5), (2, 20.0), (3, 15.5);
SELECT AVG(val) FROM t;
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
