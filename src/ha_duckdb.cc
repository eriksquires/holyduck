#define MYSQL_SERVER 1
#include "ha_duckdb.h"
#include "probes_mysql.h"
#include "sql_class.h"

// DuckDB includes - undef UNKNOWN macro defined by MariaDB's item_cmpfunc.h
// to avoid collision with DuckDB's enum values
#ifdef UNKNOWN
#undef UNKNOWN
#endif
#include "duckdb.hpp"

#include <string>
#include <sstream>

static handlerton *duckdb_hton;

static const char *ha_duckdb_exts[] = {
  ".duckdb",
  NullS
};

#ifdef HAVE_PSI_INTERFACE
static PSI_mutex_key dk_key_mutex_DuckDB_share_mutex;

static PSI_mutex_info all_duckdb_mutexes[]=
{
  { &dk_key_mutex_DuckDB_share_mutex, "DuckDB_share::mutex", 0}
};

static void init_duckdb_psi_keys()
{
  const char* category= "duckdb";
  int count= array_elements(all_duckdb_mutexes);
  mysql_mutex_register(category, all_duckdb_mutexes, count);
}
#endif

// Forward declaration
static handler* create_duckdb_handler(handlerton *hton, TABLE_SHARE *table,
                                      MEM_ROOT *mem_root);

static int duckdb_init_func(void *p)
{
  DBUG_ENTER("duckdb_init_func");

#ifdef HAVE_PSI_INTERFACE
  init_duckdb_psi_keys();
#endif

  duckdb_hton= (handlerton *)p;
  duckdb_hton->create= create_duckdb_handler;
  duckdb_hton->flags= HTON_CAN_RECREATE;
  duckdb_hton->tablefile_extensions= ha_duckdb_exts;

  DBUG_RETURN(0);
}

static handler* create_duckdb_handler(handlerton *hton,
                                      TABLE_SHARE *table,
                                      MEM_ROOT *mem_root)
{
  return new (mem_root) ha_duckdb(hton, table);
}

/**
  DuckDB_share implementation
*/
DuckDB_share::DuckDB_share()
{
  thr_lock_init(&lock);
  mysql_mutex_init(dk_key_mutex_DuckDB_share_mutex,
                   &mutex, MY_MUTEX_INIT_FAST);
  db_instance= nullptr;
}

DuckDB_share::~DuckDB_share()
{
  cleanup_database();
  thr_lock_delete(&lock);
  mysql_mutex_destroy(&mutex);
}

int DuckDB_share::init_database(const char *path)
{
  DBUG_ENTER("DuckDB_share::init_database");

  try
  {
    db_instance= new duckdb::DuckDB(path);
    sql_print_information("DuckDB storage engine: Opened database at %s",
                          path ? path : ":memory:");
  }
  catch (const std::exception& e)
  {
    sql_print_error("DuckDB storage engine: Failed to open database: %s", e.what());
    DBUG_RETURN(1);
  }

  DBUG_RETURN(0);
}

void DuckDB_share::cleanup_database()
{
  if (db_instance)
  {
    delete db_instance;
    db_instance= nullptr;
  }
}

/**
  ha_duckdb implementation
*/
ha_duckdb::ha_duckdb(handlerton *hton, TABLE_SHARE *table_arg)
  :handler(hton, table_arg), share(nullptr), connection(nullptr),
   scan_result(nullptr), scan_row(0)
{
}

ha_duckdb::~ha_duckdb()
{
  delete scan_result;
  scan_result= nullptr;
}

const char **ha_duckdb::bas_ext() const
{
  return ha_duckdb_exts;
}

DuckDB_share *ha_duckdb::get_share()
{
  DuckDB_share *tmp_share;

  DBUG_ENTER("ha_duckdb::get_share");

  lock_shared_ha_data();
  if (!(tmp_share= static_cast<DuckDB_share*>(get_ha_share_ptr())))
  {
    tmp_share= new DuckDB_share;
    if (!tmp_share)
      goto err;
    set_ha_share_ptr(static_cast<Handler_share*>(tmp_share));
  }
err:
  unlock_shared_ha_data();
  DBUG_RETURN(tmp_share);
}

