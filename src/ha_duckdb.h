#ifndef HA_DUCKDB_INCLUDED
#define HA_DUCKDB_INCLUDED

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

#define HOLYDUCK_VERSION "0.4.2"

#ifdef USE_PRAGMA_INTERFACE
#pragma interface
#endif

#include "my_global.h"
#include "thr_lock.h"
#include "handler.h"
#include "my_base.h"
#include "select_handler.h"
#include "derived_handler.h"
#include <memory>
#include <string>
#include <vector>

// Forward declare DuckDB types
namespace duckdb {
    class Connection;
    class DuckDB;
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

  // Scan state — streaming chunk-based iteration
  // Stored as void* to avoid needing full duckdb.hpp in the header
  void* scan_result;     // duckdb::QueryResult* (streaming)
  void* scan_chunk;      // duckdb::DataChunk* (current chunk, owned)
  size_t scan_chunk_row;

  // Bulk insert state — Appender kept open across write_row() calls
  // Stored as void* to avoid needing full duckdb.hpp in the header
  void* bulk_appender;

  // Condition pushdown — set by cond_push(), used in rnd_init() and direct DML
  std::string pushed_where;

  // Column subset for scans — maps DuckDB result column index → MariaDB
  // field index.  Empty means SELECT * (all fields, sequential mapping).
  std::vector<uint> scan_field_map;

  // Direct UPDATE state — field/value lists captured via info_push()
  List<Item> *update_fields;
  List<Item> *update_values;

  // Per-open metadata (public so create_duckdb_select_handler can read them)
public:
  std::string db_file_path;
  std::string db_name;
  std::string duckdb_table_name;    // schema-qualified: db.table
private:

  DuckDB_share *get_share();
  int convert_row_from_duckdb(uchar *buf, size_t row_idx, void *chunk_ptr);
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
  int truncate() override;

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

  const COND *cond_push(const COND *cond) override;
  void        cond_pop()                  override;

  int info_push(uint info_type, void *info) override;

  int direct_update_rows_init(List<Item> *update_fields) override;
  int direct_update_rows(ha_rows *update_rows, ha_rows *found_rows) override;

  int direct_delete_rows_init() override;
  int direct_delete_rows(ha_rows *delete_rows) override;

  enum_alter_inplace_result
  check_if_supported_inplace_alter(TABLE *altered_table,
                                   Alter_inplace_info *ha_alter_info) override;
  bool inplace_alter_table(TABLE *altered_table,
                           Alter_inplace_info *ha_alter_info) override;
  bool commit_inplace_alter_table(TABLE *altered_table,
                                  Alter_inplace_info *ha_alter_info,
                                  bool commit) override;

  ulonglong table_flags() const
  {
    return (HA_REC_NOT_IN_SEQ | HA_NO_BLOBS | HA_BINLOG_STMT_CAPABLE |
            HA_NULL_IN_KEY | HA_CAN_TABLE_CONDITION_PUSHDOWN |
            HA_CAN_DIRECT_UPDATE_AND_DELETE);
  }

  ulong index_flags(uint inx, uint part, bool all_parts) const
  {
    // Intentionally returns no read-scan flags (HA_READ_NEXT etc.).
    //
    // DuckDB indexes are used internally by DuckDB's own query planner when
    // a query is pushed down via ha_duckdb_select_handler.  Exposing index
    // scan capability to MariaDB (by returning HA_READ_NEXT etc.) would allow
    // the optimizer to drive cross-engine joins by range-scanning the DuckDB
    // table row-by-row through the handler API — one call per row, serialised,
    // bypassing DuckDB's vectorised execution and parallelism entirely.
    //
    // Keeping this empty forces MariaDB to either push the whole query to
    // DuckDB (best case) or do a single batched full scan (acceptable), and
    // prevents it from choosing a naïve row-at-a-time index scan path.
    return HA_NULL_IN_KEY;
  }

  uint max_supported_record_length() const { return HA_MAX_REC_LENGTH; }
  // Returning 0 here tells MariaDB this engine has no indexes, which prevents
  // it from planning ref or range joins against DuckDB tables.  Without this,
  // the optimizer would plan a ref join (type=ref) and then fail at runtime
  // when index_read_map() is called — we return HA_ERR_WRONG_COMMAND because
  // row-at-a-time index scans bypass DuckDB's vectorised execution entirely.
  //
  // DuckDB indexes are still created internally (see create()) and used by
  // DuckDB's own query planner on pushed-down queries.  MariaDB simply never
  // learns they exist, so it cannot misuse them.
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
  void *duck_result;    // duckdb::QueryResult* (streaming)
  void *duck_chunk;     // duckdb::DataChunk* (current chunk, owned)
  size_t chunk_row;
  bool use_original_sql;   // true when all leaf tables are DUCKDB engine

public:
  ha_duckdb_select_handler(THD *thd, SELECT_LEX *sel,
                           duckdb::Connection *conn,
                           bool use_original_sql);
  ha_duckdb_select_handler(THD *thd, SELECT_LEX_UNIT *unit,
                           duckdb::Connection *conn,
                           bool use_original_sql);
  ~ha_duckdb_select_handler();

  int init_scan()  override;
  int next_row()   override;
  int end_scan()   override;
};

/**
  @brief
  Pushdown handler — intercepts a derived table (subquery in FROM clause, or CTE)
  whose leaf tables are all DUCKDB engine, and runs it entirely inside DuckDB.
  MariaDB sees a small result set instead of scanning every raw row.
*/
class ha_duckdb_derived_handler : public derived_handler
{
  duckdb::Connection *connection;
  void *duck_result;    // duckdb::QueryResult* (streaming)
  void *duck_chunk;     // duckdb::DataChunk* (current chunk, owned)
  size_t chunk_row;

public:
  ha_duckdb_derived_handler(THD *thd, TABLE_LIST *tbl, duckdb::Connection *conn);
  ~ha_duckdb_derived_handler();

  int init_scan() override;
  int next_row()  override;
  int end_scan()  override;
};

#endif /* HA_DUCKDB_INCLUDED */
