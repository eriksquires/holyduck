/*
   Copyright (c) 2026, Erik Squires

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   HolyDuck embeds DuckDB (MIT License, copyright Stichting DuckDB Foundation)
   and uses the MariaDB Server plugin API (GPL v2, copyright MariaDB Foundation).
   See THIRD_PARTY_NOTICES.md for full third-party license text.
*/

#define MYSQL_SERVER 1
#include "ha_duckdb.h"
#include "probes_mysql.h"
#include "sql_class.h"
#include "sql_lex.h"
#include "sql_cte.h"
#include "key.h"

// DuckDB includes — undef UNKNOWN macro from MariaDB's item_cmpfunc.h
// to avoid collision with DuckDB's enum values
#ifdef UNKNOWN
#undef UNKNOWN
#endif
#include "duckdb.hpp"

#include <string>
#include <sstream>
#include <map>
#include <fstream>
#include <vector>
#include <regex>

// MariaDB globals
extern char mysql_real_data_home[];  // absolute path to data directory
extern char opt_plugin_dir[];        // absolute path to plugin directory

// Forward declare sysvar so registry_get() can reference it
static ulong duckdb_max_threads;

// Forward declare rewrite pass so cond_push() can use it
static std::string rewrite_mariadb_sql(const std::string &sql);

// ---------------------------------------------------------------------------
// Global DuckDB instance registry
//
// DuckDB acquires an exclusive file lock on open, so only one duckdb::DuckDB
// instance per file path is allowed per process.  All tables in the same
// MariaDB database share a single DuckDB instance via this registry.
// ---------------------------------------------------------------------------

struct DuckDBEntry {
  duckdb::DuckDB *db;
  int refcount;
};

static std::map<std::string, DuckDBEntry> g_duckdb_registry;
static mysql_mutex_t g_duckdb_mutex;

// ---------------------------------------------------------------------------
// Active query registry — maps THD* to the DuckDB connection currently
// executing a query for that thread. Used by kill_query to interrupt it.
// Protected by g_duckdb_mutex.
// ---------------------------------------------------------------------------
static std::map<THD*, duckdb::Connection*> g_active_connections;

// ---------------------------------------------------------------------------
// Per-session DuckDB connection and TEMP TABLE injection cache
//
// Each MariaDB session (THD) gets one persistent DuckDB connection that is
// reused across all queries in the session.  TEMP TABLEs injected from InnoDB
// persist in the connection, so subsequent queries referencing the same
// dimension table skip the ha_rnd_next + Appender loop entirely.
//
// Cache correctness rules:
//  - Only FULL (unfiltered) injections are cached; filtered injections are
//    not cached because they may contain only a subset of rows.
//  - Every injection (cached or not) first issues DROP TABLE IF EXISTS to
//    ensure a clean slate, then CREATE TEMP TABLE — except cache hits which
//    skip both steps and reuse the existing TEMP TABLE.
//  - Cache stores the InnoDB table's stats.records at injection time.  On
//    every cache lookup, info(HA_STATUS_VARIABLE) is called to refresh the
//    current row count; if it differs from the stored value the entry is
//    invalidated and the table is re-injected.  This catches INSERTs and
//    DELETEs (including TRUNCATE and bulk ETL loads) reliably.  It does NOT
//    catch count-preserving UPDATEs (e.g. changing a phone number without
//    adding/removing rows), which is an acceptable trade-off for OLAP
//    dimension tables that are updated infrequently.
//  - Cache is also invalidated when the session ends (close_connection hook).
//
// Protected by g_duckdb_mutex.
// ---------------------------------------------------------------------------
static void registry_release(const std::string &path);  // defined below

struct THDDuckDBConn {
  duckdb::Connection *conn;
  std::string db_file_path;
};
static std::map<THD*, THDDuckDBConn>                                    g_thd_conns;
// Maps connection → (table_name → stats.records at injection time).
// A changed row count means the InnoDB table was modified; invalidate + re-inject.
static std::map<duckdb::Connection*, std::map<std::string, ha_rows>> g_injected_cache;

static int duckdb_close_connection(THD *thd)
{
  mysql_mutex_lock(&g_duckdb_mutex);
  auto it= g_thd_conns.find(thd);
  if (it != g_thd_conns.end())
  {
    duckdb::Connection *conn= it->second.conn;
    std::string path= it->second.db_file_path;
    g_injected_cache.erase(conn);
    g_thd_conns.erase(it);
    mysql_mutex_unlock(&g_duckdb_mutex);
    delete conn;
    registry_release(path);
  }
  else
    mysql_mutex_unlock(&g_duckdb_mutex);
  return 0;
}

static void register_active_connection(THD *thd, duckdb::Connection *conn)
{
  mysql_mutex_lock(&g_duckdb_mutex);
  g_active_connections[thd]= conn;
  mysql_mutex_unlock(&g_duckdb_mutex);
}

static void unregister_active_connection(THD *thd)
{
  mysql_mutex_lock(&g_duckdb_mutex);
  g_active_connections.erase(thd);
  mysql_mutex_unlock(&g_duckdb_mutex);
}

// ---------------------------------------------------------------------------
// MariaDB-compatibility SQL macros
// Installed once per DuckDB file so that pushed-down queries using
// MariaDB datetime/string function names work transparently in DuckDB.
// Uses CREATE OR REPLACE MACRO so re-opening the file after an upgrade
// always refreshes the definitions.
//
// Primary source: <plugin_dir>/holyduck_duckdb_extensions.sql  (editable without
//                 recompiling — add or fix macros, restart MariaDB)
// Fallback:       the built-in list below, used when the file is absent.
// ---------------------------------------------------------------------------

// Split a SQL file on ';', stripping -- line comments and blank lines.
static std::vector<std::string> parse_sql_statements(const std::string &text)
{
  std::vector<std::string> stmts;
  std::string current;
  std::istringstream stream(text);
  std::string line;

  while (std::getline(stream, line))
  {
    // Strip inline -- comments
    size_t comment= line.find("--");
    if (comment != std::string::npos)
      line= line.substr(0, comment);

    current += line + ' ';

    // Each ';' ends a statement
    size_t pos;
    while ((pos= current.find(';')) != std::string::npos)
    {
      std::string stmt= current.substr(0, pos);
      current= current.substr(pos + 1);

      // Trim whitespace
      size_t s= stmt.find_first_not_of(" \t\r\n");
      if (s != std::string::npos)
        stmts.push_back(stmt.substr(s));
    }
  }
  return stmts;
}

static void run_macro_statements(duckdb::Connection &conn,
                                 const std::vector<std::string> &stmts,
                                 const char *source)
{
  for (const auto &sql : stmts)
  {
    auto r= conn.Query(sql);
    if (r->HasError())
      sql_print_warning("DuckDB: compat macro failed (%s): %s — %s",
                        source, sql.c_str(), r->GetErrorObject().Message().c_str());
  }
}

static void install_mariadb_compat_macros(duckdb::DuckDB *db)
{
  duckdb::Connection conn(*db);

  // Try the external SQL file first.
  std::string sql_file= std::string(opt_plugin_dir) + "/holyduck_duckdb_extensions.sql";
  std::ifstream f(sql_file);
  if (f.good())
  {
    std::string text((std::istreambuf_iterator<char>(f)),
                      std::istreambuf_iterator<char>());
    auto stmts= parse_sql_statements(text);
    run_macro_statements(conn, stmts, sql_file.c_str());
    sql_print_information("DuckDB: loaded compat macros from %s (%zu statements)",
                          sql_file.c_str(), stmts.size());
    return;
  }

  // File not found — fall back to built-in definitions.
  sql_print_information("DuckDB: %s not found, using built-in compat macros",
                        sql_file.c_str());

  static const char *macros[]= {
    "CREATE OR REPLACE MACRO date_format(d, fmt) AS strftime(fmt, d::TIMESTAMP)",
    "CREATE OR REPLACE MACRO unix_timestamp(d) AS epoch(d::TIMESTAMP)::BIGINT",
    "CREATE OR REPLACE MACRO from_unixtime(n) AS make_timestamp(n::BIGINT * 1000000)",
    "CREATE OR REPLACE MACRO last_day(d) AS "
      "(date_trunc('month', d::DATE) + INTERVAL 1 MONTH - INTERVAL 1 DAY)::DATE",
    "CREATE OR REPLACE MACRO if(cond, a, b) AS CASE WHEN cond THEN a ELSE b END",
  };

  for (const char *sql : macros)
  {
    auto r= conn.Query(sql);
    if (r->HasError())
      sql_print_warning("DuckDB: failed to install built-in compat macro: %s — %s",
                        sql, r->GetErrorObject().Message().c_str());
  }
}

static duckdb::DuckDB *registry_get(const std::string &path)
{
  mysql_mutex_lock(&g_duckdb_mutex);
  auto it= g_duckdb_registry.find(path);
  if (it != g_duckdb_registry.end())
  {
    it->second.refcount++;
    mysql_mutex_unlock(&g_duckdb_mutex);
    return it->second.db;
  }
  duckdb::DuckDB *db= nullptr;
  try
  {
    duckdb::DBConfig config;
    // 0 means "not set" — let DuckDB use all available cores by default
    if (duckdb_max_threads > 0)
      config.options.maximum_threads= duckdb_max_threads;
    db= new duckdb::DuckDB(path.c_str(), &config);
    install_mariadb_compat_macros(db);
  }
  catch (const std::exception &e)
  {
    mysql_mutex_unlock(&g_duckdb_mutex);
    sql_print_error("DuckDB: failed to open %s: %s", path.c_str(), e.what());
    return nullptr;
  }
  g_duckdb_registry[path]= {db, 1};
  mysql_mutex_unlock(&g_duckdb_mutex);
  return db;
}

static void registry_release(const std::string &path)
{
  mysql_mutex_lock(&g_duckdb_mutex);
  auto it= g_duckdb_registry.find(path);
  if (it != g_duckdb_registry.end())
  {
    if (--it->second.refcount == 0)
    {
      delete it->second.db;
      g_duckdb_registry.erase(it);
    }
  }
  mysql_mutex_unlock(&g_duckdb_mutex);
}

// ---------------------------------------------------------------------------
// Path helpers
//
// MariaDB passes `name` as  <datadir><db>/<table>  (no extension).
// We store all DuckDB tables in one global file:
//   <datadir>#duckdb/global.duckdb
// Tables inside are schema-qualified as  <db>.<table>.
// ---------------------------------------------------------------------------

struct DuckDBPath {
  std::string db_name;           // e.g. test
  std::string table_name;        // e.g. sales
  std::string db_file;           // e.g. /var/lib/mysql/#duckdb/global.duckdb
  std::string qualified_table;   // e.g. test.sales
};

static DuckDBPath parse_path(const char *name)
{
  DuckDBPath p;
  std::string s(name);
  size_t last= s.rfind('/');
  p.table_name= s.substr(last + 1);
  std::string dir= s.substr(0, last);
  size_t prev= dir.rfind('/');
  p.db_name= dir.substr(prev + 1);

  // Use the absolute datadir to avoid cwd-relative path issues.
  // mysql_real_data_home always has a trailing slash.
  std::string abs_datadir= mysql_real_data_home;

  // One global DuckDB file for all databases — avoids schema naming conflicts
  p.db_file= abs_datadir + "#duckdb/global.duckdb";
  p.qualified_table= p.db_name + "." + p.table_name;
  return p;
}

static int ensure_duckdb_dir()
{
  std::string dir= std::string(mysql_real_data_home) + "#duckdb";
  if (my_mkdir(dir.c_str(), 0777, MYF(0)) != 0 && errno != EEXIST)
  {
    sql_print_error("DuckDB: cannot create directory %s: errno=%d",
                    dir.c_str(), errno);
    return 1;
  }
  return 0;
}

// ---------------------------------------------------------------------------
// Plugin internals
// ---------------------------------------------------------------------------

static handlerton *duckdb_hton;

static const char *ha_duckdb_exts[] = { NullS };

// ---------------------------------------------------------------------------
// System variables
// ---------------------------------------------------------------------------

static MYSQL_SYSVAR_ULONG(
  max_threads,
  duckdb_max_threads,
  PLUGIN_VAR_RQCMDARG,
  "Maximum number of CPU threads DuckDB may use (0 = all available cores)",
  NULL, NULL,
  0,    /* default */
  0,    /* min */
  1024, /* max */
  0);

static char *duckdb_last_result_var= const_cast<char*>("");

static void set_last_result(const std::string &msg)
{
  if (duckdb_last_result_var && *duckdb_last_result_var)
    my_free(duckdb_last_result_var);
  duckdb_last_result_var= my_strdup(PSI_NOT_INSTRUMENTED,
                                    msg.c_str(), MYF(MY_WME));
}

static ulong duckdb_reload_extensions_var= 0;

static void duckdb_reload_extensions_update(THD *thd,
                                            struct st_mysql_sys_var *var,
                                            void *var_ptr, const void *save)
{
  std::string db_file= std::string(mysql_real_data_home) + "#duckdb/global.duckdb";
  duckdb::DuckDB *db= registry_get(db_file);
  if (!db)
  {
    sql_print_warning("DuckDB: reload_extensions — no open database to reload");
    return;
  }
  install_mariadb_compat_macros(db);
  registry_release(db_file);
  sql_print_information("DuckDB: extensions reloaded from holyduck_duckdb_extensions.sql");
}

static MYSQL_SYSVAR_ULONG(
  reload_extensions,
  duckdb_reload_extensions_var,
  PLUGIN_VAR_RQCMDARG,
  "Set to any value to reload holyduck_duckdb_extensions.sql without restarting MariaDB",
  NULL, duckdb_reload_extensions_update,
  0,    /* default */
  0,    /* min */
  ULONG_MAX, /* max */
  0);

static char *duckdb_execute_script_var= NULL;

static void duckdb_execute_script_update(THD *thd,
                                         struct st_mysql_sys_var *var,
                                         void *var_ptr, const void *save)
{
  const char *path= *(const char **)save;
  if (!path || !*path) return;

  std::ifstream f(path);
  if (!f.good())
  {
    sql_print_error("DuckDB: execute_script — cannot open file: %s", path);
    return;
  }

  std::string text((std::istreambuf_iterator<char>(f)),
                    std::istreambuf_iterator<char>());
  auto stmts= parse_sql_statements(text);

  std::string db_file= std::string(mysql_real_data_home) + "#duckdb/global.duckdb";
  duckdb::DuckDB *db= registry_get(db_file);
  if (!db)
  {
    sql_print_error("DuckDB: execute_script — no open database");
    return;
  }

  duckdb::Connection conn(*db);
  run_macro_statements(conn, stmts, path);
  registry_release(db_file);
  std::string ok= std::string(path) + " — " + std::to_string(stmts.size()) + " statement(s)";
  sql_print_information("DuckDB: executed script %s", ok.c_str());
  set_last_result(ok);
}

static MYSQL_SYSVAR_STR(
  execute_script,
  duckdb_execute_script_var,
  PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_MEMALLOC,
  "Path to a DuckDB SQL script to execute immediately (runs on SET, not persisted)",
  NULL, duckdb_execute_script_update,
  NULL);

static char *duckdb_execute_sql_var= NULL;

static void duckdb_execute_sql_update(THD *thd,
                                      struct st_mysql_sys_var *var,
                                      void *var_ptr, const void *save)
{
  const char *sql= *(const char **)save;
  if (!sql || !*sql) return;

  std::string db_file= std::string(mysql_real_data_home) + "#duckdb/global.duckdb";
  duckdb::DuckDB *db= registry_get(db_file);
  if (!db)
  {
    sql_print_error("DuckDB: execute_sql — no open database");
    return;
  }

  duckdb::Connection conn(*db);
  auto r= conn.Query(std::string(sql));
  if (r->HasError())
  {
    std::string err= r->GetErrorObject().Message();
    sql_print_error("DuckDB: execute_sql failed: %s\nSQL: %s", err.c_str(), sql);
    set_last_result("Error: " + err);
  }
  else
  {
    std::string ok= std::to_string(r->RowCount()) + " row(s)";
    sql_print_information("DuckDB: execute_sql succeeded (%s)", ok.c_str());
    set_last_result(ok);
  }

  registry_release(db_file);
}

static MYSQL_SYSVAR_STR(
  last_result,
  duckdb_last_result_var,
  PLUGIN_VAR_READONLY | PLUGIN_VAR_NOCMDOPT,
  "Result of the last duckdb_execute_sql or duckdb_execute_script call",
  NULL, NULL,
  "");

static MYSQL_SYSVAR_STR(
  execute_sql,
  duckdb_execute_sql_var,
  PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_MEMALLOC,
  "Execute a DDL/DML statement directly in DuckDB. Result rows from SELECT are discarded — use for CREATE, INSERT, DROP, COPY etc. Check duckdb_last_result for outcome.",
  NULL, duckdb_execute_sql_update,
  NULL);

