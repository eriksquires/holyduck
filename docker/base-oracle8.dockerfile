# syntax=docker/dockerfile:1.7
# Oracle Linux 8 base image — MariaDB 11.8.3 + build environment for ha_duckdb.so
# Mirrors the structure of base-ubuntu.dockerfile for the Oracle/RHEL ecosystem.
FROM oraclelinux:8

# Set environment variables
ENV MYSQL_ROOT_PASSWORD=testpass
ENV MARIADB_VERSION=11.8.3

# Enable EPEL and CodeReady Builder (needed for lz4-devel and a few others)
RUN dnf install -y oracle-epel-release-el8 && \
    dnf config-manager --enable ol8_codeready_builder && \
    dnf update -y

# Install build tools and development dependencies
RUN --mount=type=cache,target=/var/cache/dnf \
    dnf install -y \
    wget \
    curl \
    vim \
    git \
    cmake \
    gcc \
    gcc-c++ \
    make \
    bison \
    ncurses-devel \
    readline-devel \
    openssl-devel \
    libxml2-devel \
    libaio-devel \
    libevent-devel \
    python3-devel \
    boost-devel \
    lz4-devel \
    zlib-devel \
    pkgconf

# Add MariaDB 11.8.3 repository
RUN curl -LsS https://r.mariadb.com/downloads/mariadb_repo_setup | bash -s -- --mariadb-server-version="mariadb-${MARIADB_VERSION}" --skip-maxscale

# Install MariaDB
RUN --mount=type=cache,target=/var/cache/dnf \
    dnf install -y \
    MariaDB-server \
    MariaDB-client \
    MariaDB-devel \
    MariaDB-shared

# Create run directory for mysqld
RUN mkdir -p /run/mysqld && chown mysql:mysql /run/mysqld

# Configure MariaDB
RUN mkdir -p /etc/my.cnf.d && \
    echo "[mysqld]" > /etc/my.cnf.d/50-server.cnf && \
    echo "bind-address = 0.0.0.0" >> /etc/my.cnf.d/50-server.cnf && \
    echo "port = 3306" >> /etc/my.cnf.d/50-server.cnf && \
    echo "innodb_buffer_pool_size = 512M" >> /etc/my.cnf.d/50-server.cnf && \
    echo "max_connections = 100" >> /etc/my.cnf.d/50-server.cnf

# Install DuckDB CLI
RUN curl https://install.duckdb.org | sh \
 && ln -s /root/.duckdb/cli/latest/duckdb /usr/local/bin/duckdb

# Create workspace directories
WORKDIR /workspace
RUN mkdir -p /plugin-src /mariadb-src /build

# Create startup script for MariaDB
# Note: uses mysqld_safe directly — systemd is not available in containers
RUN echo '#!/bin/bash' > /usr/local/bin/start-mariadb.sh \
&& echo 'set -e' >> /usr/local/bin/start-mariadb.sh \
&& echo 'chown -R mysql:mysql /var/lib/mysql' >> /usr/local/bin/start-mariadb.sh \
&& echo 'if [ ! -d /var/lib/mysql/mysql ]; then' >> /usr/local/bin/start-mariadb.sh \
&& echo '  echo "Initializing MariaDB data directory..."' >> /usr/local/bin/start-mariadb.sh \
&& echo '  mysql_install_db --user=mysql --datadir=/var/lib/mysql' >> /usr/local/bin/start-mariadb.sh \
&& echo 'fi' >> /usr/local/bin/start-mariadb.sh \
&& echo 'echo "Starting MariaDB..."' >> /usr/local/bin/start-mariadb.sh \
&& echo 'mysqld_safe --user=mysql &' >> /usr/local/bin/start-mariadb.sh \
&& echo 'echo "Waiting for MariaDB to be ready..."' >> /usr/local/bin/start-mariadb.sh \
&& echo 'while ! mysqladmin -h 127.0.0.1 ping --silent 2>/dev/null; do sleep 1; done' >> /usr/local/bin/start-mariadb.sh \
&& echo 'echo "Setting root password..."' >> /usr/local/bin/start-mariadb.sh \
&& echo 'mariadb -u root -e "ALTER USER '"'"'root'"'"'@'"'"'localhost'"'"' IDENTIFIED BY '"'"'${MYSQL_ROOT_PASSWORD}'"'"'; FLUSH PRIVILEGES;" 2>/dev/null || true' >> /usr/local/bin/start-mariadb.sh \
&& echo 'echo "MariaDB is ready!"' >> /usr/local/bin/start-mariadb.sh \
&& chmod +x /usr/local/bin/start-mariadb.sh

# Expose MariaDB port
EXPOSE 3306

# Default command
CMD ["/bin/bash", "-c", "/usr/local/bin/start-mariadb.sh && /bin/bash"]
