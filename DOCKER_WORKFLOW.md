# Docker Development Workflow

## Code Development Process

### ✅ Code Changes (Local)
- **Edit code locally** on host machine in `~/shared/mariadb/mariadb-duckdb-plugin/src/`
- Plugin source files: `ha_duckdb.cc`, `ha_duckdb.h`, `CMakeLists.txt`
- Changes are immediately visible inside Docker container via volume mount

### ✅ Build Environment (Docker)
- **Start container**: `./scripts/docker-run.sh ubuntu`
- **Plugin source mounted**: `/plugin-src/` inside container
- **MariaDB source available**: `/mariadb-src/` (read-only)
- **Build happens inside container** with proper MariaDB headers and environment

## Development Cycle

### 1. Make Code Changes
```bash
# Edit files locally with any editor
vim ~/shared/mariadb/mariadb-duckdb-plugin/src/ha_duckdb.cc
```

### 2. Build Plugin
```bash
# Inside Docker container
cd /plugin-src/src/
# Build commands needed here - not yet documented
```

### 3. Install Plugin
```bash
# Inside Docker container
# Copy plugin to MariaDB plugin directory
# Restart MariaDB or reload plugin
# Install commands needed here - not yet documented
```

### 4. Test Plugin
```bash
# Inside Docker container
mariadb -u root -p
> SHOW ENGINES;  # Should show DUCKDB
> CREATE TABLE test (...) ENGINE=DUCKDB;  # Currently fails
```

## Current Gaps in Workflow

### ❌ Build Process Not Documented
- Exact cmake/make commands to build plugin unknown
- Integration with MariaDB build system unclear
- Plugin compilation steps need to be figured out

### ❌ Installation Process Not Documented  
- How to install compiled plugin in MariaDB unknown
- Plugin reload/restart process unclear
- Installation automation needed

### ❌ Testing Process Incomplete
- Manual testing only
- No automated test scripts
- Error debugging workflow unclear

## Next Priority: Complete the Workflow

Focus on Ubuntu only. Figure out:
1. **Build commands**: How to compile `ha_duckdb.so` inside container
2. **Install commands**: How to get MariaDB to load the updated plugin
3. **Test cycle**: Quick way to test changes
4. **Script it**: Automate build/install/test cycle

Goal: Fast iteration cycle for implementing storage engine methods.