static struct st_mysql_sys_var *duckdb_system_variables[]=
{
  MYSQL_SYSVAR(max_threads),
  MYSQL_SYSVAR(reload_extensions),
  MYSQL_SYSVAR(execute_script),
  MYSQL_SYSVAR(execute_sql),
  MYSQL_SYSVAR(last_result),
  NULL
};

#ifdef HAVE_PSI_INTERFACE
static PSI_mutex_key dk_key_mutex_DuckDB_share_mutex;
static PSI_mutex_key dk_key_mutex_g_duckdb_mutex;

static PSI_mutex_info all_duckdb_mutexes[]=
{
  { &dk_key_mutex_DuckDB_share_mutex, "DuckDB_share::mutex", 0 },
  { &dk_key_mutex_g_duckdb_mutex,     "duckdb_global_mutex", 0 }
};

static void init_duckdb_psi_keys()
{
  const char *category= "duckdb";
  int count= array_elements(all_duckdb_mutexes);
  mysql_mutex_register(category, all_duckdb_mutexes, count);
}
#endif

static handler *create_duckdb_handler(handlerton *hton, TABLE_SHARE *table,
                                      MEM_ROOT *mem_root);

static select_handler  *create_duckdb_select_handler(THD *thd,
                                                      SELECT_LEX *sel,
                                                      SELECT_LEX_UNIT *unit);
static select_handler  *create_duckdb_unit_handler(THD *thd,
                                                    SELECT_LEX_UNIT *unit);
static derived_handler *create_duckdb_derived_handler(THD *thd,
                                                      TABLE_LIST *derived);
static int duckdb_discover_table(handlerton *hton, THD *thd, TABLE_SHARE *share);
static int duckdb_discover_table_names(handlerton *hton, const LEX_CSTRING *db,
                                       MY_DIR *dir,
                                       handlerton::discovered_list *result);
static int duckdb_discover_table_existence(handlerton *hton, const char *db,
                                           const char *table_name);
static void duckdb_kill_query(handlerton *hton, THD *thd,
                              enum thd_kill_levels level);
static bool duckdb_show_status(handlerton *hton, THD *thd,
                               stat_print_fn *stat_print,
                               enum ha_stat_type stat_type);

static int duckdb_init_func(void *p)
{
  DBUG_ENTER("duckdb_init_func");

#ifdef HAVE_PSI_INTERFACE
  init_duckdb_psi_keys();
#endif

  mysql_mutex_init(dk_key_mutex_g_duckdb_mutex, &g_duckdb_mutex,
                   MY_MUTEX_INIT_FAST);

  duckdb_hton= (handlerton *)p;
  duckdb_hton->create=          create_duckdb_handler;
  duckdb_hton->create_select=   create_duckdb_select_handler;
  duckdb_hton->create_unit=     create_duckdb_unit_handler;
  duckdb_hton->create_derived=  create_duckdb_derived_handler;
  duckdb_hton->discover_table=            duckdb_discover_table;
  duckdb_hton->discover_table_names=      duckdb_discover_table_names;
  duckdb_hton->discover_table_existence=  duckdb_discover_table_existence;
  duckdb_hton->kill_query=                duckdb_kill_query;
  duckdb_hton->show_status=               duckdb_show_status;
  duckdb_hton->close_connection=          duckdb_close_connection;
  duckdb_hton->flags=           0;
  duckdb_hton->tablefile_extensions= ha_duckdb_exts;

  DBUG_RETURN(0);
}

static int duckdb_done_func(void *)
{
  mysql_mutex_destroy(&g_duckdb_mutex);
  return 0;
}

// ---------------------------------------------------------------------------
// Table discovery
//
// Called by MariaDB when a query references a table not in its catalog.
// We query DuckDB's information_schema.columns for the name, build a
// CREATE TABLE string from the result, and hand it to MariaDB via
// init_from_sql_statement_string(). Works transparently for both DuckDB
// base tables and views — MariaDB never needs to know the difference.
// ---------------------------------------------------------------------------

// Map a DuckDB data_type string (from information_schema.columns) to a MariaDB type.
// DuckDB embeds precision/scale directly in data_type, e.g. "DECIMAL(15,2)", "VARCHAR".
static std::string duckdb_type_to_mariadb(const std::string &dtype)
{
  // Helper: strip spaces from "(p, s)" params and return "DECIMAL(p,s)" etc.
  auto with_params= [&](const std::string &base) -> std::string {
    size_t open= dtype.find('(');
    if (open == std::string::npos) return base;
    std::string r= base;
    for (size_t i= open; i < dtype.size(); i++)
      if (dtype[i] != ' ') r += dtype[i];
    return r;
  };

  // Prefix-matched parameterised types (data_type includes "(p,s)" or "(n)")
  if (dtype.find("DECIMAL") == 0 || dtype.find("NUMERIC") == 0)
    return dtype.find('(') != std::string::npos ? with_params("DECIMAL") : "DECIMAL(18,3)";
  if (dtype.find("CHARACTER VARYING") == 0)
    return dtype.find('(') != std::string::npos ? with_params("VARCHAR") : "VARCHAR(255)";
  if (dtype.find("VARCHAR") == 0)
    return dtype.find('(') != std::string::npos ? with_params("VARCHAR") : "VARCHAR(255)";
  if (dtype.find("CHARACTER") == 0)   // must come after CHARACTER VARYING
    return dtype.find('(') != std::string::npos ? with_params("CHAR") : "CHAR(1)";

  // Integer family
  if (dtype == "BIGINT")              return "BIGINT";
  if (dtype == "INTEGER")             return "INT";
  if (dtype == "SMALLINT")            return "SMALLINT";
  if (dtype == "TINYINT")             return "TINYINT";
  if (dtype == "HUGEINT")             return "DECIMAL(38,0)"; // 128-bit, no direct equivalent
  if (dtype == "UBIGINT")             return "DECIMAL(20,0)";
  if (dtype == "UINTEGER")            return "INT UNSIGNED";
  if (dtype == "USMALLINT")           return "SMALLINT UNSIGNED";
  if (dtype == "UTINYINT")            return "TINYINT UNSIGNED";

  // Floating-point
  if (dtype == "FLOAT" || dtype == "REAL")  return "FLOAT";
  if (dtype == "DOUBLE")                    return "DOUBLE";

  // Date/time
  if (dtype == "DATE")                      return "DATE";
  if (dtype == "TIME" || dtype == "TIMETZ") return "TIME";
  if (dtype == "TIMESTAMP" || dtype == "DATETIME" ||
      dtype == "TIMESTAMP WITH TIME ZONE"  ||
      dtype == "TIMESTAMPTZ")               return "DATETIME";
  if (dtype == "INTERVAL")                  return "VARCHAR(64)";

  // Boolean
  if (dtype == "BOOLEAN" || dtype == "BOOL") return "TINYINT(1)";

  // Binary / JSON
  if (dtype == "BLOB" || dtype == "BYTEA")  return "BLOB";
  if (dtype == "JSON")                      return "JSON";

  // Unknown/complex (LIST, STRUCT, MAP, etc.) — approximate as VARCHAR
  return "VARCHAR(255)";
}

static int duckdb_discover_table_existence(handlerton *hton, const char *db,
                                           const char *table_name)
{
  std::string db_file= std::string(mysql_real_data_home) + "#duckdb/global.duckdb";

  duckdb::DuckDB *duck= registry_get(db_file);
  if (!duck) return 0;

  int exists= 0;
  try
  {
    duckdb::Connection conn(*duck);
    std::string sql=
      "SELECT COUNT(*) FROM information_schema.tables "
      "WHERE table_schema = '" + std::string(db) + "' "
      "AND table_name = '" + std::string(table_name) + "'";
    auto res= conn.Query(sql);
    if (!res->HasError() && res->RowCount() > 0)
      exists= (res->GetValue(0, 0).GetValue<int64_t>() > 0) ? 1 : 0;
  }
  catch (...) {}

  registry_release(db_file);
  return exists;
}

static void duckdb_kill_query(handlerton *hton, THD *thd,
                              enum thd_kill_levels level)
{
  mysql_mutex_lock(&g_duckdb_mutex);
  auto it= g_active_connections.find(thd);
  if (it != g_active_connections.end())
    it->second->Interrupt();
  mysql_mutex_unlock(&g_duckdb_mutex);
}

static bool duckdb_show_status(handlerton *hton, THD *thd,
                               stat_print_fn *stat_print,
                               enum ha_stat_type stat_type)
{
  if (stat_type != HA_ENGINE_STATUS)
    return false;

  std::string db_file= std::string(mysql_real_data_home) + "#duckdb/global.duckdb";

  // DuckDB version
  {
    std::string ver= duckdb::DuckDB::LibraryVersion();
    stat_print(thd, "DuckDB", 6, "Version", 7, ver.c_str(), ver.length());
  }

  // Data file path
  stat_print(thd, "DuckDB", 6, "Data file", 9,
             db_file.c_str(), db_file.length());

  // Open databases and active queries (under mutex)
  mysql_mutex_lock(&g_duckdb_mutex);
  std::string open_dbs= std::to_string(g_duckdb_registry.size());
  std::string active_q= std::to_string(g_active_connections.size());
  mysql_mutex_unlock(&g_duckdb_mutex);

  stat_print(thd, "DuckDB", 6, "Open databases", 14,
             open_dbs.c_str(), open_dbs.length());
  stat_print(thd, "DuckDB", 6, "Active queries", 14,
             active_q.c_str(), active_q.length());

  return false;
}

static int duckdb_discover_table_names(handlerton *hton, const LEX_CSTRING *db,
                                       MY_DIR *dir,
                                       handlerton::discovered_list *result)
{
  DBUG_ENTER("duckdb_discover_table_names");

  std::string db_file= std::string(mysql_real_data_home) + "#duckdb/global.duckdb";
  std::string db_name(db->str, db->length);

  duckdb::DuckDB *duck= registry_get(db_file);
  if (!duck)
    DBUG_RETURN(0);  // No DuckDB file yet — return empty, not an error

  try
  {
    duckdb::Connection conn(*duck);
    std::string sql=
      "SELECT table_name FROM information_schema.tables "
      "WHERE table_schema = '" + db_name + "' "
      "AND table_schema NOT IN ('information_schema', 'pg_catalog', 'main')";

    auto res= conn.Query(sql);
    if (!res->HasError())
    {
      for (size_t i= 0; i < res->RowCount(); i++)
      {
        std::string name= res->GetValue(0, i).ToString();
        result->add_table(name.c_str(), name.length());
      }
    }
  }
  catch (...) {}

  registry_release(db_file);
  DBUG_RETURN(0);
}

static int duckdb_discover_table(handlerton *hton, THD *thd, TABLE_SHARE *share)
{
  DBUG_ENTER("duckdb_discover_table");

  std::string db_file= std::string(mysql_real_data_home) + "#duckdb/global.duckdb";
  std::string db_name(share->db.str, share->db.length);
  std::string table_name(share->table_name.str, share->table_name.length);

  duckdb::DuckDB *db= registry_get(db_file);
  if (!db)
    DBUG_RETURN(HA_ERR_NO_SUCH_TABLE);

  int rc= HA_ERR_NO_SUCH_TABLE;
  try
  {
    duckdb::Connection conn(*db);
    // data_type in DuckDB's information_schema includes precision/scale inline,
    // e.g. "DECIMAL(15,2)", "VARCHAR" (no length for unbounded), "BIGINT".
    std::string sql=
      "SELECT column_name, data_type "
      "FROM information_schema.columns "
      "WHERE table_schema = '" + db_name + "' "
      "AND table_name = '" + table_name + "' "
      "ORDER BY ordinal_position";

    auto result= conn.Query(sql);
    if (result->HasError() || result->RowCount() == 0)
    {
      registry_release(db_file);
      DBUG_RETURN(HA_ERR_NO_SUCH_TABLE);
    }

    // Build: CREATE TABLE name (col1 TYPE1, col2 TYPE2, ...) ENGINE=DUCKDB
    std::string create= "CREATE TABLE `" + db_name + "`.`" + table_name + "` (";
    for (size_t i= 0; i < result->RowCount(); i++)
    {
      if (i > 0) create += ", ";
      std::string col_name= result->GetValue(0, i).ToString();
      std::string col_type= result->GetValue(1, i).ToString();
      create += "`" + col_name + "` " + duckdb_type_to_mariadb(col_type);
    }
    create += ") ENGINE=DUCKDB";

    rc= share->init_from_sql_statement_string(thd, false,
                                              create.c_str(), create.length());
  }
  catch (const std::exception &e)
  {
    sql_print_error("DuckDB discover_table exception: %s", e.what());
    rc= HA_ERR_NO_SUCH_TABLE;
  }

  registry_release(db_file);
  DBUG_RETURN(rc);
}

static handler *create_duckdb_handler(handlerton *hton, TABLE_SHARE *table,
                                      MEM_ROOT *mem_root)
{
  return new (mem_root) ha_duckdb(hton, table);
}

// ---------------------------------------------------------------------------
// DuckDB_share
// ---------------------------------------------------------------------------

DuckDB_share::DuckDB_share()
{
  thr_lock_init(&lock);
  mysql_mutex_init(dk_key_mutex_DuckDB_share_mutex,
                   &mutex, MY_MUTEX_INIT_FAST);
}

DuckDB_share::~DuckDB_share()
{
  if (!db_file_path.empty())
    registry_release(db_file_path);
  thr_lock_delete(&lock);
  mysql_mutex_destroy(&mutex);
}

// ---------------------------------------------------------------------------
// ha_duckdb
// ---------------------------------------------------------------------------

ha_duckdb::ha_duckdb(handlerton *hton, TABLE_SHARE *table_arg)
  :handler(hton, table_arg), share(nullptr), connection(nullptr),
   scan_result(nullptr), scan_row(0), bulk_appender(nullptr)
{}

ha_duckdb::~ha_duckdb()
{
  delete static_cast<duckdb::Appender*>(bulk_appender);
  delete scan_result;
}

const char **ha_duckdb::bas_ext() const { return ha_duckdb_exts; }

DuckDB_share *ha_duckdb::get_share()
{
  DuckDB_share *tmp_share;
  DBUG_ENTER("ha_duckdb::get_share");
  lock_shared_ha_data();
  if (!(tmp_share= static_cast<DuckDB_share*>(get_ha_share_ptr())))
  {
    tmp_share= new DuckDB_share;
    if (!tmp_share) goto err;
    set_ha_share_ptr(static_cast<Handler_share*>(tmp_share));
  }
err:
  unlock_shared_ha_data();
  DBUG_RETURN(tmp_share);
}

static const char *field_type_to_duckdb(Field *field)
{
  switch (field->type())
  {
    case MYSQL_TYPE_TINY:
    case MYSQL_TYPE_SHORT:
    case MYSQL_TYPE_LONG:
    case MYSQL_TYPE_INT24:      return "INTEGER";
    case MYSQL_TYPE_LONGLONG:   return "BIGINT";
    case MYSQL_TYPE_FLOAT:      return "FLOAT";
    case MYSQL_TYPE_DOUBLE:     return "DOUBLE";
    case MYSQL_TYPE_DECIMAL:
    case MYSQL_TYPE_NEWDECIMAL: return "DECIMAL";
    case MYSQL_TYPE_DATE:       return "DATE";
    case MYSQL_TYPE_TIME:       return "TIME";
    case MYSQL_TYPE_DATETIME:
    case MYSQL_TYPE_TIMESTAMP:  return "TIMESTAMP";
    default:                    return "VARCHAR";
  }
}

std::string ha_duckdb::build_create_sql(TABLE *table_arg)
{
  std::ostringstream sql;
  sql << "CREATE TABLE IF NOT EXISTS " << duckdb_table_name << " (";

  for (uint i= 0; i < table_arg->s->fields; i++)
  {
    Field *field= table_arg->field[i];
    if (i > 0) sql << ", ";
    sql << '"' << field->field_name.str << "\" " << field_type_to_duckdb(field);
  }
  sql << ")";
  return sql.str();
}

// ---------------------------------------------------------------------------
// Table operations
// ---------------------------------------------------------------------------

int ha_duckdb::open(const char *name, int mode, uint test_if_locked)
{
  DBUG_ENTER("ha_duckdb::open");

  DuckDBPath p= parse_path(name);
  db_file_path=     p.db_file;
  db_name=          p.db_name;
  duckdb_table_name= p.qualified_table;

  if (!(share= get_share()))
    DBUG_RETURN(1);

  // First open for this table: register the db file path on the share
  if (share->db_file_path.empty())
    share->db_file_path= db_file_path;

  duckdb::DuckDB *db= registry_get(db_file_path);
  if (!db) DBUG_RETURN(1);

  try { connection= new duckdb::Connection(*db); }
  catch (const std::exception &e)
  {
    registry_release(db_file_path);
    sql_print_error("DuckDB: connection failed: %s", e.what());
    DBUG_RETURN(1);
  }

  thr_lock_data_init(&share->lock, &lock, NULL);
  DBUG_RETURN(0);
}

