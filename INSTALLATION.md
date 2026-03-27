## Installation

Pre-built binaries are available on the [Releases](https://github.com/eriksquires/HolyDuck/releases) page for Ubuntu 22.04 (and Debian 12), Oracle Linux 8 and 9.

### 1. Find your plugin directory

```sql
SELECT @@plugin_dir;
```

### 2. Install libduckdb.so

HolyDuck v0.4.0 requires DuckDB v1.5.0. Download `libduckdb-linux-amd64.zip` from the
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
