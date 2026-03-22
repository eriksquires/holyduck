#ifndef HA_DUCKDB_INCLUDED
#define HA_DUCKDB_INCLUDED

/*
   Copyright (c) 2026, DuckDB Storage Engine for MariaDB

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.
*/

#ifdef USE_PRAGMA_INTERFACE
#pragma interface
#endif

#include "my_global.h"
#include "thr_lock.h"
#include "handler.h"
#include "my_base.h"
#include "select_handler.h"
#include <string>

// Forward declare DuckDB types
namespace duckdb {
    class Connection;
    class DuckDB;
    class MaterializedQueryResult;
}

/**
  @brief
  DuckDB_share is shared among all open handlers for the same table.
  The actual DuckDB database instance is managed by the global registry
  in ha_duckdb.cc, keyed by database file path.
*/
class DuckDB_share : public Handler_share {
public:
  mysql_mutex_t mutex;
  THR_LOCK lock;
  std::string db_file_path;    // path to #duckdb/global.duckdb

  DuckDB_share();
  ~DuckDB_share();
};

/**
  @brief
  Storage engine handler for DuckDB
*/
class ha_duckdb: public handler
{
  THR_LOCK_DATA lock;
  DuckDB_share *share;
  duckdb::Connection* connection;

  // Scan state
  duckdb::MaterializedQueryResult* scan_result;
  size_t scan_row;

  // Bulk insert state — Appender kept open across write_row() calls
  // Stored as void* to avoid needing full duckdb.hpp in the header
  void* bulk_appender;

  // Per-open metadata (public so create_duckdb_select_handler can read them)
public:
  std::string db_file_path;
  std::string db_name;
  std::string duckdb_table_name;    // schema-qualified: db.table
private:

  DuckDB_share *get_share();
  int convert_row_from_duckdb(uchar *buf, size_t row_idx,
                               duckdb::MaterializedQueryResult *result);
  std::string build_create_sql(TABLE *table_arg);

public:
  ha_duckdb(handlerton *hton, TABLE_SHARE *table_arg);
  ~ha_duckdb();

  const char *table_type() const { return "DUCKDB"; }
  const char *index_type(uint inx) { return "NONE"; }

  const char **bas_ext() const;

  int open(const char *name, int mode, uint test_if_locked);
  int close(void);
  int create(const char *name, TABLE *table_arg, HA_CREATE_INFO *create_info);
  int delete_table(const char *name);
  int rename_table(const char *from, const char *to);

  void start_bulk_insert(ha_rows rows, uint flags);
  int  end_bulk_insert();

  int write_row(const uchar *buf);
  int update_row(const uchar *old_data, const uchar *new_data);
  int delete_row(const uchar *buf);

  int rnd_init(bool scan);
  int rnd_end();
  int rnd_next(uchar *buf);
  int rnd_pos(uchar *buf, uchar *pos);
  void position(const uchar *record);

  THR_LOCK_DATA **store_lock(THD *thd, THR_LOCK_DATA **to,
                             enum thr_lock_type lock_type);

  int info(uint flag);
  int analyze(THD *thd, HA_CHECK_OPT *check_opt);
  ha_rows records_in_range(uint inx, const key_range *min_key,
                           const key_range *max_key, page_range *res);

  ulonglong table_flags() const
  {
    return (HA_REC_NOT_IN_SEQ | HA_NO_BLOBS | HA_BINLOG_STMT_CAPABLE |
            HA_NULL_IN_KEY);
  }

  ulong index_flags(uint inx, uint part, bool all_parts) const
  { return HA_NULL_IN_KEY; }

  uint max_supported_record_length() const { return HA_MAX_REC_LENGTH; }
  uint max_supported_keys()          const { return 64; }
  uint max_supported_key_parts()     const { return 16; }
  uint max_supported_key_length()    const { return 3072; }

  IO_AND_CPU_COST scan_time()
  {
    IO_AND_CPU_COST cost;
    cost.io= ulonglong2double(stats.data_file_length) / IO_SIZE + 2;
    cost.cpu= 0;
    return cost;
  }
};

/**
  @brief
  Pushdown handler — intercepts a full SELECT and runs it in DuckDB,
  bypassing MariaDB's row-by-row rnd_next() loop entirely.
*/
class ha_duckdb_select_handler : public select_handler
{
  duckdb::Connection *connection;
  duckdb::MaterializedQueryResult *result;
  size_t current_row;

public:
  ha_duckdb_select_handler(THD *thd, SELECT_LEX *sel,
                           duckdb::Connection *conn);
  ~ha_duckdb_select_handler();

  int init_scan()  override;
  int next_row()   override;
  int end_scan()   override;
};

#endif /* HA_DUCKDB_INCLUDED */