int ha_duckdb::close(void)
{
  DBUG_ENTER("ha_duckdb::close");
  delete scan_result; scan_result= nullptr;
  delete connection;  connection= nullptr;
  if (!db_file_path.empty())
    registry_release(db_file_path);
  DBUG_RETURN(0);
}

int ha_duckdb::create(const char *name, TABLE *table_arg,
                      HA_CREATE_INFO *create_info)
{
  DBUG_ENTER("ha_duckdb::create");

  DuckDBPath p= parse_path(name);
  duckdb_table_name= p.qualified_table;
  db_name=           p.db_name;


  if (ensure_duckdb_dir())
    DBUG_RETURN(1);

  std::string create_sql= build_create_sql(table_arg);

  try
  {
    duckdb::DuckDB *db= registry_get(p.db_file);
    if (!db) DBUG_RETURN(1);

    duckdb::Connection conn(*db);

    // Ensure the schema exists
    std::string schema_sql= "CREATE SCHEMA IF NOT EXISTS " + p.db_name;
    auto r= conn.Query(schema_sql);
    if (r->HasError())
    {
      registry_release(p.db_file);
      sql_print_error("DuckDB: schema creation failed: %s",
                      r->GetErrorObject().Message().c_str());
      DBUG_RETURN(1);
    }

    r= conn.Query(create_sql);
    if (r->HasError())
    {
      registry_release(p.db_file);
      sql_print_error("DuckDB: table creation failed: %s",
                      r->GetErrorObject().Message().c_str());
      DBUG_RETURN(1);
    }

    // Create indexes in DuckDB — used by DuckDB's own query planner
    for (uint i= 0; i < table_arg->s->keys; i++)
    {
      KEY *key= &table_arg->key_info[i];
      bool is_unique= (key->flags & HA_NOSAME);
      std::ostringstream idx;
      idx << (is_unique ? "CREATE UNIQUE INDEX " : "CREATE INDEX ");
      idx << "idx_" << p.table_name << "_" << key->name.str;
      idx << " ON " << p.qualified_table << " (";
      for (uint j= 0; j < key->user_defined_key_parts; j++)
      {
        if (j > 0) idx << ", ";
        idx << '"' << key->key_part[j].field->field_name.str << '"';
      }
      idx << ")";
      auto ri= conn.Query(idx.str());
      if (ri->HasError())
        sql_print_warning("DuckDB: index creation failed for %s: %s",
                          key->name.str, ri->GetErrorObject().Message().c_str());
    }

    registry_release(p.db_file);
    sql_print_information("DuckDB: created table %s", p.qualified_table.c_str());
  }
  catch (const std::exception &e)
  {
    sql_print_error("DuckDB: exception in create: %s", e.what());
    DBUG_RETURN(1);
  }

  DBUG_RETURN(0);
}

int ha_duckdb::delete_table(const char *name)
{
  DBUG_ENTER("ha_duckdb::delete_table");
  DuckDBPath p= parse_path(name);
  try
  {
    duckdb::DuckDB *db= registry_get(p.db_file);
    if (db)
    {
      duckdb::Connection conn(*db);
      conn.Query("DROP TABLE IF EXISTS " + p.qualified_table);
      registry_release(p.db_file);
    }
  }
  catch (...) {}
  DBUG_RETURN(0);
}

int ha_duckdb::truncate()
{
  DBUG_ENTER("ha_duckdb::truncate");
  auto r= connection->Query("TRUNCATE " + duckdb_table_name);
  if (r->HasError())
  {
    sql_print_error("DuckDB: TRUNCATE failed: %s",
                    r->GetErrorObject().Message().c_str());
    DBUG_RETURN(HA_ERR_INTERNAL_ERROR);
  }
  DBUG_RETURN(0);
}

int ha_duckdb::rename_table(const char *from, const char *to)
{
  DBUG_ENTER("ha_duckdb::rename_table");
  DuckDBPath fp= parse_path(from);
  DuckDBPath tp= parse_path(to);
  try
  {
    duckdb::DuckDB *db= registry_get(fp.db_file);
    if (db)
    {
      duckdb::Connection conn(*db);
      std::string sql= "ALTER TABLE " + fp.qualified_table +
                       " RENAME TO " + tp.table_name;
      conn.Query(sql);
      registry_release(fp.db_file);
    }
  }
  catch (...) {}
  DBUG_RETURN(0);
}

// ---------------------------------------------------------------------------
// Bulk insert — keep one Appender open across all write_row() calls
// ---------------------------------------------------------------------------

void ha_duckdb::start_bulk_insert(ha_rows rows, uint flags)
{
  if (!connection) return;

  // For REPLACE, skip the Appender — write_row() will use INSERT OR REPLACE
  THD *thd= table->in_use;
  if (thd && (thd->lex->sql_command == SQLCOM_REPLACE ||
              thd->lex->sql_command == SQLCOM_REPLACE_SELECT))
    return;

  try
  {
    bulk_appender= new duckdb::Appender(*connection, db_name,
                                        table->s->table_name.str);
  }
  catch (const std::exception &e)
  {
    sql_print_error("DuckDB: start_bulk_insert failed: %s", e.what());
    bulk_appender= nullptr;
  }
}

int ha_duckdb::end_bulk_insert()
{
  if (!bulk_appender) return 0;
  duckdb::Appender *app= static_cast<duckdb::Appender*>(bulk_appender);
  try
  {
    app->Close();
  }
  catch (const std::exception &e)
  {
    delete app;
    bulk_appender= nullptr;
    std::string msg= e.what();
    if (msg.find("\"exception_type\":\"Constraint\"") != std::string::npos)
      return HA_ERR_FOUND_DUPP_KEY;
    sql_print_error("DuckDB: end_bulk_insert flush failed: %s", e.what());
    return HA_ERR_INTERNAL_ERROR;
  }
  delete app;
  bulk_appender= nullptr;
  return 0;
}

// ---------------------------------------------------------------------------
// Row write
// ---------------------------------------------------------------------------

int ha_duckdb::write_row(const uchar *buf)
{
  DBUG_ENTER("ha_duckdb::write_row");
  if (!connection) DBUG_RETURN(HA_ERR_INTERNAL_ERROR);

  // Bulk path: use Appender (fast, no constraint checking — caller ensures clean data)
  if (bulk_appender)
  {
    try
    {
      duckdb::Appender *appender= static_cast<duckdb::Appender*>(bulk_appender);
      appender->BeginRow();
      for (uint i= 0; i < table->s->fields; i++)
      {
        Field *field= table->field[i];
        if (field->is_null()) { appender->Append<nullptr_t>(nullptr); continue; }
        switch (field->type())
        {
          case MYSQL_TYPE_TINY:
          case MYSQL_TYPE_SHORT:
          case MYSQL_TYPE_LONG:
          case MYSQL_TYPE_INT24:
            appender->Append<int32_t>((int32_t)field->val_int()); break;
          case MYSQL_TYPE_LONGLONG:
            appender->Append<int64_t>(field->val_int()); break;
          case MYSQL_TYPE_FLOAT:
            appender->Append<float>((float)field->val_real()); break;
          case MYSQL_TYPE_DOUBLE:
            appender->Append<double>(field->val_real()); break;
          default:
          {
            String s; field->val_str(&s, &s);
            appender->Append(duckdb::Value(std::string(s.ptr(), s.length())));
            break;
          }
        }
      }
      appender->EndRow();
    }
    catch (const std::exception &e)
    {
      std::string msg= e.what();
      if (msg.find("\"exception_type\":\"Constraint\"") != std::string::npos)
        DBUG_RETURN(HA_ERR_FOUND_DUPP_KEY);
      sql_print_error("DuckDB: write_row (bulk) failed: %s", e.what());
      DBUG_RETURN(HA_ERR_INTERNAL_ERROR);
    }
    DBUG_RETURN(0);
  }

  // Single-row path: use SQL INSERT so DuckDB enforces PK/UNIQUE constraints
  {
    std::ostringstream sql;
    THD *thd= table->in_use;
    bool is_replace= thd && (thd->lex->sql_command == SQLCOM_REPLACE ||
                             thd->lex->sql_command == SQLCOM_REPLACE_SELECT);
    sql << (is_replace ? "INSERT OR REPLACE INTO " : "INSERT INTO ")
        << duckdb_table_name << " VALUES (";
    for (uint i= 0; i < table->s->fields; i++)
    {
      Field *field= table->field[i];
      if (i > 0) sql << ", ";
      if (field->is_null()) { sql << "NULL"; continue; }
      switch (field->type())
      {
        case MYSQL_TYPE_TINY:
        case MYSQL_TYPE_SHORT:
        case MYSQL_TYPE_LONG:
        case MYSQL_TYPE_INT24:
          sql << (int32_t)field->val_int(); break;
        case MYSQL_TYPE_LONGLONG:
          sql << field->val_int(); break;
        case MYSQL_TYPE_FLOAT:
        case MYSQL_TYPE_DOUBLE:
          sql << field->val_real(); break;
        default:
        {
          String s; field->val_str(&s, &s);
          std::string v(s.ptr(), s.length());
          sql << '\'';
          for (char c : v) { if (c == '\'') sql << '\''; sql << c; }
          sql << '\'';
          break;
        }
      }
    }
    sql << ")";

    auto r= connection->Query(sql.str());
    if (r->HasError())
    {
      if (r->GetErrorObject().Type() == duckdb::ExceptionType::CONSTRAINT)
        DBUG_RETURN(HA_ERR_FOUND_DUPP_KEY);
      sql_print_error("DuckDB: write_row failed: %s",
                      r->GetErrorObject().Message().c_str());
      DBUG_RETURN(HA_ERR_INTERNAL_ERROR);
    }
    DBUG_RETURN(0);
  }
}

// Serialize all fields from buf into a DuckDB SQL fragment of the form:
//   "col1"=val1 AND "col2"=val2 ...   (for WHERE clauses)
//   "col1"=val1, "col2"=val2 ...      (for SET clauses, sep=", ")
// Uses move_field_offset() to read values from an arbitrary row buffer.
static std::string row_to_sql_fragment(TABLE *table, const uchar *buf,
                                       const char *sep)
{
  my_ptrdiff_t diff= (my_ptrdiff_t)(buf - table->record[0]);
  std::string out;

  for (uint i= 0; i < table->s->fields; i++)
  {
    Field *field= table->field[i];
    field->move_field_offset(diff);

    if (!out.empty()) out += sep;
    out += '"';
    out += field->field_name.str;
    out += '"';
    out += '=';

    if (field->is_null())
    {
      out += "NULL";
    }
    else
    {
      switch (field->type())
      {
        case MYSQL_TYPE_TINY:
        case MYSQL_TYPE_SHORT:
        case MYSQL_TYPE_LONG:
        case MYSQL_TYPE_INT24:
        {
          char tmp[32];
          snprintf(tmp, sizeof(tmp), "%d", (int32_t)field->val_int());
          out += tmp;
          break;
        }
        case MYSQL_TYPE_LONGLONG:
        {
          char tmp[32];
          snprintf(tmp, sizeof(tmp), "%lld", (long long)field->val_int());
          out += tmp;
          break;
        }
        case MYSQL_TYPE_FLOAT:
        case MYSQL_TYPE_DOUBLE:
        {
          char tmp[64];
          snprintf(tmp, sizeof(tmp), "%.17g", field->val_real());
          out += tmp;
          break;
        }
        default:
        {
          String s;
          field->val_str(&s, &s);
          out += '\'';
          for (uint j= 0; j < s.length(); j++)
          {
            if (s[j] == '\'') out += '\'';
            out += s[j];
          }
          out += '\'';
          break;
        }
      }
    }

    field->move_field_offset(-diff);
  }
  return out;
}

int ha_duckdb::update_row(const uchar *old_data, const uchar *new_data)
{
  DBUG_ENTER("ha_duckdb::update_row");
  if (!connection) DBUG_RETURN(HA_ERR_INTERNAL_ERROR);

  std::string sql= "UPDATE " + duckdb_table_name +
                   " SET "   + row_to_sql_fragment(table, new_data, ", ") +
                   " WHERE " + row_to_sql_fragment(table, old_data, " AND ");

  auto r= connection->Query(sql);
  if (r->HasError())
  {
    sql_print_error("DuckDB: update_row failed: %s",
                    r->GetErrorObject().Message().c_str());
    DBUG_RETURN(HA_ERR_INTERNAL_ERROR);
  }
  DBUG_RETURN(0);
}

int ha_duckdb::delete_row(const uchar *buf)
{
  DBUG_ENTER("ha_duckdb::delete_row");
  if (!connection) DBUG_RETURN(HA_ERR_INTERNAL_ERROR);

  std::string sql= "DELETE FROM " + duckdb_table_name +
                   " WHERE " + row_to_sql_fragment(table, buf, " AND ");

  auto r= connection->Query(sql);
  if (r->HasError())
  {
    sql_print_error("DuckDB: delete_row failed: %s",
                    r->GetErrorObject().Message().c_str());
    DBUG_RETURN(HA_ERR_INTERNAL_ERROR);
  }
  DBUG_RETURN(0);
}

// ---------------------------------------------------------------------------
// Scan operations
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Condition pushdown
// ---------------------------------------------------------------------------

const COND *ha_duckdb::cond_push(const COND *cond)
{
  // Serialize the condition to SQL using MariaDB's own printer, then apply
  // the same rewrite passes used for pushed-down SELECT queries.
  String str;
  const_cast<COND *>(cond)->print(&str, QT_ORDINARY);
  std::string where(str.ptr(), str.length());
  for (char &c : where)
    if (c == '`') c = '"';
  where= rewrite_mariadb_sql(where);

  // Strip any remaining two-part "alias"."col" → "col" qualifiers.
  // cond_push() delivers a predicate for THIS table only, so column refs
  // like "s"."sale_date" are meaningless to DuckDB's single-table query.
  // strip_db_qualifier() already reduced three-part refs; here we drop the
  // leading "alias". from any remaining two-part refs.
  {
    std::string out;
    out.reserve(where.size());
    size_t i= 0, n= where.size();
    while (i < n)
    {
      if (where[i] != '"') { out += where[i++]; continue; }
      // Read p1 (quoted identifier)
      size_t p1s= i++;
      while (i < n && where[i] != '"') i++;
      if (i < n) i++;           // closing "
      size_t p1e= i;
      // If followed by ."  it's a two-part ref — drop the qualifier
      if (i < n && where[i] == '.' && i + 1 < n && where[i + 1] == '"')
      {
        i++;                    // skip '.'
        // p2 — keep only this part
        out += where[i];        // opening "
        i++;
        while (i < n && where[i] != '"') out += where[i++];
        if (i < n) { out += where[i++]; }  // closing "
      }
      else
      {
        out += where.substr(p1s, p1e - p1s);
      }
    }
    where= out;
  }

  pushed_where= where;
  return nullptr;  // we handle the entire condition
}

void ha_duckdb::cond_pop()
{
  pushed_where.clear();
}

// ---------------------------------------------------------------------------
// Direct UPDATE / DELETE pushdown
//
// When HA_CAN_DIRECT_UPDATE_AND_DELETE is set, MariaDB:
//   1. Calls cond_push() with the WHERE condition → stored in pushed_where
//   2. Calls info_push(UPDATE_FIELDS, ...) + info_push(UPDATE_VALUES, ...)
//   3. Calls direct_update_rows_init() / direct_delete_rows_init()
//   4. Calls direct_update_rows() / direct_delete_rows() once to execute
//
// This pushes the entire UPDATE/DELETE as a single DuckDB statement,
// avoiding the expensive row-at-a-time update_row()/delete_row() path.
// ---------------------------------------------------------------------------

int ha_duckdb::info_push(uint info_type, void *info)
{
  if (info_type == INFO_KIND_UPDATE_FIELDS)
    update_fields= static_cast<List<Item> *>(info);
  else if (info_type == INFO_KIND_UPDATE_VALUES)
    update_values= static_cast<List<Item> *>(info);
  return 0;
}

int ha_duckdb::direct_update_rows_init(List<Item> *)
{
  return (connection && update_fields && update_values) ? 0
                                                        : HA_ERR_WRONG_COMMAND;
}

