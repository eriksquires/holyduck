#ifndef HA_DUCKDB_INCLUDED
#define HA_DUCKDB_INCLUDED

/*
   Copyright (c) 2026, DuckDB Storage Engine for MariaDB

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA
*/

#ifdef USE_PRAGMA_INTERFACE
#pragma interface
#endif

#include "my_global.h"
#include "thr_lock.h"
#include "handler.h"
#include "my_base.h"
#include <string>

// Forward declare DuckDB types to avoid including headers here
namespace duckdb {
    class Connection;
    class DuckDB;
    class MaterializedQueryResult;
}

/**
  @brief
  DuckDB_share is shared among all open handlers for the same table.
  It owns the DuckDB database instance (and therefore the on-disk file).
*/
class DuckDB_share : public Handler_share {
public:
  mysql_mutex_t mutex;
  THR_LOCK lock;
  duckdb::DuckDB* db_instance;

  DuckDB_share();
  ~DuckDB_share();

  int init_database(const char *path);
  void cleanup_database();
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

  // Per-open metadata
  std::string db_path;
  std::string duckdb_table_name;

  DuckDB_share *get_share();
  std::string build_create_sql(TABLE *table_arg);
  int convert_row_from_duckdb(uchar *buf, size_t row_idx);

public:
  ha_duckdb(handlerton *hton, TABLE_SHARE *table_arg);
  ~ha_duckdb();

  const char *table_type() const { return "DUCKDB"; }
  const char *index_type(uint inx) { return "NONE"; }

  const char **bas_ext() const;

  // Table operations
  int open(const char *name, int mode, uint test_if_locked);
  int close(void);
  int create(const char *name, TABLE *table_arg, HA_CREATE_INFO *create_info);
  int delete_table(const char *name);
  int rename_table(const char *from, const char *to);

  // Row operations
  int write_row(const uchar *buf);
  int update_row(const uchar *old_data, const uchar *new_data);
  int delete_row(const uchar *buf);

  // Scan operations
  int rnd_init(bool scan);
  int rnd_end();
  int rnd_next(uchar *buf);
  int rnd_pos(uchar *buf, uchar *pos);
  void position(const uchar *record);

  // Lock management
  THR_LOCK_DATA **store_lock(THD *thd, THR_LOCK_DATA **to,
                             enum thr_lock_type lock_type);

  // Statistics and info
  int info(uint flag);
  ha_rows records_in_range(uint inx, const key_range *min_key,
                           const key_range *max_key, page_range *res);

  ulonglong table_flags() const
  {
    return (HA_REC_NOT_IN_SEQ | HA_NO_BLOBS | HA_BINLOG_STMT_CAPABLE);
  }

  ulong index_flags(uint inx, uint part, bool all_parts) const
  {
    return 0;
  }

  uint max_supported_record_length() const { return HA_MAX_REC_LENGTH; }
  uint max_supported_keys()          const { return 0; }
  uint max_supported_key_parts()     const { return 0; }
  uint max_supported_key_length()    const { return 0; }

  IO_AND_CPU_COST scan_time()
  {
    IO_AND_CPU_COST cost;
    cost.io= ulonglong2double(stats.data_file_length) / IO_SIZE + 2;
    cost.cpu= 0;
    return cost;
  }
};

#endif /* HA_DUCKDB_INCLUDED */
