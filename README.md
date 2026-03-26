# HolyDuck — DuckDB Analytics Engine for MariaDB

*Bringing divine analytical powers to Maria.*

**HolyDuck** is a MariaDB storage engine plugin that embeds [DuckDB](https://duckdb.org/) as a first-class DB storage engine next to InnoDB. Create tables with `ENGINE=DUCKDB` and query them with standard SQL — DuckDB handles the heavy lifting.

## What's Unique
HolyDuck enables true mixed-engine table joins by shifting execution into DuckDB while dynamically sourcing data from both DuckDB and InnoDB tables. Predicates are applied at the appropriate layer to minimize data movement and avoid full table scans.

The full TPC-H test suite runs successfully with tables split across InnoDB and DuckDB, or entirely in DuckDB, with consistently strong performance.

## Why

MariaDB is beloved. DuckDB is a miracle of analytical performance and small teams needed a fast OLAP oriented data engine. HolyDuck is that blessing — it embeds DuckDB directly inside MariaDB so you can:

- Run analytical queries (GROUP BY, aggregations, window functions) inside DuckDB at DuckDB speed
- Mix DuckDB and InnoDB tables in the same query without moving tables unnecessarily

HolyDuck is an extremely easy to install, easy to use OLAP engine that lives inside your existing MariaDB data infrastructure.  The speed of a parallel column store database with the convenience of MariaDB with incredibly simple installation. An ideal solution for small teams living in a MariaDB ecosystem.

We are particularly proud of the performance of mixed-engine joins.  The pain point for any mixed-engine database is joins on tables that come from multiple engines/storage systems.  With a little care in your SQL query and the favor of HolyDuck you can avoid the penalty of bringing large datasets out to MariaDB for row by row joining.

The ideal user of HolyDuck is a small team with a handful of very large tables that get a lot of reads during the day and are updated nightly.  

## DuckDB Limitations

HoldyDuck is much more team friendly in a sense than DuckDB alone but it's a gift, not magic. 

Alone DuckDB does not handle more than one write connection at a time.  HolyDuck does allows for multiple concurrent users but the entire engine blocks per write operation.  That is, you and your colleagues can open and access the host MariaDB instance and query all the tables you normally would inside and outside of DuckDB but any DuckDB change operations will block all other DuckDB writers.   This naturally promotes a pattern of using DuckDB for scanning very big tables to create smaller InnoDB which need more frequent edits/refactoring.  

Another important limitation is that MariaDB enforces it's SQL language idioms.  You can't run anything against DuckDB that MariaDB won't allow. Put on your pirate boots though, we have ways around that.  

## Additional resources
- [INSTALLATION.md](INSTALLATION.md)
- [BUILDING.md](BUILDING.md)
- [SQL Guidelines](WRITING_SQL.md)
- Comparison Guides:
  - [ColumnStore](comparisons/HD_vs_CS.md)
  - [AliSQL](comparisons/HD_vs_ALISQL.md) 


## License

Copyright (c) 2026, Erik Squires. This project is dual-licensed:

- **[GPL v2](LICENSE)** — free to use under the terms of the GNU General Public License v2.
  Any derivative works must also be GPL v2.
- **[Commercial License](LICENSE-COMMERCIAL)** — for use in proprietary or closed-source
  products without GPL obligations. Contact via [GitHub](https://github.com/eriksquires).

## Third-Party Software

HolyDuck is built on the shoulders of two exceptional projects:

**[DuckDB](https://duckdb.org/)** — embedded analytical database engine.
Copyright 2018-2024 Stichting DuckDB Foundation. Licensed under the
[MIT License](https://github.com/duckdb/duckdb/blob/main/LICENSE).

**[MariaDB Server](https://mariadb.org/)** — relational database server and plugin API.
Copyright MariaDB Foundation and contributors. Licensed under the
[GNU General Public License v2](https://www.gnu.org/licenses/old-licenses/gpl-2.0.html).

Full license texts for all third-party components are in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

## Acknowledgements

This project is essentially a shim between two exceptional data stores — it could not exist without the hard work of the [DuckDB](https://duckdb.org/) and [MariaDB Foundation](https://mariadb.org/) teams and everyone who has contributed to both projects.
