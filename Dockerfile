# ============================================================
# Stage 1: Compile the C++ application
# ============================================================
FROM registry.access.redhat.com/hi/core-runtime:latest-builder AS builder

USER 0
RUN dnf install -y gcc-c++ make libcurl-devel
WORKDIR /build
COPY main.cpp .
RUN g++ -O2 \
    -fstack-protector-strong \
    -D_FORTIFY_SOURCE=3 \
    -fPIE -pie \
    -Wl,-z,relro,-z,now \
    -o /app main.cpp -lcurl && \
    strip --strip-all /app

# ============================================================
# Stage 2: Install runtime shared libraries into the
#           hardened core-runtime image using --installroot
# ============================================================
FROM registry.access.redhat.com/hi/core-runtime:latest

USER 0
# Bind-mount the builder stage to get access to dnf, then
# install only the runtime libs (not -devel headers) into
# the current image's root filesystem.
RUN --mount=type=bind,from=builder,target=/builder \
    export LD_LIBRARY_PATH=/builder/lib64:/builder/usr/lib64 \
        RPM_CONFIGDIR=/builder/usr/lib/rpm \
    && /builder/usr/bin/dnf install -y \
        --installroot=/ \
        --setopt=reposdir=/builder/etc/yum.repos.d \
        --setopt=install_weak_deps=False \
        --setopt=tsflags=nodocs \
        libcurl openssl ca-certificates-bundle \
    && /builder/usr/bin/dnf --installroot=/ clean all \
    && rm -rf /var/cache/dnf /var/cache/yum /var/log/dnf* /var/log/hawkey.log

# Copy the compiled binary from the build stage
COPY --from=builder /app /app

USER 65532
ENTRYPOINT ["/app"]
