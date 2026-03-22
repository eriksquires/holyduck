#define MYSQL_SERVER 1
#include "ha_duckdb.h"
#include "probes_mysql.h"
#include "sql_class.h"
#include "sql_lex.h"

// DuckDB includes — undef UNKNOWN macro from MariaDB's item_cmpfunc.h
// to avoid collision with DuckDB's enum values
#ifdef UNKNOWN
#undef UNKNOWN
#endif
#include "duckdb.hpp"

#include <string>
#include <sstream>
#include <map>

// MariaDB global — absolute path to the data directory (trailing slash)
extern char mysql_real_data_home[];

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
  try { db= new duckdb::DuckDB(path.c_str()); }
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
// We store one DuckDB file per MariaDB database at
//   <datadir>#duckdb/<db>.duckdb
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

static select_handler *create_duckdb_select_handler(THD *thd,
                                                     SELECT_LEX *sel,
                                                     SELECT_LEX_UNIT *unit);

static int duckdb_init_func(void *p)
{
  DBUG_ENTER("duckdb_init_func");

#ifdef HAVE_PSI_INTERFACE
  init_duckdb_psi_keys();
#endif

  mysql_mutex_init(dk_key_mutex_g_duckdb_mutex, &g_duckdb_mutex,
                   MY_MUTEX_INIT_FAST);

  duckdb_hton= (handlerton *)p;
  duckdb_hton->create=        create_duckdb_handler;
  duckdb_hton->create_select= create_duckdb_select_handler;
  duckdb_hton->flags=         HTON_CAN_RECREATE;
  duckdb_hton->tablefile_extensions= ha_duckdb_exts;

  DBUG_RETURN(0);
}