int ha_duckdb::direct_update_rows(ha_rows *update_rows, ha_rows *found_rows)
{
  DBUG_ENTER("ha_duckdb::direct_update_rows");
  if (!connection || !update_fields || !update_values)
    DBUG_RETURN(HA_ERR_INTERNAL_ERROR);

  // Build SET clause by printing each field=value pair
  std::string set_clause;
  List_iterator<Item> fi(*update_fields);
  List_iterator<Item> vi(*update_values);
  Item *fld, *val;
  while ((fld= fi++) && (val= vi++))
  {
    String fs, vs;
    fld->print(&fs, QT_ORDINARY);
    val->print(&vs, QT_ORDINARY);

    // Convert backticks to double-quotes and apply rewrite pass on values
    std::string col(fs.ptr(), fs.length());
    std::string v(vs.ptr(), vs.length());
    for (char &c : col) if (c == '`') c = '"';
    for (char &c : v)   if (c == '`') c = '"';
    v= rewrite_mariadb_sql(v);

    // Strip any table qualifier from the column name
    size_t dot= col.rfind('.');
    if (dot != std::string::npos) col= col.substr(dot + 1);

    if (!set_clause.empty()) set_clause += ", ";
    set_clause += col + "=" + v;
  }

  std::string sql= "UPDATE " + duckdb_table_name + " SET " + set_clause;
  if (!pushed_where.empty())
    sql += " WHERE " + pushed_where;

  auto r= connection->Query(sql);
  if (r->HasError())
  {
    sql_print_error("DuckDB: direct_update_rows failed: %s",
                    r->GetErrorObject().Message().c_str());
    DBUG_RETURN(HA_ERR_INTERNAL_ERROR);
  }

  ha_rows affected= (ha_rows)r->GetValue<int64_t>(0, 0);
  *update_rows= affected;
  *found_rows=  affected;
  update_fields= nullptr;
  update_values= nullptr;
  DBUG_RETURN(0);
}

int ha_duckdb::direct_delete_rows_init()
{
  return connection ? 0 : HA_ERR_WRONG_COMMAND;
}

int ha_duckdb::direct_delete_rows(ha_rows *delete_rows)
{
  DBUG_ENTER("ha_duckdb::direct_delete_rows");
  if (!connection) DBUG_RETURN(HA_ERR_INTERNAL_ERROR);

  std::string sql= "DELETE FROM " + duckdb_table_name;
  if (!pushed_where.empty())
    sql += " WHERE " + pushed_where;

  auto r= connection->Query(sql);
  if (r->HasError())
  {
    sql_print_error("DuckDB: direct_delete_rows failed: %s",
                    r->GetErrorObject().Message().c_str());
    DBUG_RETURN(HA_ERR_INTERNAL_ERROR);
  }

  *delete_rows= (ha_rows)r->GetValue<int64_t>(0, 0);
  DBUG_RETURN(0);
}

// ---------------------------------------------------------------------------
// In-place ALTER TABLE — index creation and deletion
//
// MariaDB routes CREATE INDEX / DROP INDEX through the inplace alter API.
// We accept only pure index add/drop operations and push them straight to
// DuckDB.  MariaDB's query planner never uses these indexes (index_flags()
// returns no read-capability bits), but DuckDB's own planner uses them
// on pushed-down queries.
// ---------------------------------------------------------------------------

enum_alter_inplace_result
ha_duckdb::check_if_supported_inplace_alter(TABLE *altered_table,
                                            Alter_inplace_info *ha_alter_info)
{
  const ulonglong index_ops= ALTER_ADD_INDEX                      |
                             ALTER_DROP_INDEX                     |
                             ALTER_ADD_UNIQUE_INDEX               |
                             ALTER_DROP_UNIQUE_INDEX              |
                             ALTER_ADD_NON_UNIQUE_NON_PRIM_INDEX  |
                             ALTER_DROP_NON_UNIQUE_NON_PRIM_INDEX |
                             ALTER_INDEX_ORDER;

  const ulonglong col_ops=   ALTER_ADD_STORED_BASE_COLUMN        |
                             ALTER_DROP_STORED_COLUMN            |
                             ALTER_RENAME_COLUMN                 |
                             ALTER_CHANGE_COLUMN_DEFAULT         |
                             ALTER_COLUMN_ORDER                  |
                             ALTER_COLUMN_NAME                   |
                             ALTER_COLUMN_NULLABLE               |
                             ALTER_COLUMN_NOT_NULLABLE;

  if (ha_alter_info->handler_flags & ~(index_ops | col_ops))
    return HA_ALTER_INPLACE_NOT_SUPPORTED;

  return HA_ALTER_INPLACE_NO_LOCK;
}

bool ha_duckdb::inplace_alter_table(TABLE *altered_table,
                                    Alter_inplace_info *ha_alter_info)
{
  if (!connection) return true;

  const ulonglong flags= ha_alter_info->handler_flags;

  // --- ADD COLUMN ---
  if (flags & ALTER_ADD_STORED_BASE_COLUMN)
  {
    // Find fields present in altered_table but not in original table
    for (uint i= 0; i < altered_table->s->fields; i++)
    {
      Field *new_f= altered_table->field[i];
      bool exists= false;
      for (uint j= 0; j < table->s->fields; j++)
      {
        if (strcasecmp(new_f->field_name.str,
                       table->field[j]->field_name.str) == 0)
        { exists= true; break; }
      }
      if (exists) continue;

      std::string sql= "ALTER TABLE " + duckdb_table_name +
                       " ADD COLUMN \"" + new_f->field_name.str +
                       "\" " + field_type_to_duckdb(new_f);
      auto r= connection->Query(sql);
      if (r->HasError())
      {
        sql_print_error("DuckDB: ADD COLUMN %s failed: %s",
                        new_f->field_name.str,
                        r->GetErrorObject().Message().c_str());
        return true;
      }
      sql_print_information("DuckDB: added column %s to %s",
                            new_f->field_name.str, duckdb_table_name.c_str());
    }
  }

  // --- DROP COLUMN ---
  if (flags & ALTER_DROP_STORED_COLUMN)
  {
    // Find fields present in original table but not in altered_table
    for (uint i= 0; i < table->s->fields; i++)
    {
      Field *old_f= table->field[i];
      bool exists= false;
      for (uint j= 0; j < altered_table->s->fields; j++)
      {
        if (strcasecmp(old_f->field_name.str,
                       altered_table->field[j]->field_name.str) == 0)
        { exists= true; break; }
      }
      if (exists) continue;

      std::string sql= "ALTER TABLE " + duckdb_table_name +
                       " DROP COLUMN \"" + old_f->field_name.str + "\"";
      auto r= connection->Query(sql);
      if (r->HasError())
      {
        sql_print_error("DuckDB: DROP COLUMN %s failed: %s",
                        old_f->field_name.str,
                        r->GetErrorObject().Message().c_str());
        return true;
      }
      sql_print_information("DuckDB: dropped column %s from %s",
                            old_f->field_name.str, duckdb_table_name.c_str());
    }
  }

  // --- RENAME COLUMN ---
  if (flags & (ALTER_RENAME_COLUMN | ALTER_COLUMN_NAME))
  {
    // Walk columns that exist in both old and new tables (by position).
    // Handle rename first, then type change (DuckDB requires separate ALTER).
    uint n= MY_MIN(table->s->fields, altered_table->s->fields);
    for (uint i= 0; i < n; i++)
    {
      Field *old_f= table->field[i];
      Field *new_f= altered_table->field[i];
      const char *old_name= old_f->field_name.str;
      const char *new_name= new_f->field_name.str;

      // Rename if names differ
      if (strcasecmp(old_name, new_name) != 0)
      {
        std::string sql= "ALTER TABLE " + duckdb_table_name +
                         " RENAME COLUMN \"" + old_name +
                         "\" TO \"" + new_name + "\"";
        auto r= connection->Query(sql);
        if (r->HasError())
        {
          sql_print_error("DuckDB: RENAME COLUMN %s→%s failed: %s",
                          old_name, new_name,
                          r->GetErrorObject().Message().c_str());
          return true;
        }
        sql_print_information("DuckDB: renamed column %s → %s in %s",
                              old_name, new_name, duckdb_table_name.c_str());
      }

    }
  }

  // Add indexes
  for (uint i= 0; i < ha_alter_info->index_add_count; i++)
  {
    KEY *key= &ha_alter_info->key_info_buffer[ha_alter_info->index_add_buffer[i]];
    bool is_unique= (key->flags & HA_NOSAME);

    std::ostringstream sql;
    sql << (is_unique ? "CREATE UNIQUE INDEX " : "CREATE INDEX ");
    sql << "\"idx_" << table->s->table_name.str << "_" << key->name.str << "\"";
    sql << " ON " << duckdb_table_name << " (";
    for (uint j= 0; j < key->user_defined_key_parts; j++)
    {
      if (j > 0) sql << ", ";
      Field *f= altered_table->field[key->key_part[j].fieldnr];
      sql << '"' << f->field_name.str << '"';
    }
    sql << ")";

    auto r= connection->Query(sql.str());
    if (r->HasError())
    {
      sql_print_error("DuckDB: CREATE INDEX failed for %s: %s",
                      key->name.str, r->GetErrorObject().Message().c_str());
      return true;
    }
    sql_print_information("DuckDB: created index %s on %s",
                          key->name.str, duckdb_table_name.c_str());
  }

  // Drop indexes
  for (uint i= 0; i < ha_alter_info->index_drop_count; i++)
  {
    KEY *key= ha_alter_info->index_drop_buffer[i];
    std::string idx_name= "\"idx_" + std::string(table->s->table_name.str) +
                          "_" + key->name.str + "\"";
    std::string sql= "DROP INDEX IF EXISTS " + idx_name;

    auto r= connection->Query(sql);
    if (r->HasError())
    {
      sql_print_error("DuckDB: DROP INDEX failed for %s: %s",
                      key->name.str, r->GetErrorObject().Message().c_str());
      return true;
    }
    sql_print_information("DuckDB: dropped index %s on %s",
                          key->name.str, duckdb_table_name.c_str());
  }

  return false;
}

bool ha_duckdb::commit_inplace_alter_table(TABLE *altered_table,
                                           Alter_inplace_info *ha_alter_info,
                                           bool commit)
{
  // Index operations are executed immediately in inplace_alter_table().
  // Nothing to commit or roll back here.
  ha_alter_info->group_commit_ctx= NULL;
  return false;
}

// ---------------------------------------------------------------------------
// Full-table scan
// ---------------------------------------------------------------------------

int ha_duckdb::rnd_init(bool scan)
{
  DBUG_ENTER("ha_duckdb::rnd_init");
  if (!connection) DBUG_RETURN(HA_ERR_INTERNAL_ERROR);

  delete scan_result; scan_result= nullptr; scan_row= 0;
  scan_field_map.clear();

  try
  {
    // Build column list from read_set — only fetch columns MariaDB needs.
    std::string select_cols;
    for (uint i= 0; i < table->s->fields; i++)
    {
      if (bitmap_is_set(table->read_set, i))
      {
        if (!select_cols.empty()) select_cols += ", ";
        select_cols += '"';
        select_cols += table->s->field[i]->field_name.str;
        select_cols += '"';
        scan_field_map.push_back(i);
      }
    }
    if (select_cols.empty()) select_cols= "1";  // e.g. COUNT(*) needs no cols

    std::string sql= "SELECT " + select_cols + " FROM " + duckdb_table_name;
    if (!pushed_where.empty())
      sql += " WHERE " + pushed_where;

    // Set search_path so bare table names in pushed subquery conditions
    // (e.g. correlated subqueries injected by cond_push) resolve correctly.
    {
      THD *thd= ha_thd();
      if (thd && thd->db.str && thd->db.length)
      {
        std::string sp= "SET search_path = '";
        sp += std::string(thd->db.str, thd->db.length);
        sp += "'";
        connection->Query(sp);
      }
    }

    register_active_connection(ha_thd(), connection);
    auto r= connection->Query(sql);
    unregister_active_connection(ha_thd());
    if (r->HasError())
    {
      sql_print_error("DuckDB: rnd_init failed: %s", r->GetErrorObject().Message().c_str());
      DBUG_RETURN(HA_ERR_INTERNAL_ERROR);
    }
    scan_result= r.release();
  }
  catch (const std::exception &e)
  {
    unregister_active_connection(ha_thd());
    sql_print_error("DuckDB: rnd_init exception: %s", e.what());
    DBUG_RETURN(HA_ERR_INTERNAL_ERROR);
  }
  DBUG_RETURN(0);
}

int ha_duckdb::rnd_end()
{
  DBUG_ENTER("ha_duckdb::rnd_end");
  delete scan_result; scan_result= nullptr;
  DBUG_RETURN(0);
}

int ha_duckdb::convert_row_from_duckdb(uchar *buf, size_t row_idx,
                                        duckdb::MaterializedQueryResult *result)
{
  memset(buf, 0, table->s->null_bytes);

  // If scan_field_map is populated, only a column subset was fetched.
  // DuckDB result column j maps to MariaDB field scan_field_map[j].
  // Otherwise fall back to sequential 1:1 mapping.
  uint ncols= scan_field_map.empty() ? table->s->fields
                                     : (uint)scan_field_map.size();
  for (uint j= 0; j < ncols; j++)
  {
    uint fi= scan_field_map.empty() ? j : scan_field_map[j];
    Field *field= table->field[fi];
    duckdb::Value val= result->GetValue(j, row_idx);

    if (val.IsNull()) { field->set_null(); continue; }
    field->set_notnull();

    switch (field->type())
    {
      case MYSQL_TYPE_TINY:
      case MYSQL_TYPE_SHORT:
      case MYSQL_TYPE_LONG:
      case MYSQL_TYPE_INT24:
        field->store(val.GetValue<int32_t>(), false); break;
      case MYSQL_TYPE_LONGLONG:
        field->store(val.GetValue<int64_t>(), false); break;
      case MYSQL_TYPE_FLOAT:
      case MYSQL_TYPE_DOUBLE:
        field->store(val.GetValue<double>()); break;
      default:
      {
        std::string s= val.ToString();
        field->store(s.c_str(), s.length(), system_charset_info);
        break;
      }
    }
  }
  return 0;
}

int ha_duckdb::rnd_next(uchar *buf)
{
  DBUG_ENTER("ha_duckdb::rnd_next");
  if (!scan_result || scan_row >= scan_result->RowCount())
    DBUG_RETURN(HA_ERR_END_OF_FILE);
  convert_row_from_duckdb(buf, scan_row++, scan_result);
  DBUG_RETURN(0);
}

int ha_duckdb::rnd_pos(uchar *, uchar *)
{
  DBUG_ENTER("ha_duckdb::rnd_pos");
  DBUG_RETURN(HA_ERR_WRONG_COMMAND);
}

void ha_duckdb::position(const uchar *)
{
  DBUG_ENTER("ha_duckdb::position");
  DBUG_VOID_RETURN;
}

// ---------------------------------------------------------------------------
// Lock management
// ---------------------------------------------------------------------------

THR_LOCK_DATA **ha_duckdb::store_lock(THD *, THR_LOCK_DATA **to,
                                      enum thr_lock_type lock_type)
{
  if (lock_type != TL_IGNORE && lock.type == TL_UNLOCK)
  {
    // Upgrade any write lock to TL_WRITE: serializes concurrent writers
    // since DuckDB only supports one writer at a time. Readers are briefly
    // blocked during writes, which is acceptable given writes are infrequent.
    if (lock_type >= TL_WRITE_ALLOW_WRITE)
      lock_type= TL_WRITE;
    lock.type= lock_type;
  }
  *to++= &lock;
  return to;
}

// ---------------------------------------------------------------------------
// Statistics
// ---------------------------------------------------------------------------

int ha_duckdb::info(uint flag)
{
  DBUG_ENTER("ha_duckdb::info");
  if ((flag & HA_STATUS_VARIABLE) && connection)
  {
    try
    {
      // Row count from DuckDB's own per-table estimate — no full scan needed
      std::string tname= std::string(table->s->table_name.str);
      auto r= connection->Query(
        "SELECT estimated_size FROM duckdb_tables() "
        "WHERE schema_name='" + db_name + "' AND table_name='" + tname + "'");
      if (!r->HasError() && r->RowCount() > 0)
        stats.records= (ha_rows)r->GetValue(0, 0).GetValue<int64_t>();

      // Actual compressed on-disk size from DuckDB storage metadata
      // 262144 = DuckDB's default block size (256KB)
      r= connection->Query(
        "SELECT COUNT(DISTINCT block_id) * 262144 "
        "FROM pragma_storage_info('" + duckdb_table_name + "') "
        "WHERE block_id > 0");
      if (!r->HasError() && r->RowCount() > 0)
      {
        stats.data_file_length= (ulonglong)r->GetValue(0, 0).GetValue<int64_t>();
        if (stats.records > 0)
          stats.mean_rec_length= (ulong)(stats.data_file_length / stats.records);
      }
    }
    catch (...) {}
  }
  DBUG_RETURN(0);
}

// ---------------------------------------------------------------------------
// records_in_range helpers
// ---------------------------------------------------------------------------

