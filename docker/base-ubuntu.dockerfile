# syntax=docker/dockerfile:1.7
# Ubuntu 22.04 base image — MariaDB 11.8.3 + build environment for ha_duckdb.so
FROM ubuntu:22.04

ENV DEBIAN_FRONTEND=noninteractive
ENV MYSQL_ROOT_PASSWORD=testpass
ENV MARIADB_VERSION=11.8.3

# Install build tools and development dependencies
RUN --mount=type=cache,target=/var/cache/apt \
    --mount=type=cache,target=/var/lib/apt \
    apt-get update && apt-get install -y \
    wget \
    curl \
    vim \
    git \
    cmake \
    build-essential \
    gdb \
    bison \
    libncurses-dev \
    libreadline-dev \
    libssl-dev \
    libxml2-dev \
    libaio-dev \
    libevent-dev \
    python3-dev \
    libboost-dev \
    liblz4-dev \
    zlib1g-dev \
    pkg-config

# Add MariaDB 11.8.3 repository
RUN curl -LsS https://r.mariadb.com/downloads/mariadb_repo_setup | bash -s -- --mariadb-server-version="mariadb-${MARIADB_VERSION}" --skip-maxscale
RUN rm -f /etc/apt/sources.list.d/mariadb-maxscale.list

# Install MariaDB
RUN --mount=type=cache,target=/var/cache/apt \
    --mount=type=cache,target=/var/lib/apt \
    apt-get update && apt-get install -y \
    mariadb-server \
    mariadb-client \
    libmariadb-dev \
    libmariadbd-dev

# Initialize MariaDB system tables
RUN mysql_install_db --user=mysql --datadir=/var/lib/mysql

# Create run directory for mysqld
RUN mkdir -p /run/mysqld && chown mysql:mysql /run/mysqld

# Configure MariaDB
RUN printf '[mysqld]\nbind-address = 0.0.0.0\nport = 3306\ninnodb_buffer_pool_size = 512M\nmax_connections = 100\ntmpdir = /var/tmp/mariadb\n' > /etc/my.cnf

# Install DuckDB CLI
RUN curl https://install.duckdb.org | sh \
 && ln -s /root/.duckdb/cli/latest/duckdb /usr/local/bin/duckdb

# Create workspace and temp directories
WORKDIR /workspace
RUN mkdir -p /plugin-src /mariadb-src /build /var/tmp/mariadb \
 && chown mysql:mysql /var/tmp/mariadb

# Create startup script for MariaDB
RUN echo '#!/bin/bash' > /usr/local/bin/start-mariadb.sh \
&& echo 'set -e' >> /usr/local/bin/start-mariadb.sh \
&& echo 'chown -R mysql:mysql /var/lib/mysql' >> /usr/local/bin/start-mariadb.sh \
&& echo 'if [ ! -d /var/lib/mysql/mysql ]; then' >> /usr/local/bin/start-mariadb.sh \
&& echo '  echo "Initializing MariaDB data directory..."' >> /usr/local/bin/start-mariadb.sh \
&& echo '  mysql_install_db --user=mysql --datadir=/var/lib/mysql' >> /usr/local/bin/start-mariadb.sh \
&& echo 'fi' >> /usr/local/bin/start-mariadb.sh \
&& echo 'echo "Starting MariaDB..."' >> /usr/local/bin/start-mariadb.sh \
&& echo 'service mariadb start' >> /usr/local/bin/start-mariadb.sh \
&& echo 'echo "Waiting for MariaDB to be ready..."' >> /usr/local/bin/start-mariadb.sh \
&& echo 'while ! mysqladmin ping --silent; do sleep 1; done' >> /usr/local/bin/start-mariadb.sh \
&& echo 'echo "Setting root password..."' >> /usr/local/bin/start-mariadb.sh \
&& echo 'mysqladmin -u root password "${MYSQL_ROOT_PASSWORD}" 2>/dev/null || true' >> /usr/local/bin/start-mariadb.sh \
&& echo "mariadb -uroot -p\"\${MYSQL_ROOT_PASSWORD}\" --ssl=0 -e \"GRANT ALL ON *.* TO 'root'@'%' IDENTIFIED BY '\${MYSQL_ROOT_PASSWORD}'; FLUSH PRIVILEGES;\" 2>/dev/null || true" >> /usr/local/bin/start-mariadb.sh \
&& echo 'echo "MariaDB is ready!"' >> /usr/local/bin/start-mariadb.sh \
&& chmod +x /usr/local/bin/start-mariadb.sh

# Expose MariaDB port
EXPOSE 3306

# Default command
CMD ["/bin/bash", "-c", "/usr/local/bin/start-mariadb.sh && /bin/bash"]