/**
  Build a DuckDB CREATE TABLE statement from a MariaDB table definition.
*/
std::string ha_duckdb::build_create_sql(TABLE *table_arg)
{
  std::ostringstream sql;
  sql << "CREATE TABLE IF NOT EXISTS " << table_arg->s->table_name.str << " (";

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
      case MYSQL_TYPE_INT24:
        sql << "INTEGER"; break;
      case MYSQL_TYPE_LONGLONG:
        sql << "BIGINT"; break;
      case MYSQL_TYPE_FLOAT:
        sql << "FLOAT"; break;
      case MYSQL_TYPE_DOUBLE:
        sql << "DOUBLE"; break;
      case MYSQL_TYPE_DECIMAL:
      case MYSQL_TYPE_NEWDECIMAL:
        sql << "DECIMAL"; break;
      case MYSQL_TYPE_DATE:
        sql << "DATE"; break;
      case MYSQL_TYPE_TIME:
        sql << "TIME"; break;
      case MYSQL_TYPE_DATETIME:
      case MYSQL_TYPE_TIMESTAMP:
        sql << "TIMESTAMP"; break;
      default:
        sql << "VARCHAR";
    }
  }

  sql << ")";
  return sql.str();
}

/**
  Table operations
*/
int ha_duckdb::open(const char *name, int mode, uint test_if_locked)
{
  DBUG_ENTER("ha_duckdb::open");

  db_path= std::string(name) + ".duckdb";

  std::string name_str(name);
  size_t slash_pos= name_str.rfind('/');
  duckdb_table_name= (slash_pos != std::string::npos) ?
                     name_str.substr(slash_pos + 1) : name_str;

  if (!(share= get_share()))
    DBUG_RETURN(1);

  // Initialise the DuckDB database on first open
  if (!share->db_instance)
  {
    if (share->init_database(db_path.c_str()))
      DBUG_RETURN(1);
  }

  try
  {
    connection= new duckdb::Connection(*share->db_instance);
  }
  catch (const std::exception& e)
  {
    sql_print_error("DuckDB storage engine: Failed to create connection: %s", e.what());
    DBUG_RETURN(1);
  }

  thr_lock_data_init(&share->lock, &lock, NULL);

  DBUG_RETURN(0);
}

int ha_duckdb::close(void)
{
  DBUG_ENTER("ha_duckdb::close");

  delete scan_result;
  scan_result= nullptr;

  delete connection;
  connection= nullptr;

  DBUG_RETURN(0);
}

int ha_duckdb::create(const char *name, TABLE *table_arg,
                      HA_CREATE_INFO *create_info)
{
  DBUG_ENTER("ha_duckdb::create");

  std::string name_str(name);
  size_t slash_pos= name_str.rfind('/');
  duckdb_table_name= (slash_pos != std::string::npos) ?
                     name_str.substr(slash_pos + 1) : name_str;
  db_path= name_str + ".duckdb";

  std::string create_sql= build_create_sql(table_arg);

  try
  {
    // create() is called before open() so we use a short-lived local instance
    duckdb::DuckDB tmp_db(db_path.c_str());
    duckdb::Connection tmp_conn(tmp_db);
    auto result= tmp_conn.Query(create_sql);

    if (result->HasError())
    {
      sql_print_error("DuckDB storage engine: Failed to create table: %s",
                      result->GetError().c_str());
      DBUG_RETURN(1);
    }

    sql_print_information("DuckDB storage engine: Created table '%s'",
                          duckdb_table_name.c_str());
  }
  catch (const std::exception& e)
  {
    sql_print_error("DuckDB storage engine: Exception in create: %s", e.what());
    DBUG_RETURN(1);
  }

  DBUG_RETURN(0);
}