// Convert a Field's current value (after key_restore) to a DuckDB SQL literal
static std::string field_to_sql_literal(Field *field)
{
  if (field->is_null()) return "NULL";

  std::ostringstream out;
  switch (field->type())
  {
    case MYSQL_TYPE_TINY:
    case MYSQL_TYPE_SHORT:
    case MYSQL_TYPE_LONG:
    case MYSQL_TYPE_INT24:
    case MYSQL_TYPE_LONGLONG:
      out << field->val_int();
      break;
    case MYSQL_TYPE_FLOAT:
    case MYSQL_TYPE_DOUBLE:
    {
      char buf[64];
      snprintf(buf, sizeof(buf), "%.15g", field->val_real());
      out << buf;
      break;
    }
    default:
    {
      String s;
      field->val_str(&s, &s);
      out << "'";
      for (uint i= 0; i < s.length(); i++)
      {
        if (s.ptr()[i] == '\'') out << "''";
        else out << s.ptr()[i];
      }
      out << "'";
    }
  }
  return out.str();
}

// Build a WHERE clause fragment from a key_range
static std::string key_range_to_where(TABLE *table, KEY *key_info,
                                      const key_range *kr, bool is_max)
{
  // Restore packed key bytes into record[0] so fields can be read
  key_restore(table->record[0], kr->key, key_info, kr->length);

  // Count how many key parts are covered by this key
  uint used_parts= 0;
  uint remaining= kr->length;
  for (uint i= 0; i < key_info->user_defined_key_parts && remaining > 0; i++)
  {
    used_parts++;
    uint store_len= key_info->key_part[i].store_length;
    remaining= (remaining >= store_len) ? remaining - store_len : 0;
  }

  // Map flag to SQL operator
  const char *op;
  switch (kr->flag)
  {
    case HA_READ_KEY_EXACT:      op= "=";  break;
    case HA_READ_KEY_OR_NEXT:    op= ">="; break;
    case HA_READ_AFTER_KEY:      op= ">";  break;
    case HA_READ_KEY_OR_PREV:    op= "<="; break;
    case HA_READ_BEFORE_KEY:     op= "<";  break;
    default:                     op= is_max ? "<=" : ">="; break;
  }

  std::ostringstream where;
  for (uint i= 0; i < used_parts; i++)
  {
    Field *field= key_info->key_part[i].field;
    if (i > 0) where << " AND ";
    where << '"' << field->field_name.str << '"';
    where << " " << (i < used_parts - 1 ? "=" : op) << " ";
    where << field_to_sql_literal(field);
  }
  return where.str();
}

ha_rows ha_duckdb::records_in_range(uint inx, const key_range *min_key,
                                    const key_range *max_key, page_range *)
{
  DBUG_ENTER("ha_duckdb::records_in_range");

  if (!connection || (!min_key && !max_key))
    DBUG_RETURN(stats.records ? stats.records : 10);

  try
  {
    KEY *key_info= &table->key_info[inx];

    // Build WHERE clause from key ranges
    std::string conditions;
    if (min_key)
    {
      conditions= key_range_to_where(table, key_info, min_key, false);
    }
    if (max_key)
    {
      std::string max_cond= key_range_to_where(table, key_info, max_key, true);
      if (!conditions.empty() && !max_cond.empty())
        conditions += " AND " + max_cond;
      else if (!max_cond.empty())
        conditions= max_cond;
    }

    // Ask DuckDB's query planner for its row estimate via EXPLAIN FORMAT JSON.
    // The JSON plan contains "Estimated Cardinality": "NNN" at every node;
    // the first occurrence (top-level PROJECTION) is the total row estimate.
    std::string sql= "EXPLAIN (FORMAT JSON) SELECT * FROM " + duckdb_table_name;
    if (!conditions.empty())
      sql += " WHERE " + conditions;

    auto r= connection->Query(sql);
    if (r->HasError() || r->RowCount() == 0)
      DBUG_RETURN(stats.records ? stats.records : 10);

    // Column 1 (explain_value) contains the JSON text
    std::string plan= r->GetValue(1, 0).ToString();
    const std::string tag= "\"Estimated Cardinality\": \"";
    size_t pos= plan.find(tag);
    if (pos == std::string::npos)
      DBUG_RETURN(stats.records ? stats.records : 10);

    pos += tag.size();
    std::string num;
    while (pos < plan.size() && isdigit((unsigned char)plan[pos]))
      num += plan[pos++];

    if (num.empty())
      DBUG_RETURN(stats.records ? stats.records : 10);

    ha_rows estimate= (ha_rows)std::stoull(num);
    DBUG_RETURN(estimate ? estimate : 1);
  }
  catch (...)
  {
    DBUG_RETURN(stats.records ? stats.records : 10);
  }
}

int ha_duckdb::analyze(THD *, HA_CHECK_OPT *)
{
  DBUG_ENTER("ha_duckdb::analyze");
  if (!connection) DBUG_RETURN(HA_ADMIN_FAILED);
  try
  {
    // Propagate ANALYZE to DuckDB so it refreshes its internal statistics
    // for query planning on pushed-down queries (and future indexes)
    auto r= connection->Query("ANALYZE " + duckdb_table_name);
    if (r->HasError())
    {
      sql_print_error("DuckDB: ANALYZE failed for %s: %s",
                      duckdb_table_name.c_str(), r->GetErrorObject().Message().c_str());
      DBUG_RETURN(HA_ADMIN_FAILED);
    }
  }
  catch (const std::exception &e)
  {
    sql_print_error("DuckDB: ANALYZE exception: %s", e.what());
    DBUG_RETURN(HA_ADMIN_FAILED);
  }
  DBUG_RETURN(HA_ADMIN_OK);
}

// ---------------------------------------------------------------------------
// SQL rewrite pass — applied to every pushed-down query before DuckDB
// sees it.  Handles cases where a simple macro cannot work (e.g. DuckDB
// functions that require constant arguments can't be parameterised via
// a macro, so we inline the literal directly by renaming the call here).
// ---------------------------------------------------------------------------

// Replace every occurrence of the SQL function named `from_func` with
// `to_func`, skipping content inside single-quoted string literals.
// Matches whole function names only (must be preceded by a non-identifier
// character and followed by optional whitespace then '(').
static std::string rewrite_func_name(const std::string &sql,
                                     const char *from_func,
                                     const char *to_func)
{
  std::string out;
  out.reserve(sql.size());
  size_t i= 0, n= sql.size(), flen= strlen(from_func);

  while (i < n)
  {
    char c= sql[i];

    // Skip single-quoted string literals, handling '' escapes.
    if (c == '\'')
    {
      out += c; i++;
      while (i < n)
      {
        out += sql[i];
        if (sql[i++] == '\'')
        {
          if (i < n && sql[i] == '\'') { out += sql[i++]; } // escaped ''
          else break;
        }
      }
      continue;
    }

    // Skip double-quoted identifiers.
    if (c == '"')
    {
      out += c; i++;
      while (i < n && sql[i] != '"') out += sql[i++];
      if (i < n) { out += sql[i++]; }
      continue;
    }

    // Check for a whole-word match of from_func followed by '('.
    if (i + flen <= n &&
        strncasecmp(sql.c_str() + i, from_func, flen) == 0 &&
        (i == 0 || (!isalnum((unsigned char)sql[i-1]) && sql[i-1] != '_')))
    {
      size_t j= i + flen;
      while (j < n && sql[j] == ' ') j++;   // allow spaces before '('
      if (j < n && sql[j] == '(')
      {
        out += to_func;
        i += flen;
        continue;
      }
    }

    out += c; i++;
  }
  return out;
}

// Strip the leading database qualifier from three-part column references.
//
// MariaDB's select_lex->print() emits column refs as "db"."alias"."col"
// when the query uses table aliases (e.g. in joins).  DuckDB only understands
// two-part "alias"."col" — it tries to look up "db"."alias" as a real table
// and fails with "Referenced table not found".
//
// Two-part table refs in FROM clauses ("db"."table") are unchanged because
// they are exactly two quoted identifiers and are not matched by the
// three-part pattern.
static std::string strip_db_qualifier(const std::string &sql)
{
  std::string out;
  out.reserve(sql.size());
  size_t i= 0, n= sql.size();

  while (i < n)
  {
    if (sql[i] != '"') { out += sql[i++]; continue; }

    // Read first quoted identifier p1
    size_t p1s= i++;
    while (i < n && sql[i] != '"') i++;
    if (i < n) i++;                              // closing "
    size_t p1e= i;

    // Not followed by ."  — output p1 as-is
    if (i >= n || sql[i] != '.' || i + 1 >= n || sql[i + 1] != '"')
    { out += sql.substr(p1s, p1e - p1s); continue; }

    // Read second quoted identifier p2
    i++;                                         // skip '.'
    size_t p2s= i++;
    while (i < n && sql[i] != '"') i++;
    if (i < n) i++;
    size_t p2e= i;

    // Only two parts — output p1.p2 unchanged
    if (i >= n || sql[i] != '.' || i + 1 >= n || sql[i + 1] != '"')
    {
      out += sql.substr(p1s, p1e - p1s);
      out += '.';
      out += sql.substr(p2s, p2e - p2s);
      continue;
    }

    // Three parts: read p3 and drop p1 — output p2.p3 only
    i++;                                         // skip '.'
    size_t p3s= i++;
    while (i < n && sql[i] != '"') i++;
    if (i < n) i++;
    size_t p3e= i;

    out += sql.substr(p2s, p2e - p2s);
    out += '.';
    out += sql.substr(p3s, p3e - p3s);
  }
  return out;
}

// Apply all MariaDB→DuckDB function rewrites in one pass.
// For a specific injected table name, strip any "schema"."tname" occurrence
// in the SQL down to just "tname".  Injected temp tables are unqualified in
// DuckDB's temp schema, so the schema prefix must be removed.
// Column references like "tname"."col" are NOT affected because the lookup
// pattern is "something"."tname", not "tname"."something".
static void strip_schema_for_table(std::string &sql, const std::string &tname)
{
  const std::string quoted = '"' + tname + '"';
  size_t pos = 0;
  while ((pos = sql.find(quoted, pos)) != std::string::npos)
  {
    // Check if preceded by ."  (closing quote of a schema name + dot)
    if (pos >= 2 && sql[pos - 1] == '.' && sql[pos - 2] == '"')
    {
      size_t schema_open = sql.rfind('"', pos - 3);
      if (schema_open != std::string::npos)
      {
        sql.erase(schema_open, pos - schema_open);  // remove "schema".
        pos = schema_open;
        continue;
      }
    }
    pos += quoted.size();
  }
}

// MariaDB's printer moves INNER JOIN ON conditions into the WHERE clause,
// producing "JOIN table WHERE ..." which DuckDB rejects (it requires ON or USING).
// Find each bare JOIN (not followed by ON/USING after the table ref) and insert
// "ON TRUE" so DuckDB treats it as a cross join filtered by the WHERE clause —
// semantically identical to the original INNER JOIN.
static std::string fix_bare_joins(const std::string &sql)
{
  // Scan for JOIN keywords where the table reference is not followed by ON or
  // USING — MariaDB's derived/CTE printer moves INNER JOIN conditions to WHERE,
  // producing "JOIN table WHERE ..." which DuckDB rejects.  We insert ON TRUE
  // to satisfy DuckDB's parser while preserving the WHERE semantics.
  //
  // Uses a simple character scanner instead of regex to avoid backtracking
  // ambiguity with optional table aliases.  Skips over quoted identifiers and
  // string literals so embedded SQL keywords don't cause false matches.
  //
  // CROSS JOIN and NATURAL JOIN never have ON/USING and are left untouched.

  const size_t n = sql.size();
  std::string out;
  out.reserve(n + 64);
  size_t i = 0;

  // Skip a double-quoted identifier (handles "" escaping).
  auto skip_quoted_id = [&](size_t pos) -> size_t {
    if (pos >= n || sql[pos] != '"') return pos;
    for (++pos; pos < n; ++pos) {
      if (sql[pos] != '"') continue;
      if (++pos < n && sql[pos] == '"') continue;  // "" escape
      break;
    }
    return pos;
  };

  // Case-insensitive keyword check: sql[pos..] == kw (kw must be lowercase),
  // followed by a non-identifier character or end of string.
  auto is_kw = [&](size_t pos, const char *kw) -> bool {
    for (size_t k = 0; kw[k]; ++k, ++pos) {
      if (pos >= n || tolower((unsigned char)sql[pos]) != (unsigned char)kw[k])
        return false;
    }
    return pos >= n || !(isalnum((unsigned char)sql[pos]) || sql[pos] == '_');
  };

  // Return the last non-whitespace keyword in out (lowercase), used to detect
  // CROSS or NATURAL preceding the JOIN keyword.
  auto last_kw_in_out = [&]() -> std::string {
    size_t j = out.size();
    while (j > 0 && isspace((unsigned char)out[j - 1])) --j;
    size_t end = j;
    while (j > 0 && isalpha((unsigned char)out[j - 1])) --j;
    if (j == end) return "";
    std::string kw = out.substr(j, end - j);
    for (char &c : kw) c = (char)tolower((unsigned char)c);
    return kw;
  };

  while (i < n) {
    // Pass through single-quoted string literals without scanning inside them.
    if (sql[i] == '\'') {
      out += sql[i++];
      while (i < n) {
        char c = sql[i++];
        out += c;
        if (c == '\'') {
          if (i < n && sql[i] == '\'') out += sql[i++];  // '' escape
          else break;
        }
      }
      continue;
    }

    // Pass through double-quoted identifiers without scanning inside them.
    if (sql[i] == '"') {
      size_t end = skip_quoted_id(i);
      out.append(sql, i, end - i);
      i = end;
      continue;
    }

    // Not a JOIN keyword — emit character and continue.
    if (!is_kw(i, "join")) {
      out += sql[i++];
      continue;
    }

    // ── JOIN keyword found ───────────────────────────────────────────────────

    // CROSS JOIN and NATURAL JOIN never take ON/USING — skip them.
    std::string prev = last_kw_in_out();
    if (prev == "cross" || prev == "natural") {
      out += sql[i++];
      continue;
    }

    // 1. Emit "JOIN" (preserve original case, always 4 chars).
    out.append(sql, i, 4);
    i += 4;

    // 2. Emit whitespace after JOIN.
    while (i < n && isspace((unsigned char)sql[i]))
      out += sql[i++];

    // Helper: skip an unquoted SQL identifier ([A-Za-z_][A-Za-z0-9_]*).
    auto skip_unquoted_id = [&](size_t pos) -> size_t {
      while (pos < n && (isalnum((unsigned char)sql[pos]) || sql[pos] == '_'))
        ++pos;
      return pos;
    };

    // 3. Emit table name: optional "schema". prefix + "table".
    //    Handles both double-quoted identifiers (from AST-printer path after
    //    backtick→double-quote conversion) and unquoted names (original SQL).
    if (i < n && sql[i] == '"') {
      size_t end = skip_quoted_id(i);
      out.append(sql, i, end - i);
      i = end;
      if (i < n && sql[i] == '.') {
        out += sql[i++];
        end = skip_quoted_id(i);
        out.append(sql, i, end - i);
        i = end;
      }
    } else if (i < n && (isalpha((unsigned char)sql[i]) || sql[i] == '_')) {
      size_t end = skip_unquoted_id(i);
      out.append(sql, i, end - i);
      i = end;
      if (i < n && sql[i] == '.') {
        out += sql[i++];
        end = skip_unquoted_id(i);
        out.append(sql, i, end - i);
        i = end;
      }
    }

    // 4. Emit optional alias: a quoted or unquoted identifier not immediately
    //    followed by '.' (which would indicate schema.table, not an alias).
    {
      size_t j = i;
      while (j < n && isspace((unsigned char)sql[j])) ++j;
      if (j < n && sql[j] == '"') {
        size_t alias_end = skip_quoted_id(j);
        if (alias_end >= n || sql[alias_end] != '.') {
          out.append(sql, i, alias_end - i);  // whitespace + alias
          i = alias_end;
        }
      } else if (j < n && (isalpha((unsigned char)sql[j]) || sql[j] == '_')) {
        size_t alias_end = skip_unquoted_id(j);
        // Only treat it as an alias if it's not a SQL keyword (ON, USING,
        // WHERE, etc.) — those belong to step 6 or the outer query.
        std::string tok = sql.substr(j, alias_end - j);
        for (char &c : tok) c = (char)tolower((unsigned char)c);
        static const char * const kw_not_alias[] = {
          "on", "using", "where", "group", "order", "having",
          "limit", "union", "inner", "outer", "left", "right",
          "cross", "natural", "join", "set", nullptr
        };
        bool is_keyword = false;
        for (int ki = 0; kw_not_alias[ki]; ++ki)
          if (tok == kw_not_alias[ki]) { is_keyword = true; break; }
        if (!is_keyword && (alias_end >= n || sql[alias_end] != '.')) {
          out.append(sql, i, alias_end - i);  // whitespace + alias
          i = alias_end;
        }
      }
    }

    // 5. Emit whitespace before the next keyword.
    {
      size_t ws = i;
      while (i < n && isspace((unsigned char)sql[i])) ++i;
      out.append(sql, ws, i - ws);
    }

    // 6. If the next token is not ON or USING, insert ON TRUE.
    if (!is_kw(i, "on") && !is_kw(i, "using"))
      out += "ON TRUE ";

    // Continue — the next token (ON/USING/WHERE/etc.) is emitted by the
    // normal path on the next iteration.
  }

  return out;
}

