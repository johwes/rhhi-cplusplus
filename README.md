# rhhi-cplusplus — bind-mount shared-library example

Canonical bind-mount pattern for adding shared libraries to a hardened
runtime image without a traditional multi-stage `COPY` of system files.

## How it works

- Stage 1 (`-builder`) compiles `main.cpp` with `libcurl-devel`.
- Stage 2 (`core-runtime`, no shell/dnf) bind-mounts the builder filesystem
  at `/builder` for a single `RUN` step and uses the builder's `dnf` with
  `--installroot=/` to install only runtime libs (`libcurl`, `openssl`,
  `ca-certificates`) into the runtime rootfs. The mount is never committed
  to a layer.

## Build (requires BuildKit)

```sh
DOCKER_BUILDKIT=1 docker build -t rhhi-cplusplus .
```

## Run

```sh
docker run --rm rhhi-cplusplus
docker run --rm rhhi-cplusplus https://example.com
```

## Notes (verified with podman against `core-runtime:latest`)

- This base's `dnf` rejects `--nodocs`; the Dockerfile uses
  `--setopt=tsflags=nodocs` instead.
- Both `core-runtime` variants default to `USER 65532`, so the runtime
  stage switches to `USER 0` for the `dnf --installroot=/` step and back
  to `USER 65532` for the final image.
- `LD_LIBRARY_PATH`/`RPM_CONFIGDIR` are `export`ed: a `VAR=x cmd1 && cmd2`
  prefix only applies to `cmd1`, which broke the chained `dnf clean`.
- The base already ships `ca-certificates-bundle`, which conflicts with a
  plain `ca-certificates` install — so the Dockerfile installs
  `ca-certificates-bundle`.
- Verified: app returns `HTTP status: 200`, `libcurl.so.4`/`libssl.so.3`
  present, RPM DB updated at `/usr/lib/sysimage/rpm/rpmdb.sqlite`, and no
  `dnf`/`rpm` binaries in the final image.
