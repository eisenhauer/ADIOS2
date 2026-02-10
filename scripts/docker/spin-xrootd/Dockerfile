FROM fedora:latest

# Install development tools
RUN dnf update -y && \
    dnf upgrade -y && \
    dnf group -y install c-development development-tools

# Install XRootD and dependencies
# Note: xrootd-* installs all XRootD components including HTTP plugin
RUN dnf install -y cmake git 'xrootd-*' libcurl-devel openssl && \
    dnf clean all && \
    rm -rf /var/cache/dnf

# Clone and build ADIOS2 with XRootD and CURL support from GitHub
ARG ADIOS2_REPO=https://github.com/ornladios/ADIOS2.git
ARG ADIOS2_BRANCH=master

# Build, install, create test data, then remove source entirely
RUN git clone --depth 1 --branch ${ADIOS2_BRANCH} ${ADIOS2_REPO} /ADIOS2-src && \
    mkdir -p /ADIOS2-src/build && \
    cd /ADIOS2-src/build && \
    cmake \
        -DADIOS2_USE_XRootD=ON \
        -DADIOS2_USE_CURL=ON \
        -DADIOS2_USE_MPI=OFF \
        -DADIOS2_USE_Fortran=OFF \
        -DADIOS2_USE_Python=OFF \
        -DADIOS2_USE_SST=OFF \
        -DBUILD_TESTING=ON \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX=/usr/local \
        .. && \
    make -j$(nproc) && \
    make install && \
    mkdir -p /data && \
    cd /data && \
    /ADIOS2-src/build/bin/TestCommonWrite bp5 sample && \
    rm -rf /ADIOS2-src

# Add KSTAR campaign test data (100 runs, ~56MB compressed)
COPY kstar-data.tar /tmp/kstar-data.tar
RUN cd /data && tar xf /tmp/kstar-data.tar && rm /tmp/kstar-data.tar

# --- Everything below here changes frequently, keep after compile ---

# Copy configuration files (after compile so changes don't bust cache)
COPY xrootd-http.cfg /etc/xrootd/xrootd-http.cfg
COPY docker-entrypoint.sh /docker-entrypoint.sh
RUN chmod +x /docker-entrypoint.sh

# Create directories writable by any UID (Spin runs as NERSC user)
RUN mkdir -p /tmp/adios /var/log/xrootd && \
    chmod 777 /tmp/adios /var/log/xrootd /data

# Expose HTTP port (Spin Ingress will terminate TLS and forward to this port)
EXPOSE 8080

# Health check - verify XRootD HTTP is responding
HEALTHCHECK --interval=30s --timeout=10s --start-period=10s --retries=3 \
    CMD curl -sf -o /dev/null http://localhost:8080/ || exit 1

ENTRYPOINT ["/docker-entrypoint.sh"]