static std::string rewrite_mariadb_sql(const std::string &sql)
{
  std::string s= sql;

  s= strip_db_qualifier(s);

  // Strip all MariaDB internal optimizer artifact tokens.
  //
  // MariaDB's query printer (Item::print / SELECT_LEX::print) emits <tag>
  // tokens for internal optimizer nodes that DuckDB's parser rejects.
  // We handle two categories:
  //
  //  A. Wrapper nodes — <tag>(inner): strip the tag, keep the inner expr.
  //     Includes: <index_lookup>(, <ref_null_helper>(, <is_not_null_test>(,
  //               <not>(
  //     Note: <not>(expr) becomes NOT (expr); the others just drop the wrapper.
  //
  //  B. Bare tokens with no meaningful content for DuckDB:
  //     <nop>, <list ref>, <no matter>, <result>, <rowid>, <no_table_name>,
  //     <temporary table>, <<DISABLED>>
  //     These are stripped entirely (replaced with empty string).
  //
  // The scanner also handles the `, <primary_index_lookup>(...)` and
  // `<materialize>` cases below, which require special treatment.
  {
    // --- A. Wrapper strippers ---
    struct WrapperTag {
      const char *tag;
      const char *replacement_open;  // nullptr → just strip tag, keep '('
    };
    static const WrapperTag wrappers[] = {
      { "<index_lookup>(",    nullptr         },  // strip tag, keep '('
      { "<ref_null_helper>(", nullptr         },
      { "<is_not_null_test>(", nullptr        },
      { "<not>(",             "NOT ("         },  // logical NOT
    };

    for (const auto &w : wrappers)
    {
      std::string tag(w.tag);
      std::string out;
      out.reserve(s.size());
      size_t i= 0, n= s.size();
      // case-insensitive compare helper
      auto ci_cmp= [&](size_t pos, const std::string &t) {
        if (pos + t.size() > n) return false;
        for (size_t k= 0; k < t.size(); k++)
          if (tolower((unsigned char)s[pos+k]) != tolower((unsigned char)t[k]))
            return false;
        return true;
      };
      while (i < n)
      {
        if (ci_cmp(i, tag))
        {
          if (w.replacement_open)
            out += w.replacement_open;
          else
            out += '(';
          i += tag.size();  // tag already includes the '('
        }
        else
          out += s[i++];
      }
      s= out;
    }

    // --- B. Bare token strippers ---
    static const char *bare[] = {
      "<nop>", "<list ref>", "<no matter>", "<result>", "<rowid>",
      "<no_table_name>", "<temporary table>", "<<DISABLED>>",
    };
    for (const char *tok : bare)
    {
      std::string t(tok);
      std::string out;
      out.reserve(s.size());
      size_t i= 0, n= s.size();
      while (i < n)
      {
        if (s.compare(i, t.size(), t) == 0)
          i += t.size();
        else
          out += s[i++];
      }
      s= out;
    }
  }

  // Strip MariaDB's <expr_cache><key>(expr) wrapper.
  // MariaDB's optimizer emits <expr_cache><col_ref>(expr) for cached subquery
  // lookups (e.g. NOT IN rewritten via materialisation).  DuckDB rejects the
  // <...> syntax entirely.  Strip the wrapper and emit just the inner expr.
  {
    std::string out;
    out.reserve(s.size());
    size_t i= 0, n= s.size();
    const std::string tag= "<expr_cache>";
    while (i < n)
    {
      if (s.compare(i, tag.size(), tag) == 0)
      {
        i += tag.size();
        // Skip the <key> part: scan until matching '>'
        if (i < n && s[i] == '<')
        {
          int depth= 1;
          i++;
          while (i < n && depth > 0)
          {
            if      (s[i] == '<') depth++;
            else if (s[i] == '>') depth--;
            i++;
          }
        }
        // Now strip the outer '(' and emit inner expr up to matching ')'
        if (i < n && s[i] == '(')
        {
          i++;  // skip '('
          int depth= 1;
          while (i < n && depth > 0)
          {
            if      (s[i] == '(') { depth++; out += s[i]; }
            else if (s[i] == ')') { depth--; if (depth > 0) out += s[i]; }
            else                    out += s[i];
            i++;
          }
        }
      }
      else
        out += s[i++];
    }
    s= out;
  }

  // Strip MariaDB's <cache>(...) wrapper around constant expressions.
  // MariaDB's AST printer emits e.g. <cache>('2022-01-01') for literals
  // that have been evaluated and cached.  DuckDB's parser rejects this
  // syntax — replace <cache>(expr) with just expr.
  {
    std::string out;
    out.reserve(s.size());
    size_t i= 0, n= s.size();
    const std::string tag= "<cache>(";
    while (i < n)
    {
      if (s.compare(i, tag.size(), tag) == 0)
      {
        i += tag.size();
        int depth= 1;
        while (i < n && depth > 0)
        {
          if      (s[i] == '(') depth++;
          else if (s[i] == ')') depth--;
          if (depth > 0) out += s[i];
          i++;
        }
      }
      else
        out += s[i++];
    }
    s= out;
  }

  // Strip MariaDB's <materialize> prefix from subquery wrappers.
  // MariaDB's optimizer emits col IN (<materialize>(SELECT ...)) when it
  // decides to materialise a subquery.  DuckDB rejects the <...> tag; strip
  // it, leaving col IN ((SELECT ...)) which DuckDB handles correctly.
  {
    static const std::regex materialize_re("<materialize>\\s*",
                                           std::regex::icase);
    s= std::regex_replace(s, materialize_re, "");
  }

  // Strip , <primary_index_lookup>(...) from IN lists.
  // When MariaDB sees a primary key on a DuckDB table it may plan a NOT EXISTS
  // as: col IN ((real_subquery), <primary_index_lookup>(col IN <temporary table>))
  // The <primary_index_lookup> element is a redundant execution-path artifact
  // for the same subquery.  Drop the comma + node; the real subquery remains.
  {
    std::string out;
    out.reserve(s.size());
    size_t i= 0, n= s.size();
    const std::string tag= "<primary_index_lookup>";
    while (i < n)
    {
      // Match optional preceding comma+whitespace then the tag
      if (s[i] == ',')
      {
        size_t j= i + 1;
        while (j < n && s[j] == ' ') j++;
        if (s.compare(j, tag.size(), tag) == 0)
        {
          // Skip tag
          j += tag.size();
          // Skip parenthesised content
          if (j < n && s[j] == '(')
          {
            j++;
            int depth= 1;
            while (j < n && depth > 0)
            {
              if      (s[j] == '(') depth++;
              else if (s[j] == ')') depth--;
              j++;
            }
          }
          i= j;
          continue;
        }
      }
      out += s[i++];
    }
    s= out;
  }

  // Strip MariaDB's <in_optimizer>(val, expr) wrapper.
  // MariaDB's optimizer wraps IN/EXISTS subqueries in an internal Item_in_optimizer
  // node whose printed form is <in_optimizer>(val, expr).  DuckDB does not
  // recognise this syntax.  We strip the wrapper and emit just the inner expr.
  {
    std::string out;
    out.reserve(s.size());
    size_t i= 0, n= s.size();
    const std::string tag= "<in_optimizer>(";
    while (i < n)
    {
      if (s.compare(i, tag.size(), tag) == 0)
      {
        i += tag.size();
        // Skip the first argument (everything up to the first ',' at depth 1).
        int depth= 1;
        while (i < n && depth > 0)
        {
          if      (s[i] == '(') depth++;
          else if (s[i] == ')') { depth--; if (depth == 0) break; }
          else if (s[i] == ',' && depth == 1) { i++; break; }
          i++;
        }
        // Emit the second argument (everything up to the final closing ')').
        depth= 1;
        while (i < n && depth > 0)
        {
          if      (s[i] == '(') { depth++; out += s[i]; }
          else if (s[i] == ')') { depth--; if (depth > 0) out += s[i]; }
          else                    out += s[i];
          i++;
        }
      }
      else
        out += s[i++];
    }
    s= out;
  }

  // !(<exists>(expr)) → NOT EXISTS (expr)
  // <exists>(expr)   → EXISTS (expr)
  // MariaDB's printer emits NOT IN / EXISTS subqueries using internal tags:
  //   NOT IN  → !(<exists>(select ...))
  //   EXISTS  → <in_optimizer>(1, exists(select ...))   [handled above]
  // DuckDB rejects both the ! prefix and the <exists> tag.  We rewrite:
  //   !(<exists>(expr)) → NOT EXISTS (expr)
  //   <exists>(expr)    → EXISTS (expr)       [bare <exists> after above pass]
  {
    // Helper: extract parenthesised content starting at s[i] (i points just
    // past the opening '('), emitting it verbatim, return pos after closing ')'.
    auto extract_parens = [&](std::string &out, size_t i, size_t n,
                               const std::string &sql) -> size_t {
      int depth= 1;
      while (i < n && depth > 0)
      {
        if      (sql[i] == '(') { depth++; out += sql[i]; }
        else if (sql[i] == ')') { depth--; if (depth > 0) out += sql[i]; }
        else                      out += sql[i];
        i++;
      }
      return i;
    };

    std::string out;
    out.reserve(s.size());
    size_t i= 0, n= s.size();
    const std::string not_exists_tag= "!(<exists>(";
    const std::string exists_tag    = "<exists>(";
    while (i < n)
    {
      if (s.compare(i, not_exists_tag.size(), not_exists_tag) == 0)
      {
        out += "NOT EXISTS (";
        i += not_exists_tag.size();
        i = extract_parens(out, i, n, s);  // inner expr (no outer paren yet)
        out += ')';
        i++;                               // skip final ')' of !(<exists>(...))
      }
      else if (s.compare(i, exists_tag.size(), exists_tag) == 0)
      {
        out += "EXISTS (";
        i += exists_tag.size();
        i = extract_parens(out, i, n, s);
        out += ')';
      }
      // !exists(expr) → NOT EXISTS (expr)
      // NOT EXISTS emitted without the <exists> wrapper (bare exists function).
      else if (i + 7 < n && s[i] == '!' &&
               s.compare(i+1, 7, "exists(") == 0)
      {
        out += "NOT EXISTS (";
        i += 8;  // skip '!' + 'exists('
        i = extract_parens(out, i, n, s);
        out += ')';
      }
      // !(expr) → NOT (expr)
      // MariaDB uses C-style ! for logical NOT; DuckDB requires NOT.
      // Only rewrite !( — leave != alone.
      else if (s[i] == '!' && i + 1 < n && s[i+1] == '(')
      {
        out += "NOT ";
        i++;  // skip '!', keep '('
      }
      else
        out += s[i++];
    }
    s= out;
  }

  // str_to_date(str, fmt) → strptime(str, fmt)
  // strptime() requires a constant format — can't use a macro parameter.
  s= rewrite_func_name(s, "str_to_date", "strptime");

  // char(n) → chr(n)
  // 'char' is a reserved type keyword in DuckDB; a macro named 'char'
  // fails to install.  Single-arg rename only — multi-arg CHAR() which
  // concatenates code points is rare and not handled.
  s= rewrite_func_name(s, "char", "chr");

  // INTERVAL 'N' unit → INTERVAL 'N units'
  // MariaDB's query printer emits INTERVAL 'N' UNIT (quoted integer, separate
  // unit keyword).  DuckDB requires the number and unit in a single string:
  // INTERVAL '90 days'.  The bare-integer form INTERVAL N UNIT is also
  // rejected by DuckDB.  Normalise to the single-string form with plural unit.
  // Matches: interval '<digits>' <unit_keyword>[s]
  {
    static const std::regex interval_quoted_re(
      "\\binterval\\s+'(\\d+)'\\s+(year|month|week|day|hour|minute|second)s?\\b",
      std::regex::icase);
    // Replace with "interval 'N units'" — DuckDB's native interval string form.
    std::string result;
    result.reserve(s.size());
    auto it  = std::sregex_iterator(s.begin(), s.end(), interval_quoted_re);
    auto end = std::sregex_iterator();
    size_t last= 0;
    for (; it != end; ++it)
    {
      const std::smatch &m= *it;
      result.append(s, last, m.position() - last);
      result += "interval '";
      result += m[1].str();               // digits
      result += ' ';
      std::string unit= m[2].str();
      // Lowercase and pluralise.
      for (char &c : unit) c= tolower((unsigned char)c);
      result += unit;
      if (unit.back() != 's') result += 's';
      result += '\'';
      last= m.position() + m.length();
    }
    result.append(s, last, s.size() - last);
    s= std::move(result);
  }

  // DATE'YYYY-MM-DD' → DATE 'YYYY-MM-DD'
  // MariaDB's query printer emits ANSI date literals without a space between
  // the DATE keyword and the string (e.g. DATE'1998-12-01').  DuckDB requires
  // a space: DATE '1998-12-01'.  Without the space DuckDB misparses it and
  // the subsequent arithmetic produces an incorrect type.
  {
    static const std::regex date_nospace_re(
      "\\bDATE'(\\d{4}-\\d{2}-\\d{2})'",
      std::regex::icase);
    s= std::regex_replace(s, date_nospace_re, "DATE '$1'");
  }

  return s;
}

// Forward declarations for injection helpers defined later in this file.
static std::string extract_missing_table(const std::string &errmsg);
static TABLE      *find_open_table_by_name(THD *thd, const std::string &name);
static With_element *find_cte_by_name(THD *thd, const std::string &name);
static bool inject_table_into_duckdb(duckdb::Connection *conn, TABLE *table,
                                     const std::string &temp_name,
                                     const std::vector<Item *> &push_conds= {});
static bool inject_cte_into_duckdb(duckdb::Connection *conn, THD *thd,
                                   With_element *cte, const std::string &temp_name);

// ---------------------------------------------------------------------------
// Predicate pushdown helper for InnoDB injection
//
// Collects predicates from a WHERE tree that reference ONLY the given table
// (i.e., used_tables() ⊆ tbl_map, where tbl_map is the TABLE::map bit for
// the table being injected).  These predicates are evaluated row-by-row
// inside inject_table_into_duckdb so non-matching rows are never appended
// to the DuckDB TEMP TABLE.
//
// Walks AND conjuncts recursively; non-AND nodes that also reference other
// tables are skipped (cannot be pushed to a single-table scan).
// ---------------------------------------------------------------------------
static void collect_single_table_conds(Item *cond, table_map tbl_map,
                                        std::vector<Item *> &out)
{
  if (!cond) return;
  // If the entire subtree only touches this table (and constants), take it.
  // Exclude predicates that contain subqueries: calling val_int() on an
  // Item_in_subselect during a ha_rnd_next loop crashes because the subquery
  // needs its own execution context (Item_func::fix_fields → Eq_creator::create
  // → SIGSEGV, confirmed via GDB backtrace on Q20).
  if ((cond->used_tables() & ~tbl_map) == 0 && !cond->with_subquery())
  {
    out.push_back(cond);
    return;
  }
  // If it's a top-level AND, recurse into each argument looking for
  // conjuncts that are single-table even if the overall AND is not.
  if (cond->type() == Item::COND_ITEM)
  {
    Item_cond *ic= static_cast<Item_cond *>(cond);
    if (ic->functype() == Item_func::COND_AND_FUNC)
    {
      List_iterator<Item> li(*ic->argument_list());
      Item *child;
      while ((child= li++))
        collect_single_table_conds(child, tbl_map, out);
    }
  }
}

// ---------------------------------------------------------------------------
// Select handler — full query pushdown
// ---------------------------------------------------------------------------