int ha_duckdb::delete_table(const char *name)
{
  DBUG_ENTER("ha_duckdb::delete_table");

  std::string db_file= std::string(name) + ".duckdb";
  my_delete(db_file.c_str(), MYF(0));

  // DuckDB also creates a WAL file
  std::string wal_file= db_file + ".wal";
  my_delete(wal_file.c_str(), MYF(0));

  DBUG_RETURN(0);
}

int ha_duckdb::rename_table(const char *from, const char *to)
{
  DBUG_ENTER("ha_duckdb::rename_table");

  std::string from_db= std::string(from) + ".duckdb";
  std::string to_db=   std::string(to)   + ".duckdb";
  my_rename(from_db.c_str(), to_db.c_str(), MYF(0));

  DBUG_RETURN(0);
}

/**
  Row write — convert MariaDB row buffer to DuckDB via Appender
*/
int ha_duckdb::write_row(const uchar *buf)
{
  DBUG_ENTER("ha_duckdb::write_row");

  if (!connection)
    DBUG_RETURN(HA_ERR_INTERNAL_ERROR);

  try
  {
    duckdb::Appender appender(*connection, duckdb_table_name);
    appender.BeginRow();

    for (uint i= 0; i < table->s->fields; i++)
    {
      Field *field= table->field[i];

      if (field->is_null())
      {
        appender.Append<nullptr_t>(nullptr);
        continue;
      }

      switch (field->type())
      {
        case MYSQL_TYPE_TINY:
        case MYSQL_TYPE_SHORT:
        case MYSQL_TYPE_LONG:
        case MYSQL_TYPE_INT24:
          appender.Append<int32_t>((int32_t)field->val_int());
          break;
        case MYSQL_TYPE_LONGLONG:
          appender.Append<int64_t>(field->val_int());
          break;
        case MYSQL_TYPE_FLOAT:
          appender.Append<float>((float)field->val_real());
          break;
        case MYSQL_TYPE_DOUBLE:
          appender.Append<double>(field->val_real());
          break;
        default:
        {
          String str_val;
          field->val_str(&str_val, &str_val);
          appender.Append(duckdb::Value(std::string(str_val.ptr(),
                                                    str_val.length())));
          break;
        }
      }
    }

    appender.EndRow();
    appender.Flush();
  }
  catch (const std::exception& e)
  {
    sql_print_error("DuckDB storage engine: write_row failed: %s", e.what());
    DBUG_RETURN(HA_ERR_INTERNAL_ERROR);
  }

  DBUG_RETURN(0);
}

int ha_duckdb::update_row(const uchar *old_data, const uchar *new_data)
{
  DBUG_ENTER("ha_duckdb::update_row");
  DBUG_RETURN(HA_ERR_WRONG_COMMAND);
}

int ha_duckdb::delete_row(const uchar *buf)
{
  DBUG_ENTER("ha_duckdb::delete_row");
  DBUG_RETURN(HA_ERR_WRONG_COMMAND);
}

/**
  Scan operations
*/
int ha_duckdb::rnd_init(bool scan)
{
  DBUG_ENTER("ha_duckdb::rnd_init");

  if (!connection)
    DBUG_RETURN(HA_ERR_INTERNAL_ERROR);

  delete scan_result;
  scan_result= nullptr;
  scan_row= 0;

  try
  {
    std::string sql= "SELECT * FROM " + duckdb_table_name;
    auto result= connection->Query(sql);

    if (result->HasError())
    {
      sql_print_error("DuckDB storage engine: rnd_init query failed: %s",
                      result->GetError().c_str());
      DBUG_RETURN(HA_ERR_INTERNAL_ERROR);
    }

    scan_result= result.release();
  }
  catch (const std::exception& e)
  {
    sql_print_error("DuckDB storage engine: rnd_init failed: %s", e.what());
    DBUG_RETURN(HA_ERR_INTERNAL_ERROR);
  }

  DBUG_RETURN(0);
}

