# syntax=docker/dockerfile:1
FROM mariadb-duckdb-base:ubuntu

# Install DuckDB library system-wide for runtime
COPY lib/libduckdb.so /usr/local/lib/
RUN ldconfig

# Create expected library path structure for build
RUN mkdir -p /plugin-src && ln -s /plugin-src/lib/libduckdb.so /libduckdb.so

# Set up build environment
WORKDIR /workspace
ENV PKG_CONFIG_PATH=/usr/lib/pkgconfig:/usr/local/lib/pkgconfig
ENV LD_LIBRARY_PATH="/usr/local/lib:${LD_LIBRARY_PATH:-}"

# Create build script that will be run when container starts
RUN echo '#!/bin/bash' > /usr/local/bin/build-plugin.sh && \
    echo 'set -e' >> /usr/local/bin/build-plugin.sh && \
    echo 'cd /plugin-src/src' >> /usr/local/bin/build-plugin.sh && \
    echo 'mkdir -p build' >> /usr/local/bin/build-plugin.sh && \
    echo 'cd build' >> /usr/local/bin/build-plugin.sh && \
    echo 'cmake .. -DCMAKE_BUILD_TYPE=Release' >> /usr/local/bin/build-plugin.sh && \
    echo 'make' >> /usr/local/bin/build-plugin.sh && \
    echo 'cp ha_duckdb.so /usr/lib/mysql/plugin/' >> /usr/local/bin/build-plugin.sh && \
    chmod +x /usr/local/bin/build-plugin.sh

# Default command
CMD ["/bin/bash", "-c", "/usr/local/bin/start-mariadb.sh && /bin/bash"]