static select_handler *create_duckdb_select_handler(THD *thd, SELECT_LEX *sel,
                                                     SELECT_LEX_UNIT *unit)
{
  if (!sel) return nullptr;

  // Require at least one DuckDB leaf (to identify the database and connection).
  // Non-DuckDB leaves (InnoDB or CTE-backed) are injectable at runtime by
  // init_scan, matching the same policy as create_duckdb_derived_handler.
  TABLE_LIST *duckdb_leaf= nullptr;
  bool all_duckdb= true;
  {
    List_iterator<TABLE_LIST> it(sel->leaf_tables);
    TABLE_LIST *tl;
    while ((tl= it++))
    {
      if (!tl->table)
        continue;
      if (tl->table->file->ht == duckdb_hton)
      {
        if (!duckdb_leaf) duckdb_leaf= tl;
      }
      else if (!tl->with)
      {
        // CTE-backed leaves (tl->with != nullptr) don't count against
        // all_duckdb: thd->query() includes the full WITH clause so DuckDB
        // can resolve the CTE itself.  Only real non-DuckDB storage (InnoDB
        // etc.) requires the injection / AST-printer path.
        all_duckdb= false;
      }
    }
  }
  // If the top-level SELECT has no DuckDB leaf, check subquery tables via
  // thd->lex->query_tables (covers all levels of the query tree).  This
  // handles queries like Q20 where the outer FROM is all-InnoDB but subqueries
  // reference DuckDB tables — without this, the select handler is never
  // invoked and MariaDB falls back to a catastrophically slow nested-loop.
  if (!duckdb_leaf)
  {
    for (TABLE_LIST *tl= thd->lex->query_tables; tl; tl= tl->next_global)
    {
      if (tl->table && tl->table->file->ht == duckdb_hton)
      {
        duckdb_leaf= tl;
        all_duckdb= false;
        break;
      }
    }
  }
  if (!duckdb_leaf) return nullptr;

  ha_duckdb *h= static_cast<ha_duckdb*>(duckdb_leaf->table->file);
  if (h->db_file_path.empty())
    return nullptr;

  // Reuse the per-THD persistent connection if one already exists for this
  // database file.  This allows TEMP TABLEs injected in a previous query to
  // survive into the next query in the same session (see g_injected_cache).
  {
    mysql_mutex_lock(&g_duckdb_mutex);
    auto it= g_thd_conns.find(thd);
    if (it != g_thd_conns.end() && it->second.db_file_path == h->db_file_path)
    {
      duckdb::Connection *conn= it->second.conn;
      mysql_mutex_unlock(&g_duckdb_mutex);
      return new ha_duckdb_select_handler(thd, sel, conn, all_duckdb);
    }
    mysql_mutex_unlock(&g_duckdb_mutex);
  }

  duckdb::DuckDB *db= registry_get(h->db_file_path);
  if (!db) return nullptr;

  duckdb::Connection *conn= nullptr;
  try { conn= new duckdb::Connection(*db); }
  catch (...) { registry_release(h->db_file_path); return nullptr; }

  // Store in per-THD map; registry_release deferred to duckdb_close_connection.
  {
    mysql_mutex_lock(&g_duckdb_mutex);
    g_thd_conns[thd]= {conn, h->db_file_path};
    mysql_mutex_unlock(&g_duckdb_mutex);
  }
  return new ha_duckdb_select_handler(thd, sel, conn, all_duckdb);
}

ha_duckdb_select_handler::ha_duckdb_select_handler(THD *thd, SELECT_LEX *sel,
                                                   duckdb::Connection *conn,
                                                   bool use_orig)
  : select_handler(thd, duckdb_hton, sel),
    connection(conn), result(nullptr), current_row(0),
    use_original_sql(use_orig)
{}

ha_duckdb_select_handler::ha_duckdb_select_handler(THD *thd,
                                                   SELECT_LEX_UNIT *unit,
                                                   duckdb::Connection *conn,
                                                   bool use_orig)
  : select_handler(thd, duckdb_hton, unit),
    connection(conn), result(nullptr), current_row(0),
    use_original_sql(use_orig)
{}

ha_duckdb_select_handler::~ha_duckdb_select_handler()
{
  delete result;
  // connection is owned by g_thd_conns (per-THD persistent connection).
  // It is deleted in duckdb_close_connection when the session ends.
  // Exception: the unit handler (UNION path) still creates its own private
  // connection that is not stored in g_thd_conns — detect this by checking
  // whether the connection is registered.
  bool owned_by_thd= false;
  {
    mysql_mutex_lock(&g_duckdb_mutex);
    auto it= g_thd_conns.find(thd);
    owned_by_thd= (it != g_thd_conns.end() && it->second.conn == connection);
    mysql_mutex_unlock(&g_duckdb_mutex);
  }
  if (!owned_by_thd)
    delete connection;
}

// ---------------------------------------------------------------------------
// Minimal rewrite pass for original (pre-optimizer) SQL.
// Applies only the genuine semantic transforms needed for DuckDB compatibility:
// INTERVAL syntax, DATE literal spacing, str_to_date→strptime, char→chr.
// Does NOT apply MariaDB AST artifact rewrites — those never appear in
// user-submitted SQL and are only emitted by MariaDB's internal query printer.
// ---------------------------------------------------------------------------
static std::string rewrite_original_sql(const std::string &sql)
{
  std::string s= sql;

  // str_to_date(str, fmt) → strptime(str, fmt)
  s= rewrite_func_name(s, "str_to_date", "strptime");

  // char(n) → chr(n)
  s= rewrite_func_name(s, "char", "chr");

  // INTERVAL 'N' unit → INTERVAL 'N units'
  {
    static const std::regex interval_quoted_re(
      "\\binterval\\s+'(\\d+)'\\s+(year|month|week|day|hour|minute|second)s?\\b",
      std::regex::icase);
    std::string result;
    result.reserve(s.size());
    auto it  = std::sregex_iterator(s.begin(), s.end(), interval_quoted_re);
    auto end = std::sregex_iterator();
    size_t last= 0;
    for (; it != end; ++it)
    {
      const std::smatch &m= *it;
      result.append(s, last, m.position() - last);
      result += "interval '";
      result += m[1].str();
      result += ' ';
      std::string unit= m[2].str();
      for (char &c : unit) c= tolower((unsigned char)c);
      result += unit;
      if (unit.back() != 's') result += 's';
      result += '\'';
      last= m.position() + m.length();
    }
    result.append(s, last, s.size() - last);
    s= std::move(result);
  }

  // DATE'YYYY-MM-DD' → DATE 'YYYY-MM-DD'
  {
    static const std::regex date_nospace_re(
      "\\bDATE'(\\d{4}-\\d{2}-\\d{2})'",
      std::regex::icase);
    s= std::regex_replace(s, date_nospace_re, "DATE '$1'");
  }

  return s;
}

int ha_duckdb_select_handler::init_scan()
{
  std::string sql;

  if (use_original_sql)
  {
    // All tables are DuckDB — use the original unoptimized SQL string from the
    // client.  This bypasses MariaDB's AST printer and all its optimizer
    // artifacts (<in_optimizer>, <expr_cache>, <exists>, etc.).
    const char *qstr= thd->query();
    size_t qlen= thd->query_length();
    sql.assign(qstr ? qstr : "", qlen);

    // CTAS: thd->query() is the full "CREATE TABLE ... AS SELECT ..." DDL.
    // Strip everything up to and including the AS keyword so DuckDB only
    // receives the SELECT (or WITH) portion.
    {
      // Scan for " AS " followed by SELECT or WITH (case-insensitive).
      size_t n= sql.size();
      for (size_t i= 0; i + 4 < n; i++)
      {
        if (tolower((unsigned char)sql[i])   == 'a' &&
            tolower((unsigned char)sql[i+1]) == 's' &&
            isspace((unsigned char)sql[i+2]))
        {
          size_t j= i + 3;
          while (j < n && isspace((unsigned char)sql[j])) j++;
          // Check the next keyword is SELECT or WITH
          auto starts_with_kw= [&](const char *kw, size_t klen) {
            if (j + klen > n) return false;
            for (size_t k= 0; k < klen; k++)
              if (tolower((unsigned char)sql[j+k]) != kw[k]) return false;
            // Must be followed by whitespace or end
            return (j + klen >= n || !isalnum((unsigned char)sql[j+klen]));
          };
          if (starts_with_kw("select", 6) || starts_with_kw("with", 4))
          {
            sql= sql.substr(j);
            break;
          }
        }
      }
    }

    sql= rewrite_original_sql(sql);

    // Point DuckDB at the correct schema for unqualified table names.
    if (thd->db.str && thd->db.length)
    {
      std::string use_stmt= "SET search_path = '";
      use_stmt += std::string(thd->db.str, thd->db.length);
      use_stmt += "'";
      connection->Query(use_stmt);
    }
  }
  else
  {
    // Mixed-engine path: InnoDB tables will be injected into DuckDB as TEMP
    // TABLEs so the full query executes inside DuckDB.
    //
    // For UNION/INTERSECT/EXCEPT (lex_unit) we must use the AST printer because
    // the original SQL string is the raw client text which may not reconstruct
    // cleanly across set-operation boundaries.
    //
    // For plain SELECT (select_lex) we prefer the original SQL string — the
    // same approach as Path A (use_original_sql).  The AST printer converts
    // NOT IN → NOT EXISTS and inlines other optimizer artifacts, which can drop
    // correlation predicates and produce wrong results.  The original string
    // preserves the user's intent; rewrite_original_sql handles the few
    // MariaDB→DuckDB syntax differences (INTERVAL, DATE literals, etc.).
    if (lex_unit)
    {
      String query_str;
      lex_unit->print(&query_str, QT_ORDINARY);
      sql.assign(query_str.ptr(), query_str.length());
      for (char &c : sql)
        if (c == '`') c = '"';
      sql= rewrite_mariadb_sql(sql);
    }
    else
    {
      // Use original client SQL — identical to Path A's SQL retrieval.
      const char *qstr= thd->query();
      size_t      qlen= thd->query_length();
      sql.assign(qstr ? qstr : "", qlen);

      // CTAS: strip "CREATE TABLE ... AS" prefix, keep only the SELECT.
      {
        size_t n= sql.size();
        for (size_t i= 0; i + 4 < n; i++)
        {
          if (tolower((unsigned char)sql[i])   == 'a' &&
              tolower((unsigned char)sql[i+1]) == 's' &&
              isspace((unsigned char)sql[i+2]))
          {
            size_t j= i + 3;
            while (j < n && isspace((unsigned char)sql[j])) j++;
            auto starts_with_kw= [&](const char *kw, size_t klen) {
              if (j + klen > n) return false;
              for (size_t k= 0; k < klen; k++)
                if (tolower((unsigned char)sql[j+k]) != kw[k]) return false;
              return (j + klen >= n || !isalnum((unsigned char)sql[j+klen]));
            };
            if (starts_with_kw("select", 6) || starts_with_kw("with", 4))
            {
              sql= sql.substr(j);
              break;
            }
          }
        }
      }

      for (char &c : sql)
        if (c == '`') c = '"';

      sql= rewrite_original_sql(sql);

      // Point DuckDB at the current schema for unqualified table names.
      if (thd->db.str && thd->db.length)
      {
        std::string use_stmt= "SET search_path = '";
        use_stmt += std::string(thd->db.str, thd->db.length);
        use_stmt += "'";
        connection->Query(use_stmt);
      }
    }

    // Inject all non-DuckDB leaf tables (InnoDB, CTE-backed) into DuckDB as
    // TEMP TABLEs so the full query — including ORDER BY, GROUP BY, HAVING —
    // executes entirely inside DuckDB without any MariaDB-side row operations.
    {
      SELECT_LEX *sel= select_lex;
      if (sel)
      {
        List_iterator<TABLE_LIST> it(sel->leaf_tables);
        TABLE_LIST *tl;
        while ((tl= it++))
        {
          if (!tl->table || tl->table->file->ht == duckdb_hton)
            continue;
          // Derived tables (subqueries in FROM) cannot be row-scanned here;
          // DuckDB resolves them from the original SQL, and the retry loop
          // below injects any InnoDB tables they reference.
          if (tl->derived)
            continue;

          std::string tname(tl->table->s->table_name.str);
          bool ok= false;
          if (tl->with)
            ok= inject_cte_into_duckdb(connection, thd, tl->with, tname);
          else
          {
            std::vector<Item *> push_conds;
            if (sel && sel->where)
              collect_single_table_conds(sel->where, tl->table->map, push_conds);
            ok= inject_table_into_duckdb(connection, tl->table, tname, push_conds);
          }

          if (ok)
            strip_schema_for_table(sql, tname);
          else
            sql_print_warning("DuckDB select: pre-inject '%s' failed; retry loop will handle it",
                              tname.c_str());
        }
      }
    }

    // Fix bare JOINs: only needed for the AST-printer (lex_unit) path, but
    // harmless to run on the original-SQL path too.
    sql= fix_bare_joins(sql);
  }

  // Retry loop: handles tables not visible in leaf_tables (e.g. subquery aliases).
  for (int attempt= 0; attempt < 8; attempt++)
  {
    std::unique_ptr<duckdb::MaterializedQueryResult> r;
    try
    {
      register_active_connection(thd, connection);
      r= connection->Query(sql);
      unregister_active_connection(thd);
    }
    catch (const std::exception &e)
    {
      unregister_active_connection(thd);
      sql_print_error("DuckDB select pushdown exception: %s", e.what());
      return 1;
    }

    if (!r->HasError())
    {
      result= r.release();
      current_row= 0;
      return 0;
    }

    std::string errmsg= r->GetErrorObject().Message();
    std::string missing= extract_missing_table(errmsg);

    if (missing.empty())
    {
      sql_print_error("DuckDB select pushdown failed: %s\nSQL: %s",
                      errmsg.c_str(), sql.c_str());
      return 1;
    }

    TABLE *open_tbl= find_open_table_by_name(thd, missing);
    if (open_tbl)
    {
      if (!inject_table_into_duckdb(connection, open_tbl, missing))
      {
        sql_print_error("DuckDB select pushdown: failed to inject table '%s'", missing.c_str());
        return 1;
      }
      strip_schema_for_table(sql, missing);
      continue;
    }

    With_element *cte= find_cte_by_name(thd, missing);
    if (cte)
    {
      if (!inject_cte_into_duckdb(connection, thd, cte, missing))
      {
        sql_print_error("DuckDB select pushdown: failed to inject CTE '%s'", missing.c_str());
        return 1;
      }
      strip_schema_for_table(sql, missing);
      continue;
    }

    sql_print_error("DuckDB select pushdown: unresolvable table '%s'\nFull error: %s\nSQL: %s",
                    missing.c_str(), errmsg.c_str(), sql.c_str());
    return 1;
  }

  sql_print_error("DuckDB select pushdown: exceeded retry limit\nSQL: %s", sql.c_str());
  return 1;
}

int ha_duckdb_select_handler::next_row()
{
  if (!result || current_row >= result->RowCount())
    return HA_ERR_END_OF_FILE;

  // Fill table->record[0] — one field per SELECT output column
  memset(table->record[0], 0, table->s->null_bytes);

  for (uint i= 0; i < table->s->fields && i < result->ColumnCount(); i++)
  {
    Field *field= table->field[i];
    duckdb::Value val= result->GetValue(i, current_row);

    if (val.IsNull()) { field->set_null(); continue; }
    field->set_notnull();

    // Use string conversion for all types — safe for aggregates/expressions
    std::string s= val.ToString();
    field->store(s.c_str(), s.length(), system_charset_info);
  }

  current_row++;
  return 0;
}

int ha_duckdb_select_handler::end_scan()
{
  delete result;
  result= nullptr;
  return 0;
}

// ---------------------------------------------------------------------------
// UNION / INTERSECT / EXCEPT pushdown handler
// ---------------------------------------------------------------------------

static select_handler *
create_duckdb_unit_handler(THD *thd, SELECT_LEX_UNIT *unit)
{
  if (!unit) return nullptr;

  // Walk every SELECT arm — all leaf tables must be DUCKDB engine
  TABLE_LIST *first_table= nullptr;
  for (SELECT_LEX *sl= unit->first_select(); sl; sl= sl->next_select())
  {
    List_iterator<TABLE_LIST> it(sl->leaf_tables);
    TABLE_LIST *tl;
    while ((tl= it++))
    {
      if (!tl->table || tl->table->file->ht != duckdb_hton)
        return nullptr;
      if (!first_table) first_table= tl;
    }
  }

  if (!first_table) return nullptr;

  ha_duckdb *h= static_cast<ha_duckdb*>(first_table->table->file);
  if (h->db_file_path.empty()) return nullptr;

  duckdb::DuckDB *db= registry_get(h->db_file_path);
  if (!db) return nullptr;

  duckdb::Connection *conn= nullptr;
  try { conn= new duckdb::Connection(*db); }
  catch (...) { registry_release(h->db_file_path); return nullptr; }

  return new ha_duckdb_select_handler(thd, unit, conn, /*all_duckdb=*/true);
}

// ---------------------------------------------------------------------------
// Cross-engine injection helpers
//
// When the derived-handler pushes a CTE to DuckDB and DuckDB reports a
// missing table (because the CTE references an InnoDB table or another
// non-DuckDB CTE), these helpers inject the missing data into the live
// DuckDB connection as a TEMP TABLE, then the caller retries the query.
//
// Injection is transparent: TEMP tables are connection-scoped and are
// dropped automatically when the derived handler's connection is deleted.
// ---------------------------------------------------------------------------

