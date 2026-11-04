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
> needs `oras push` or `cosign attest` (see next section).

### syft flow (alternative, works with Docker too)

```sh
podman save -o /tmp/image.tar rhhi-cplusplus:latest
syft scan docker-archive:/tmp/image.tar -o spdx-json=sbom.spdx.json

# or straight from the registry after pushing
syft scan registry:quay.io/jwesterl/rhhi-cplusplus:latest -o spdx-json=sbom.spdx.json
syft scan registry:quay.io/jwesterl/rhhi-cplusplus:latest -o table | grep -E 'libcurl|openssl'
```

## Signing & attestations (cosign, keyless OIDC)

cosign (`~/.local/bin`, v3.1.4 — no root needed) signs the image and
attaches the SBOM and vulnerability scan as DSSE attestations (OCI
referrers on the image digest). No long-lived keys: signing uses your
OIDC identity via Fulcio, recorded in Rekor. These steps are
interactive once (browser OIDC login):

```sh
TAG=$(git rev-parse --short HEAD)
IMG=quay.io/jwesterl/rhhi-cplusplus:$TAG

# sign the image
cosign sign --yes $IMG

# attach the SBOM
cosign attest --predicate sbom.spdx.json --type spdx --yes $IMG

# attach the vulnerability scan (grype JSON, committed as vuln.grype.json)
grype registry:$IMG -o json=vuln.grype.json
cosign attest --predicate vuln.grype.json --type vuln --yes $IMG
```

Verify (pin the identity/issuer shown at signing time):

```sh
cosign verify $IMG \
  --certificate-identity <you@example.com> \
  --certificate-oidc-issuer https://accounts.google.com
cosign verify-attestation --type spdx $IMG \
  --certificate-identity <you@example.com> \
  --certificate-oidc-issuer https://accounts.google.com
```

The committed `vuln.grype.json` is a point-in-time sample: 12 matches
(4 High, 6 Medium, 2 Low) — even hardened-pipeline libs carry CVEs,
which is precisely why the RPM-visible dynamic approach matters.

## Notes (verified with podman against `core-runtime:latest`)

- This base's `dnf` rejects `--nodocs`; the Dockerfile uses
  `--setopt=tsflags=nodocs` instead.
- Both `core-runtime` variants default to `USER 65532`, so the runtime
  stage switches to `USER 0` for the `dnf --installroot=/` step and back
  to `USER 65532` for the final image.
- `LD_LIBRARY_PATH`/`RPM_CONFIGDIR` are `export`ed: a `VAR=x cmd1 && cmd2`
  prefix only applies to `cmd1`, which broke the chained `dnf clean`.
- The builder is bind-mounted via its stage name (`from=builder`), so the
  image reference stays DRY — verified to work with podman/buildah.
- The runtime `rm -rf /var/cache/dnf /var/cache/yum /var/log/dnf* ...`
  removes `dnf5.log` (~78 KB) that `dnf clean all` leaves behind.
- Binary hardening (verified with `readelf`): `DYN` (PIE), `BIND_NOW`
  (full RELRO), symbol table stripped (18.7 KB → 15.0 KB).
  `-fstack-protector-strong` and `-D_FORTIFY_SOURCE=3` are compiled in;
  this tiny program has no stack buffers so no canary is emitted
  (protector instruments only functions with addressable stack objects).
- Watch for RPM scriptlets (`%pre`/`%post`): `dnf --installroot=/` runs
  them, and they need a shell/utilities the runtime may lack. The
  packages used here have lightweight scriptlets that succeed, but if a
  future package fails, escape with `--setopt=tsflags=noscripts` and
  perform any required initialization manually.
- The base already ships `ca-certificates-bundle`, which conflicts with a
  plain `ca-certificates` install — so the Dockerfile installs
  `ca-certificates-bundle`.
- Verified: app returns `HTTP status: 200`, `libcurl.so.4`/`libssl.so.3`
  present, RPM DB updated at `/usr/lib/sysimage/rpm/rpmdb.sqlite`, and no
  `dnf`/`rpm` binaries in the final image.
