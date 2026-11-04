# rhhi-cplusplus — bind-mount shared-library example

Canonical bind-mount pattern for adding shared libraries to a hardened
runtime image without a traditional multi-stage `COPY` of system files.

## How it works

- Stage 1 (`-builder`) compiles `main.cpp` with `libcurl-devel`.
- Stage 2 (`core-runtime`) bind-mounts the builder filesystem
  at `/builder` for a single `RUN` step and uses the builder's `dnf` with
  `--installroot=/` to install only runtime libs (`libcurl`, `openssl`,
  `ca-certificates-bundle`) into the runtime rootfs. The mount is never
  committed to a layer.

## Build (tagged by git commit)

```sh
TAG=$(git rev-parse --short HEAD)
podman build -t quay.io/jwesterl/rhhi-cplusplus:$TAG -t quay.io/jwesterl/rhhi-cplusplus:latest .
```

With Docker (requires BuildKit for the `--mount=type=bind,from=...` step):

```sh
DOCKER_BUILDKIT=1 docker build -t rhhi-cplusplus .
```

## Run

```sh
podman run --rm rhhi-cplusplus
podman run --rm rhhi-cplusplus https://example.com
```

## SBOM

Because the runtime libraries are installed via `dnf --installroot=/`,
the image carries a real RPM database — so SBOM generators see the
actual packages (this is what breaks if you `COPY` bare `.so` files or
statically link instead).

### Native podman flow (primary)

```sh
TAG=$(git rev-parse --short HEAD)
podman build --sbom=syft-spdx --sbom-output sbom.spdx.json \
  -t quay.io/jwesterl/rhhi-cplusplus:$TAG \
  -t quay.io/jwesterl/rhhi-cplusplus:latest .
podman push quay.io/jwesterl/rhhi-cplusplus:$TAG
podman push quay.io/jwesterl/rhhi-cplusplus:latest
```

The committed `sbom.spdx.json` (62 packages, SPDX 2.3) was produced this
way — it lists `libcurl 8.23.0`, `openssl 3.5.9`,
`ca-certificates-bundle`, etc. with `rpm` as the package type. Available
presets: `syft-spdx`, `syft`/`syft-cyclonedx`, `trivy`/`trivy-spdx`.

> Limitation, verified 2026-11-04: `podman push` does **not** attach the
> SBOM as an OCI referrer — the referrers list for the pushed digest is
> empty (`GET /v2/jwesterl/rhhi-cplusplus/referrers/<digest>` →
> `"manifests": []`), and `podman push` has no referrer/attestation
> flags. The SBOM is a build-time file; Quay shows it via its own
> scanning, not as an attached artifact. Attaching it as a referrer
> needs `oras push` or `cosign attest` (neither installed here).

### syft flow (alternative, works with Docker too)

```sh
podman save -o /tmp/image.tar rhhi-cplusplus:latest
syft scan docker-archive:/tmp/image.tar -o spdx-json=sbom.spdx.json

# or straight from the registry after pushing
syft scan registry:quay.io/jwesterl/rhhi-cplusplus:latest -o spdx-json=sbom.spdx.json
syft scan registry:quay.io/jwesterl/rhhi-cplusplus:latest -o table | grep -E 'libcurl|openssl'
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
