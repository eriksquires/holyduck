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
RUN echo "[mysqld]" > /etc/mysql/mariadb.conf.d/50-server.cnf \
&& echo "bind-address = 0.0.0.0" >> /etc/mysql/mariadb.conf.d/50-server.cnf \
&& echo "port = 3306" >> /etc/mysql/mariadb.conf.d/50-server.cnf \
&& echo "innodb_buffer_pool_size = 512M" >> /etc/mysql/mariadb.conf.d/50-server.cnf \
&& echo "max_connections = 100" >> /etc/mysql/mariadb.conf.d/50-server.cnf

# Install DuckDB CLI
RUN curl https://install.duckdb.org | sh \
 && ln -s /root/.duckdb/cli/latest/duckdb /usr/local/bin/duckdb

# Create workspace directories
WORKDIR /workspace
RUN mkdir -p /plugin-src /mariadb-src /build

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
&& echo 'echo "MariaDB is ready!"' >> /usr/local/bin/start-mariadb.sh \
&& chmod +x /usr/local/bin/start-mariadb.sh

# Expose MariaDB port
EXPOSE 3306

# Default command
CMD ["/bin/bash", "-c", "/usr/local/bin/start-mariadb.sh && /bin/bash"]