// Parse "Table with name 'X' does not exist" from a DuckDB error message.
// Returns the table name, or "" if the error is not a missing-table error.
static std::string extract_missing_table(const std::string &errmsg)
{
  // "Table with name 'X'" (quoted, older DuckDB format)
  // "Table with name X does not exist" (unquoted, newer DuckDB format)
  static const char kPrefix1[] = "Table with name ";
  size_t pos = errmsg.find(kPrefix1);
  if (pos != std::string::npos)
  {
    pos += sizeof(kPrefix1) - 1;
    if (pos < errmsg.size() && errmsg[pos] == '\'')
    {
      pos++;  // skip opening quote
      size_t end = errmsg.find('\'', pos);
      if (end != std::string::npos)
        return errmsg.substr(pos, end - pos);
    }
    else
    {
      // Unquoted: name ends at first space or newline
      size_t end = errmsg.find_first_of(" \n\r", pos);
      if (end == std::string::npos) end = errmsg.size();
      if (end > pos)
        return errmsg.substr(pos, end - pos);
    }
  }
  static const char kPrefix2[] = "Referenced table \"";
  pos = errmsg.find(kPrefix2);
  if (pos != std::string::npos)
  {
    pos += sizeof(kPrefix2) - 1;
    size_t end = errmsg.find('"', pos);
    if (end != std::string::npos)
      return errmsg.substr(pos, end - pos);
  }
  return "";
}

// Find an already-open non-DuckDB MariaDB table by its unqualified name.
// Searches thd->open_tables (all tables opened for the current query).
static TABLE *find_open_table_by_name(THD *thd, const std::string &name)
{
  for (TABLE *t = thd->open_tables; t; t = t->next)
  {
    if (t->s && t->s->table_name.str &&
        name == t->s->table_name.str &&
        t->file->ht != duckdb_hton)
      return t;
  }
  return nullptr;
}

// Find a CTE With_element referenced in the current query by name.
// Scans thd->lex->query_tables — CTE references have tl->with != nullptr.
static With_element *find_cte_by_name(THD *thd, const std::string &name)
{
  for (TABLE_LIST *tl = thd->lex->query_tables; tl; tl = tl->next_global)
  {
    if (tl->with &&
        tl->table_name.str &&
        name == tl->table_name.str)
      return tl->with;
  }
  return nullptr;
}

// Inject an already-open MariaDB TABLE into DuckDB as a TEMP TABLE.
// duck_name is the name DuckDB will see (typically the MariaDB table_name).
// Uses CREATE TEMP TABLE IF NOT EXISTS so double-injection is harmless.
static bool inject_table_into_duckdb(duckdb::Connection *conn,
                                      TABLE *t,
                                      const std::string &duck_name,
                                      const std::vector<Item *> &push_conds)
{
  // Cache check: if this table was fully injected on this connection before,
  // verify the InnoDB row count hasn't changed since then.  If it matches,
  // the TEMP TABLE is still valid — skip injection entirely.  If it differs,
  // the table was modified (INSERT/DELETE/TRUNCATE); invalidate and re-inject.
  {
    mysql_mutex_lock(&g_duckdb_mutex);
    auto conn_it= g_injected_cache.find(conn);
    bool have_entry= false;
    ha_rows cached_records= 0;
    if (conn_it != g_injected_cache.end())
    {
      auto tbl_it= conn_it->second.find(duck_name);
      if (tbl_it != conn_it->second.end())
      {
        have_entry= true;
        cached_records= tbl_it->second;
      }
    }
    mysql_mutex_unlock(&g_duckdb_mutex);

    if (have_entry)
    {
      // Refresh approximate row count from InnoDB and compare.
      t->file->info(HA_STATUS_VARIABLE);
      ha_rows current_records= t->file->stats.records;
      if (current_records == cached_records)
        return true;  // Cache hit: table unchanged.

      // Row count changed — evict stale entry; fall through to re-inject.
      mysql_mutex_lock(&g_duckdb_mutex);
      auto it= g_injected_cache.find(conn);
      if (it != g_injected_cache.end())
        it->second.erase(duck_name);
      mysql_mutex_unlock(&g_duckdb_mutex);
    }
  }

  // Drop any pre-existing TEMP TABLE for this name.  With a persistent per-THD
  // connection a previous query may have left a filtered (partial) copy; we
  // need a clean slate before injecting (possibly with different predicates).
  conn->Query("DROP TABLE IF EXISTS \"" + duck_name + "\"");

  // Build CREATE TEMP TABLE with the same schema as the MariaDB table
  std::ostringstream create_sql;
  create_sql << "CREATE TEMP TABLE \"" << duck_name << "\" (";
  for (uint i = 0; i < t->s->fields; i++)
  {
    if (i > 0) create_sql << ", ";
    create_sql << '"' << t->field[i]->field_name.str << "\" "
               << field_type_to_duckdb(t->field[i]);
  }
  create_sql << ")";

  auto cr = conn->Query(create_sql.str());
  if (cr->HasError())
  {
    sql_print_warning("DuckDB: inject_table CREATE failed for '%s': %s",
                      duck_name.c_str(),
                      cr->GetErrorObject().Message().c_str());
    return false;
  }

  // Temporarily enable all-column reads so val_int/val_str work for every field
  MY_BITMAP *saved_read_set = t->read_set;
  t->read_set = &t->s->all_set;

  bool ok = true;
  try
  {
    duckdb::Appender appender(*conn, duck_name);

    if (t->file->ha_rnd_init(1) != 0)
    {
      t->read_set = saved_read_set;
      return false;
    }

    int rc;
    while ((rc = t->file->ha_rnd_next(t->record[0])) == 0)
    {
      // Evaluate pushed-down single-table predicates; skip non-matching rows.
      if (!push_conds.empty())
      {
        bool pass= true;
        for (Item *pred : push_conds)
          if (!pred->val_int()) { pass= false; break; }
        if (!pass) continue;
      }
      appender.BeginRow();
      for (uint i = 0; i < t->s->fields; i++)
      {
        Field *f = t->field[i];
        if (f->is_null())
        {
          appender.Append<nullptr_t>(nullptr);
          continue;
        }
        switch (f->type())
        {
          case MYSQL_TYPE_TINY:
          case MYSQL_TYPE_SHORT:
          case MYSQL_TYPE_LONG:
          case MYSQL_TYPE_INT24:
            appender.Append<int32_t>((int32_t)f->val_int());
            break;
          case MYSQL_TYPE_LONGLONG:
            appender.Append<int64_t>(f->val_int());
            break;
          case MYSQL_TYPE_FLOAT:
            appender.Append<float>((float)f->val_real());
            break;
          case MYSQL_TYPE_DOUBLE:
            appender.Append<double>(f->val_real());
            break;
          default:
          {
            String sv;
            f->val_str(&sv, &sv);
            appender.Append(duckdb::Value(std::string(sv.ptr(), sv.length())));
            break;
          }
        }
      }
      appender.EndRow();
    }
    t->file->ha_rnd_end();
    appender.Close();
  }
  catch (const std::exception &e)
  {
    sql_print_warning("DuckDB: inject_table Appender failed for '%s': %s",
                      duck_name.c_str(), e.what());
    t->file->ha_rnd_end();
    ok = false;
  }

  t->read_set = saved_read_set;

  // Cache successful full (unfiltered) injections.  Store the current row
  // count so future lookups can detect InnoDB modifications.
  if (ok && push_conds.empty())
  {
    t->file->info(HA_STATUS_VARIABLE);
    mysql_mutex_lock(&g_duckdb_mutex);
    g_injected_cache[conn][duck_name]= t->file->stats.records;
    mysql_mutex_unlock(&g_duckdb_mutex);
  }

  return ok;
}

// Inject a CTE into DuckDB as a TEMP TABLE.
// First pre-injects any non-DuckDB leaf tables that the CTE body references,
// then executes CREATE TEMP TABLE name AS (cte_sql) in DuckDB.
static bool inject_cte_into_duckdb(duckdb::Connection *conn, THD *thd,
                                    With_element *cte,
                                    const std::string &cte_name);

static bool inject_cte_into_duckdb(duckdb::Connection *conn, THD *thd,
                                    With_element *cte,
                                    const std::string &cte_name)
{
  // Pre-inject leaf tables so the CREATE TEMP TABLE AS (...) will succeed
  SELECT_LEX *cte_sel = cte->spec->first_select();
  if (cte_sel)
  {
    List_iterator<TABLE_LIST> it(cte_sel->leaf_tables);
    TABLE_LIST *tl;
    while ((tl = it++))
    {
      if (!tl->table || tl->table->file->ht == duckdb_hton)
        continue;  // skip DuckDB tables

      std::string leaf_name = tl->table->s->table_name.str;

      if (tl->with)
      {
        // Leaf is itself a CTE — recurse
        if (!inject_cte_into_duckdb(conn, thd, tl->with, leaf_name))
          return false;
      }
      else
      {
        if (!inject_table_into_duckdb(conn, tl->table, leaf_name))
          return false;
      }
    }
  }

  // Now materialize the CTE body into a DuckDB temp table
  String spec_str;
  cte->spec->print(&spec_str, QT_ORDINARY);
  std::string cte_sql(spec_str.ptr(), spec_str.length());
  for (char &c : cte_sql) if (c == '`') c = '"';
  cte_sql = rewrite_mariadb_sql(cte_sql);

  std::string create_sql =
    "CREATE TEMP TABLE IF NOT EXISTS \"" + cte_name + "\" AS (" + cte_sql + ")";
  auto r = conn->Query(create_sql);
  if (r->HasError())
  {
    sql_print_warning("DuckDB: inject_cte failed for '%s': %s\nSQL: %s",
                      cte_name.c_str(),
                      r->GetErrorObject().Message().c_str(),
                      create_sql.c_str());
    return false;
  }
  return true;
}

// ---------------------------------------------------------------------------
// Derived table pushdown handler
// ---------------------------------------------------------------------------

static derived_handler *
create_duckdb_derived_handler(THD *thd, TABLE_LIST *derived)
{
  if (!derived || !derived->derived) return nullptr;

  SELECT_LEX *sel= derived->derived->first_select();
  if (!sel) return nullptr;

  // Require at least one DuckDB leaf (to identify the database and connection).
  // All other leaves — whether CTE-backed or physical MariaDB tables — are
  // injectable at runtime by init_scan's retry loop, so we allow them here.
  TABLE_LIST *duckdb_leaf= nullptr;
  {
    List_iterator<TABLE_LIST> it(sel->leaf_tables);
    TABLE_LIST *tl;
    while ((tl= it++))
    {
      if (!tl->table)
        continue;  // unresolved ref — injection will handle it
      if (tl->table->file->ht == duckdb_hton && !duckdb_leaf)
        duckdb_leaf= tl;
      // non-DuckDB leaves (CTE or physical) are injectable at runtime
    }
  }
  if (!duckdb_leaf) return nullptr;

  ha_duckdb *h= static_cast<ha_duckdb*>(duckdb_leaf->table->file);
  if (h->db_file_path.empty()) return nullptr;

  duckdb::DuckDB *db= registry_get(h->db_file_path);
  if (!db) return nullptr;

  duckdb::Connection *conn= nullptr;
  try { conn= new duckdb::Connection(*db); }
  catch (...) { registry_release(h->db_file_path); return nullptr; }

  return new ha_duckdb_derived_handler(thd, derived, conn);
}

ha_duckdb_derived_handler::ha_duckdb_derived_handler(THD *thd, TABLE_LIST *tbl,
                                                     duckdb::Connection *conn)
  : derived_handler(thd, duckdb_hton),
    connection(conn), result(nullptr), current_row(0)
{
  derived= tbl;
}

ha_duckdb_derived_handler::~ha_duckdb_derived_handler()
{
  delete result;
  delete connection;
}

int ha_duckdb_derived_handler::init_scan()
{
  // unit is inherited from derived_handler base; print() reconstructs the full
  // subquery SQL including GROUP BY, HAVING, ORDER BY.
  String query_str;
  unit->print(&query_str, QT_ORDINARY);
  std::string sql(query_str.ptr(), query_str.length());

  for (char &c : sql)
    if (c == '`') c = '"';
  sql= rewrite_mariadb_sql(sql);

  // Proactively inject all non-DuckDB leaf tables into DuckDB as TEMP TABLEs.
  // This handles the common case (MariaDB CTEs or InnoDB tables inlined into
  // the CTE body) before the first DuckDB attempt.  For each injected table we
  // also strip its schema qualifier from the SQL so DuckDB can find the
  // unqualified temp table.
  {
    SELECT_LEX *sel= unit->first_select();
    if (sel)
    {
      List_iterator<TABLE_LIST> it(sel->leaf_tables);
      TABLE_LIST *tl;
      while ((tl= it++))
      {
        if (!tl->table || tl->table->file->ht == duckdb_hton)
          continue;  // DuckDB table — no injection needed

        std::string tname(tl->table->s->table_name.str);
        bool ok= false;
        if (tl->with)
          ok= inject_cte_into_duckdb(connection, thd, tl->with, tname);
        else
          ok= inject_table_into_duckdb(connection, tl->table, tname);

        if (ok)
          strip_schema_for_table(sql, tname);
        else
          sql_print_warning("DuckDB derived: pre-inject '%s' failed; retry loop will handle it",
                            tname.c_str());
      }
    }
  }

  // Fix bare INNER JOINs: MariaDB's printer moves ON conditions to WHERE,
  // which DuckDB rejects.  Convert "JOIN table WHERE" to "JOIN table ON TRUE WHERE".
  sql= fix_bare_joins(sql);

  sql_print_information("DuckDB derived SQL (final): %s", sql.c_str());

  // Retry loop: if DuckDB still reports a missing table (e.g. a table referenced
  // via a subquery alias not in leaf_tables), inject it and retry.
  for (int attempt = 0; attempt < 8; attempt++)
  {
    std::unique_ptr<duckdb::MaterializedQueryResult> r;
    try
    {
      register_active_connection(thd, connection);
      r= connection->Query(sql);
      unregister_active_connection(thd);
    }
    catch (const std::exception &e)
    {
      unregister_active_connection(thd);
      sql_print_error("DuckDB derived pushdown exception: %s", e.what());
      return 1;
    }

    if (!r->HasError())
    {
      result= r.release();
      current_row= 0;
      return 0;
    }

    std::string errmsg= r->GetErrorObject().Message();
    std::string missing= extract_missing_table(errmsg);

    if (missing.empty())
    {
      sql_print_error("DuckDB derived pushdown failed: %s\nSQL: %s",
                      errmsg.c_str(), sql.c_str());
      return 1;
    }

    // Resolve missing table from MariaDB — physical table first, then CTE
    TABLE *open_tbl= find_open_table_by_name(thd, missing);
    if (open_tbl)
    {
      if (!inject_table_into_duckdb(connection, open_tbl, missing))
      {
        sql_print_error("DuckDB derived pushdown: failed to inject table '%s'",
                        missing.c_str());
        return 1;
      }
      strip_schema_for_table(sql, missing);
      sql= fix_bare_joins(sql);
      continue;
    }

    With_element *cte= find_cte_by_name(thd, missing);
    if (cte)
    {
      if (!inject_cte_into_duckdb(connection, thd, cte, missing))
      {
        sql_print_error("DuckDB derived pushdown: failed to inject CTE '%s'",
                        missing.c_str());
        return 1;
      }
      strip_schema_for_table(sql, missing);
      sql= fix_bare_joins(sql);
      continue;
    }

    // Cannot resolve — propagate original DuckDB error
    sql_print_error("DuckDB derived pushdown: unresolvable table '%s'\n"
                    "Full error: %s\nSQL: %s",
                    missing.c_str(), errmsg.c_str(), sql.c_str());
    return 1;
  }

  sql_print_error("DuckDB derived pushdown: injection retry limit reached\nSQL: %s",
                  sql.c_str());
  return 1;
}

int ha_duckdb_derived_handler::next_row()
{
  if (!result || current_row >= result->RowCount())
    return HA_ERR_END_OF_FILE;

  memset(table->record[0], 0, table->s->null_bytes);

  for (uint i= 0; i < table->s->fields && i < result->ColumnCount(); i++)
  {
    Field *field= table->field[i];
    duckdb::Value val= result->GetValue(i, current_row);
    if (val.IsNull()) { field->set_null(); continue; }
    field->set_notnull();
    std::string s= val.ToString();
    field->store(s.c_str(), s.length(), system_charset_info);
  }
  current_row++;
  return 0;
}

int ha_duckdb_derived_handler::end_scan()
{
  delete result;
  result= nullptr;
  return 0;
}

// ---------------------------------------------------------------------------
// Plugin registration
// ---------------------------------------------------------------------------

struct st_mysql_storage_engine duckdb_storage_engine=
{ MYSQL_HANDLERTON_INTERFACE_VERSION };

maria_declare_plugin(duckdb)
{
  MYSQL_STORAGE_ENGINE_PLUGIN,
  &duckdb_storage_engine,
  "DUCKDB",
  "DuckDB Storage Engine Team",
  "DuckDB storage engine with native backend and query pushdown",
  PLUGIN_LICENSE_GPL,
  duckdb_init_func,
  duckdb_done_func,
  0x0100,
  NULL,
  duckdb_system_variables,
  "1.0",
  MariaDB_PLUGIN_MATURITY_STABLE,
}
maria_declare_plugin_end;