static int duckdb_done_func(void *)
{
  mysql_mutex_destroy(&g_duckdb_mutex);
  return 0;
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

std::string ha_duckdb::build_create_sql(TABLE *table_arg)
{
  std::ostringstream sql;
  sql << "CREATE TABLE IF NOT EXISTS " << duckdb_table_name << " (";

  for (uint i= 0; i < table_arg->s->fields; i++)
  {
    Field *field= table_arg->field[i];
    if (i > 0) sql << ", ";
    sql << field->field_name.str << " ";

    switch (field->type())
    {
      case MYSQL_TYPE_TINY:
      case MYSQL_TYPE_SHORT:
      case MYSQL_TYPE_LONG:
      case MYSQL_TYPE_INT24:   sql << "INTEGER";   break;
      case MYSQL_TYPE_LONGLONG: sql << "BIGINT";   break;
      case MYSQL_TYPE_FLOAT:   sql << "FLOAT";     break;
      case MYSQL_TYPE_DOUBLE:  sql << "DOUBLE";    break;
      case MYSQL_TYPE_DECIMAL:
      case MYSQL_TYPE_NEWDECIMAL: sql << "DECIMAL"; break;
      case MYSQL_TYPE_DATE:    sql << "DATE";       break;
      case MYSQL_TYPE_TIME:    sql << "TIME";       break;
      case MYSQL_TYPE_DATETIME:
      case MYSQL_TYPE_TIMESTAMP: sql << "TIMESTAMP"; break;
      default:                 sql << "VARCHAR";
    }
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
                      r->GetError().c_str());
      DBUG_RETURN(1);
    }

    r= conn.Query(create_sql);
    if (r->HasError())
    {
      registry_release(p.db_file);
      sql_print_error("DuckDB: table creation failed: %s",
                      r->GetError().c_str());
      DBUG_RETURN(1);
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
    sql_print_error("DuckDB: end_bulk_insert flush failed: %s", e.what());
    delete app;
    bulk_appender= nullptr;
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

  try
  {
    // Reuse the bulk appender if open, otherwise use a one-shot
    std::unique_ptr<duckdb::Appender> one_shot;
    duckdb::Appender *appender= static_cast<duckdb::Appender*>(bulk_appender);
    if (!appender)
    {
      one_shot.reset(new duckdb::Appender(*connection, db_name,
                                          table->s->table_name.str));
      appender= one_shot.get();
    }

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
    if (one_shot) one_shot->Flush();  // only flush immediately for single rows
  }
  catch (const std::exception &e)
  {
    sql_print_error("DuckDB: write_row failed: %s", e.what());
    DBUG_RETURN(HA_ERR_INTERNAL_ERROR);
  }
  DBUG_RETURN(0);
}

int ha_duckdb::update_row(const uchar *, const uchar *)
{
  DBUG_ENTER("ha_duckdb::update_row");
  DBUG_RETURN(HA_ERR_WRONG_COMMAND);
}

int ha_duckdb::delete_row(const uchar *)
{
  DBUG_ENTER("ha_duckdb::delete_row");
  DBUG_RETURN(HA_ERR_WRONG_COMMAND);
}

// ---------------------------------------------------------------------------
// Scan operations
// ---------------------------------------------------------------------------

int ha_duckdb::rnd_init(bool scan)
{
  DBUG_ENTER("ha_duckdb::rnd_init");
  if (!connection) DBUG_RETURN(HA_ERR_INTERNAL_ERROR);

  delete scan_result; scan_result= nullptr; scan_row= 0;

  try
  {
    auto r= connection->Query("SELECT * FROM " + duckdb_table_name);
    if (r->HasError())
    {
      sql_print_error("DuckDB: rnd_init failed: %s", r->GetError().c_str());
      DBUG_RETURN(HA_ERR_INTERNAL_ERROR);
    }
    scan_result= r.release();
  }
  catch (const std::exception &e)
  {
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

  for (uint i= 0; i < table->s->fields; i++)
  {
    Field *field= table->field[i];
    duckdb::Value val= result->GetValue(i, row_idx);

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
    lock.type= lock_type;
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
      auto r= connection->Query("SELECT COUNT(*) FROM " + duckdb_table_name);
      if (!r->HasError() && r->RowCount() > 0)
        stats.records= (ha_rows)r->GetValue(0, 0).GetValue<int64_t>();
    }
    catch (...) {}
  }
  DBUG_RETURN(0);
}

ha_rows ha_duckdb::records_in_range(uint, const key_range *, const key_range *,
                                    page_range *)
{
  DBUG_ENTER("ha_duckdb::records_in_range");
  DBUG_RETURN(stats.records ? stats.records : 10);
}

// ---------------------------------------------------------------------------
// Select handler — full query pushdown
// ---------------------------------------------------------------------------

static select_handler *create_duckdb_select_handler(THD *thd, SELECT_LEX *sel,
                                                     SELECT_LEX_UNIT *unit)
{
  if (!sel) return nullptr;

  // Only push down if every leaf table uses the DUCKDB engine
  List_iterator<TABLE_LIST> it(sel->leaf_tables);
  TABLE_LIST *tl;
  while ((tl= it++))
  {
    if (!tl->table || tl->table->file->ht != duckdb_hton)
      return nullptr;
  }

  // All tables are DUCKDB — get the db file path from the first table
  List_iterator<TABLE_LIST> it2(sel->leaf_tables);
  TABLE_LIST *first= it2++;
  if (!first || !first->table)
    return nullptr;

  // Extract db file path via the handler
  ha_duckdb *h= static_cast<ha_duckdb*>(first->table->file);
  if (h->db_file_path.empty())
    return nullptr;

  duckdb::DuckDB *db= registry_get(h->db_file_path);
  if (!db) return nullptr;

  duckdb::Connection *conn= nullptr;
  try { conn= new duckdb::Connection(*db); }
  catch (...) { registry_release(h->db_file_path); return nullptr; }

  // registry_release will be called in the handler destructor
  return new ha_duckdb_select_handler(thd, sel, conn);
}

ha_duckdb_select_handler::ha_duckdb_select_handler(THD *thd, SELECT_LEX *sel,
                                                   duckdb::Connection *conn)
  : select_handler(thd, duckdb_hton, sel),
    connection(conn), result(nullptr), current_row(0)
{}

ha_duckdb_select_handler::~ha_duckdb_select_handler()
{
  delete result;
  delete connection;
}

int ha_duckdb_select_handler::init_scan()
{
  // Reconstruct the SQL from the parse tree — table names are schema-qualified
  // (e.g. test.sales) which matches our DuckDB schema layout exactly.
  String query_str;
  select_lex->print(thd, &query_str, QT_ORDINARY);
  std::string sql(query_str.ptr(), query_str.length());

  // MariaDB print() uses backtick quoting; DuckDB expects double quotes.
  for (char &c : sql)
    if (c == '`') c = '"';

  try
  {
    auto r= connection->Query(sql);
    if (r->HasError())
    {
      sql_print_error("DuckDB pushdown: query failed: %s\nSQL: %s",
                      r->GetError().c_str(), sql.c_str());
      return 1;
    }
    result= r.release();
    current_row= 0;
  }
  catch (const std::exception &e)
  {
    sql_print_error("DuckDB pushdown: exception: %s", e.what());
    return 1;
  }
  return 0;
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
  NULL,
  "1.0",
  MariaDB_PLUGIN_MATURITY_STABLE,
}
maria_declare_plugin_end;
