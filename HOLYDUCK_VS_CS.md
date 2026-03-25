## HolyDuck vs ColumnStore

MariaDB's ColumnStore is the official enterprise-grade MariaDB analytical storage engine in this space. Here's how our humble contribution compares:

|                    | HolyDuck                          | ColumnStore                      |
| ------------------ | --------------------------------- | -------------------------------- |
| Architecture       | In-process — runs inside mysqld   | Separate process / service       |
| Concurrent writers | Single writer (DuckDB limitation) | Parallel ingestion               |
| High availability  | None — local file only            | MariaDB replication + clustering |
| Multi-server       | Single node                       | Distributed across nodes         |
| Deployment         | Drop in a `.so` file              | Full cluster infrastructure      |
| Setup complexity   | Minutes                           | Hours to days                    |

**HolyDuck's sweet spot:** single-node analytics alongside InnoDB. Big overnight ETL jobs, exploratory data analysis during the day, million-to-billion row scans that return small aggregated results — all without standing up any infrastructure. HolyDuck tables are native DuckDB tables that live and grow in a DuckDB database — no external connections or translations occur.

**ColumnStore's sweet spot:** when you've outgrown a single node and need HA, replication, and multi-server distribution.

It should go without saying but HolyDuck has not used any resources from ColumnStore.
