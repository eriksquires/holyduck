# Task: Normalize build/version info to match IntensityDB

## Goal

Both IntensityDB and HolyDuck should report build info the same way so
automation scripts can validate both engines with identical logic.

## Current state

### IntensityDB (sysvars)
```sql
SELECT @@intensitydb_build_timestamp;   -- "Apr  4 2026 13:35:34"
SELECT @@intensitydb_build_flags;       -- "STUB_IO" or ""
```

### HolyDuck (SHOW ENGINE STATUS)
```sql
SHOW ENGINE DUCKDB STATUS;
-- Returns rows: HolyDuck version, HolyDuck built, DuckDB version, etc.
```

## Requested changes for HolyDuck

### 1. Add sysvars matching IntensityDB pattern

```cpp
static char *duckdb_build_timestamp = (char *)__DATE__ " " __TIME__;
static char *duckdb_build_flags = (char *)"";  // or compile flags
static char *duckdb_holyduck_version = (char *)HOLYDUCK_VERSION;

static MYSQL_SYSVAR_STR(build_timestamp, duckdb_build_timestamp,
    PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
    "Compile timestamp of the HolyDuck plugin",
    NULL, NULL, __DATE__ " " __TIME__);

static MYSQL_SYSVAR_STR(build_flags, duckdb_build_flags,
    PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
    "Compile-time feature flags",
    NULL, NULL, "");

static MYSQL_SYSVAR_STR(holyduck_version, duckdb_holyduck_version,
    PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
    "HolyDuck version string",
    NULL, NULL, HOLYDUCK_VERSION);
```

Note: MYSQL_SYSVAR_STR default value overrides the variable at init.
Use a macro for the default (like IntensityDB does with IDB_BUILD_FLAGS)
to ensure the compiled-in value is the one reported.

### 2. Keep SHOW ENGINE STATUS as-is

The SHOW ENGINE STATUS output is useful for detailed diagnostics.
The sysvars are for quick automated checks.

### 3. Timezone

`__DATE__` and `__TIME__` use the build machine's local time.
Both plugins build on the same machine, so they should agree.
The 4-hour discrepancy seen (13:14 file mtime vs 17:14 reported) needs
investigation — could be the Docker container running in UTC while the
host is in a different timezone.

**Fix:** If the container's timezone differs from the host, set TZ in
the build environment. Or use epoch seconds instead of formatted time:
```cpp
// At compile time, store epoch
static const time_t build_epoch = __TIME_EPOCH__;  // not standard
```

Actually simplest: just verify the `__DATE__ __TIME__` string matches
between what the sysvar reports and what the build script recorded
at compile time. If they match, the binary is correct.

## Validation logic (for build_and_test.pl)

After deploy and bounce:
```perl
# IntensityDB
my ($idb_ts) = $dbh->selectrow_array("SELECT \@\@intensitydb_build_timestamp");

# HolyDuck (after this change)
my ($hd_ts)  = $dbh->selectrow_array("SELECT \@\@duckdb_build_timestamp");

# Both should be recent (within 5 minutes of now)
# Parse "Mon DD YYYY HH:MM:SS" and compare to time()
```

## Testing

```sql
-- After implementing, both should work:
SELECT @@intensitydb_build_timestamp;
SELECT @@duckdb_build_timestamp;
SELECT @@duckdb_holyduck_version;
```