int ha_duckdb::rnd_end()
{
  DBUG_ENTER("ha_duckdb::rnd_end");

  delete scan_result;
  scan_result= nullptr;

  DBUG_RETURN(0);
}

/**
  Convert a DuckDB result row into a MariaDB row buffer.
*/
int ha_duckdb::convert_row_from_duckdb(uchar *buf, size_t row_idx)
{
  memset(buf, 0, table->s->null_bytes);

  for (uint i= 0; i < table->s->fields; i++)
  {
    Field *field= table->field[i];
    duckdb::Value val= scan_result->GetValue(i, row_idx);

    if (val.IsNull())
    {
      field->set_null();
      continue;
    }

    field->set_notnull();

    switch (field->type())
    {
      case MYSQL_TYPE_TINY:
      case MYSQL_TYPE_SHORT:
      case MYSQL_TYPE_LONG:
      case MYSQL_TYPE_INT24:
        field->store(val.GetValue<int32_t>(), false);
        break;
      case MYSQL_TYPE_LONGLONG:
        field->store(val.GetValue<int64_t>(), false);
        break;
      case MYSQL_TYPE_FLOAT:
      case MYSQL_TYPE_DOUBLE:
        field->store(val.GetValue<double>());
        break;
      default:
      {
        std::string str= val.ToString();
        field->store(str.c_str(), str.length(), system_charset_info);
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

  convert_row_from_duckdb(buf, scan_row);
  scan_row++;

  DBUG_RETURN(0);
}

int ha_duckdb::rnd_pos(uchar *buf, uchar *pos)
{
  DBUG_ENTER("ha_duckdb::rnd_pos");
  DBUG_RETURN(HA_ERR_WRONG_COMMAND);
}

void ha_duckdb::position(const uchar *record)
{
  DBUG_ENTER("ha_duckdb::position");
  DBUG_VOID_RETURN;
}

/**
  Lock management
*/
THR_LOCK_DATA **ha_duckdb::store_lock(THD *thd, THR_LOCK_DATA **to,
                                      enum thr_lock_type lock_type)
{
  if (lock_type != TL_IGNORE && lock.type == TL_UNLOCK)
    lock.type= lock_type;
  *to++= &lock;
  return to;
}

/**
  Table statistics
*/
int ha_duckdb::info(uint flag)
{
  DBUG_ENTER("ha_duckdb::info");

  if ((flag & HA_STATUS_VARIABLE) && connection)
  {
    try
    {
      std::string sql= "SELECT COUNT(*) FROM " + duckdb_table_name;
      auto result= connection->Query(sql);
      if (!result->HasError() && result->RowCount() > 0)
        stats.records= (ha_rows)result->GetValue(0, 0).GetValue<int64_t>();
    }
    catch (...) {}
  }

  DBUG_RETURN(0);
}

ha_rows ha_duckdb::records_in_range(uint inx, const key_range *min_key,
                                    const key_range *max_key, page_range *res)
{
  DBUG_ENTER("ha_duckdb::records_in_range");
  DBUG_RETURN(stats.records ? stats.records : 10);
}

/**
  Plugin registration
*/
struct st_mysql_storage_engine duckdb_storage_engine=
{ MYSQL_HANDLERTON_INTERFACE_VERSION };

mysql_declare_plugin(duckdb)
{
  MYSQL_STORAGE_ENGINE_PLUGIN,
  &duckdb_storage_engine,
  "DUCKDB",
  "DuckDB Storage Engine Team",
  "DuckDB storage engine with native backend",
  PLUGIN_LICENSE_GPL,
  duckdb_init_func,
  NULL,
  0x0100,
  NULL,
  NULL,
  NULL,
  0,
}
mysql_declare_plugin_end;
