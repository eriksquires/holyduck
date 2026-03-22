FROM ubuntu:22.04

# Set environment variables
ENV DEBIAN_FRONTEND=noninteractive
ENV MYSQL_ROOT_PASSWORD=testpass
ENV MARIADB_VERSION=11.8

# Install basic tools and dependencies
RUN apt-get update && apt-get install -y \
    wget \
    curl \
    vim \
    git \
    cmake \
    build-essential \
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
    systemd \
    pkg-config

# Add MariaDB repository and install
RUN curl -LsS https://r.mariadb.com/downloads/mariadb_repo_setup | bash -s -- --mariadb-server-version="mariadb-${MARIADB_VERSION}"
RUN apt-get update && apt-get install -y \
    mariadb-server \
    mariadb-client \
    libmariadb-dev \
    mariadb-plugin-connect

# Create workspace directories
WORKDIR /workspace
RUN mkdir -p /plugin-src /mariadb-src /build

# Copy MariaDB source for headers
COPY --from=mariadb-src /mariadb-src/ /mariadb-src/

# Create startup script
RUN echo '#!/bin/bash' > /usr/local/bin/start-dev.sh \
&& echo 'set -e' >> /usr/local/bin/start-dev.sh \
&& echo 'echo "Starting MariaDB..."' >> /usr/local/bin/start-dev.sh \
&& echo 'service mariadb start' >> /usr/local/bin/start-dev.sh \
&& echo 'echo "MariaDB started"' >> /usr/local/bin/start-dev.sh \
&& echo 'cd /plugin-src' >> /usr/local/bin/start-dev.sh \
&& echo 'exec /bin/bash' >> /usr/local/bin/start-dev.sh \
&& chmod +x /usr/local/bin/start-dev.sh

# Default command
CMD ["/usr/local/bin/start-dev.sh"]