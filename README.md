# rhhi-cplusplus — bind-mount shared-library example

[![Supply Chain](https://github.com/johwes/rhhi-cplusplus/actions/workflows/supply-chain.yml/badge.svg)](https://github.com/johwes/rhhi-cplusplus/actions/workflows/supply-chain.yml)

Canonical bind-mount pattern for adding shared libraries to a hardened
runtime image without a traditional multi-stage `COPY` of system files.

## How it works

- Stage 1 (`-builder`) compiles `main.cpp` with `libcurl-devel`.
- Stage 2 (`core-runtime`) bind-mounts the builder filesystem
  at `/builder` for a single `RUN` step and uses the builder's `dnf` with
  `--installroot=/` to install only runtime libs (`libcurl`,
  `ca-certificates-bundle` — plus `openssl-libs`, pulled automatically
  via `libcurl`'s soname dependencies) into the runtime rootfs. The
  mount is never committed to a layer.

## Build (tagged by git commit)

```sh
TAG=$(git rev-parse --short HEAD)
podman build \
  --build-arg GIT_SHA=$TAG \
  --build-arg BUILD_DATE=$(date -u +%Y-%m-%dT%H:%M:%SZ) \
  -t quay.io/jwesterl/rhhi-cplusplus:$TAG \
  -t quay.io/jwesterl/rhhi-cplusplus:latest .
```

The build args stamp our own OCI labels (`created`, `revision`,
`name`, `title`, `source`) onto the final stage. Without them, the
image inherits the *base image's* labels (`org.opencontainers.image.
created`, `revision`, `name: hi/core-runtime`) — scanners doing age
assessment would date your image to the base build, not yours. The
`cpe: cpe:/a:redhat:hummingbird:1` label is intentionally inherited:
this image is still a hummingbird product derivative.

With Docker (requires BuildKit for the `--mount=type=bind,from=...` step):

```sh
DOCKER_BUILDKIT=1 docker build -t rhhi-cplusplus .
```

## Run

```sh
podman run --rm rhhi-cplusplus
podman run --rm rhhi-cplusplus https://example.com
```

## Dynamic linking notes (why not static?)

The binary is dynamically linked against glibc and the RPM-installed
libs. That is deliberate — static linking a C++ app in this ecosystem
has sharp edges:

- **glibc static is fragile.** A fully static glibc binary (`-static`)
  breaks NSS (`getaddrinfo`/DNS can misbehave) and `dlopen`. There is
  no musl in the Red Hat hardened pipeline, so "just use musl" means
  leaving the hardened ecosystem entirely.
- **Static libcurl drags a long tail.** You would need the `-static`
  variants of openssl, nghttp2/3, libssh, zlib, zstd, brotli, … and you
  own the CVE lifecycle for all of them from then on.
- **Scanners go blind.** A statically linked binary is opaque to Grype,
  Trivy, and OpenSCAP — the CVE surface is still there (it's inside
  your binary), just invisible. Dynamic RPM installs keep it visible
  (see SBOM below).

Known issues with the dynamic approach:

- **Builder/runtime glibc drift.** The binary is built against the
  builder's glibc and runs against the runtime's. Both stages currently
  track `latest`/`latest-builder`, which can drift apart — a binary
  built against a *newer* glibc than the runtime ships will fail to
  start. Pin both stages to the same tag/digest for anything beyond a
  demo.
- **Transitive closure.** `dnf install libcurl` pulls ~30 packages
  (krb5, openldap, nghttp2/3, … — see `sbom.spdx.json`). Adding one
  library can substantially grow the image; check `syft ... -o table`
  before and after.
- **Floating versions.** The `dnf install` line has no version pins, so
  rebuilds float with the repos. Committed SBOM/vuln samples will drift
  from what a fresh build produces. Pin versions
  (`libcurl-8.23.0-2.hum1`) when reproducibility matters.
- **No runtime `LD_LIBRARY_PATH` needed.** Libs land in standard paths
  and the RPM scriptlets rebuild the `ld.so` cache, so the binary
  resolves everything at startup with a clean environment.
- **Stripped binary, limited debuggability.** `strip --all` shrinks the
  image but removes symbols — keep an unstripped copy (or debuginfo) of
  every shipped binary if you ever need to read a core dump.

## SBOM

Because the runtime libraries are installed via `dnf --installroot=/`,
the image carries a real RPM database — so SBOM generators see the
actual packages (this is what breaks if you `COPY` bare `.so` files or
statically link instead).

### Native podman flow (primary)

```sh
TAG=$(git rev-parse --short HEAD)
podman build --sbom=syft-spdx --sbom-output sbom.spdx.json \
  --build-arg GIT_SHA=$TAG \
  --build-arg BUILD_DATE=$(date -u +%Y-%m-%dT%H:%M:%SZ) \
  -t quay.io/jwesterl/rhhi-cplusplus:$TAG \
  -t quay.io/jwesterl/rhhi-cplusplus:latest .
podman push quay.io/jwesterl/rhhi-cplusplus:$TAG
podman push quay.io/jwesterl/rhhi-cplusplus:latest
```

The committed `sbom.spdx.json` (61 packages, SPDX 2.3) was produced this
way — it lists `libcurl 8.23.0`, `openssl-libs 3.5.9`,
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

# attach the SBOM (NB: --type spdxjson, not spdx — cosign v3 embeds spdx
# as a legacy string predicate, which fails its own bundle self-check
# with "invalid attestation: decoding json")
cosign attest --predicate sbom.spdx.json --type spdxjson --yes $IMG

# attach the vulnerability scan (grype JSON, committed as vuln.grype.json)
grype registry:$IMG -o json=vuln.grype.json
cosign attest --predicate vuln.grype.json --type vuln --yes $IMG
```

Verify (pin the identity/issuer shown at signing time):

```sh
cosign verify $IMG \
  --certificate-identity <you@example.com> \
  --certificate-oidc-issuer https://accounts.google.com
cosign verify-attestation --type spdxjson $IMG \
  --certificate-identity <you@example.com> \
  --certificate-oidc-issuer https://accounts.google.com
```

The committed `vuln.grype.json` is a point-in-time sample: 12 matches
(4 High, 6 Medium, 2 Low) — even hardened-pipeline libs carry CVEs,
which is precisely why the RPM-visible dynamic approach matters.

## Supply-chain position (RHHI scanning model)

Red Hat's [security scanning integration guide](https://security.access.redhat.com/)
describes the RHHI ecosystem as three components — here's how this
derived image maps onto each:

1. **CSAF/VEX feed — triaged, all 12 matches corroborated.** Grype
   matches were checked against the per-CVE VEX documents at
   `https://security.access.redhat.com/data/csaf/v2/vex-feed/<year>/<cve>.json`
   (lowercase filename). Result: every match is `known_affected` for the
   hummingbird product streams we ship (`glibc.src`, `gcc.src`,
   `cyrus-sasl.src`) — the scanner wasn't crying wolf. Two nuances:
   - CVE-2026-115455 is `fixed` in the **gcc13** stream
     (RHSA-2026:94989) but `known_affected` in **gcc 16.x**, which is
     what our `libstdc++ 16.2.1` comes from. Stream matters, not just
     CVE ID.
   - The cyrus-sasl VEX suggests removing `cyrus-sasl-md5` if
     DIGEST-MD5 isn't needed — not applicable here (only
     `cyrus-sasl-lib` is installed; `install_weak_deps=False`).
   Grype can ingest VEX directly (`grype --vex`, OpenVEX format) to
   filter `not_affected` — worth wiring into CI once the match count
   justifies it.
2. **Image metadata — ours, not just inherited.** The base's
   `/usr/share/buildinfo/labels.json` and OCI labels ride along, but
   `created`/`revision`/`name` are overridden at our build (see Build
   section) so scanner age-assessment dates *this* image. The `cpe`
   label stays `cpe:/a:redhat:hummingbird:1` — the key that links the
   image to VEX statements.
3. **Package data — RPM DB + attested SBOM.** The RPM DB updated by
   `--installroot` is what Syft/Grype read. Note the asymmetry with
   official RHHI images: Red Hat attaches release SBOMs retrievable via
   `cosign download sbom`; ours travels as a DSSE attestation
   (`verify-attestation --type spdxjson`) because `cosign attach sbom`
   is deprecated in v3.

### Scanner workflow for this image

Following the guide's recommended workflow, concretely:

1. **Identify the product:** `cpe: cpe:/a:redhat:hummingbird:1` (inherited
   label) + our `name: rhhi-cplusplus` label (overridden at build).
   Filesystem-level scanners can read the same values from
   `/usr/share/buildinfo/labels.json` (note: `created`/`revision` there
   still describe the *base* build — the OCI labels are authoritative
   for this image).
2. **Check the VEX feed:** per-CVE documents (lowercase filenames) under
   the feed URL above; match on the hummingbird CPE, then on product
   names (`hummingbird-1:<srpm>.src`) for the fixed/affected split.
3. **Enumerate packages:** committed `sbom.spdx.json`, or
   `syft scan registry:quay.io/jwesterl/rhhi-cplusplus:<tag>`, or the
   RPM DB directly. Syft PURLs look like
   `pkg:rpm/hummingbird/libcurl@8.23.0-2.hum1?arch=x86_64&distro=...&upstream=...src.rpm`
   — the `upstream` qualifier ties the binary RPM to its source.
4. **Correlate:** package name/version/PURL against VEX product status
   (done in §1 above for the current 12 matches).
5. **Assess image age:** our overridden `org.opencontainers.image.created`
   vs. advisory fix dates.

Out of scope here by design: Hermeto/Mobster build-provenance SBOMs
(no lockfile dependencies in this repo — system RPMs are the whole
supply chain), and the `quay.io/hummingbird-community/` and `-rawhide`
registries. Official image releases and advisories live in the
[Red Hat Container Catalog](https://catalog.redhat.com/).

### References (from the RHHI scanning guide)

- [CSAF VEX feed](https://security.access.redhat.com/data/csaf/v2/vex-feed/)
  · [Advisories](https://security.access.redhat.com/data/csaf/v2/advisories/)
- [Embedded metadata schema](https://github.com/RedHatProductSecurity/security-data-guidelines/blob/main/schema/embedded_metadata.v1.schema.json)
- [Syft](https://github.com/anchore/syft) · [Hermeto](https://github.com/hermetoproject/hermeto) · [Mobster](https://github.com/konflux-ci/mobster) · [Cosign](https://github.com/sigstore/cosign) · [SPDX 2.3](https://spdx.github.io/spdx-spec/v2.3/)
- [RHHI RPMs](https://packages.redhat.com/api/pulp-content/public-hummingbird/source/Packages)
- Registries: `registry.access.redhat.com/hi/` (production),
  `quay.io/hummingbird/` (mirror), `quay.io/hummingbird-community/`,
  `quay.io/hummingbird-rawhide/` (dev)

## Verifying the base images (RHHI Konflux key)

`rhhi-cosign.pub` is Red Hat's Hummingbird release key (key-based
signing, so verification skips the transparency log). The Quay mirror
is content-identical to `registry.access.redhat.com` (same digests —
verified 2026-11-05 for both `core-runtime:latest` and
`:latest-builder`), so verify the mirror and compare digests:

```sh
cosign verify --key rhhi-cosign.pub --insecure-ignore-tlog \
  quay.io/hummingbird/core-runtime:latest
skopeo inspect docker://registry.access.redhat.com/hi/core-runtime:latest \
  | grep Digest
```

Known gap, stated plainly: only the **runtime** image carries a
signature (`no signatures found` for `:latest-builder`). The builder is
trusted via registry TLS/auth + digest pinning only. If that gap
matters to you, pin both bases by digest in the Dockerfile.

## Continuous supply chain (GitHub Actions)

`.github/workflows/supply-chain.yml` automates this whole README on
every push to `main` (PRs get build + scan only — nothing is pushed or
signed from a PR):

1. Verify the base via the Quay mirror + RHHI key, and fail if
   `registry.access.redhat.com` drifts from the mirror digest.
2. Build with `GIT_SHA`/`BUILD_DATE` args, push by short-SHA tag
   (+ `latest`), expose the manifest digest.
3. Syft SBOM + Grype scan; **gate fails only on Critical** — the
   current Highs are VEX-acknowledged upstream (see above), so failing
   on High would red-light every build by design.
4. Keyless `cosign sign` + `attest` (SBOM `spdxjson`, scan `vuln`)
   against the digest, using ambient GitHub OIDC — no browser, no keys.
   SBOM and scan are also kept as per-run workflow artifacts.

CI-signed artifacts carry the workflow identity, so verify them with:

```sh
ID=https://github.com/johwes/rhhi-cplusplus/.github/workflows/supply-chain.yml@refs/heads/main
cosign verify --certificate-identity "$ID" \
  --certificate-oidc-issuer https://token.actions.githubusercontent.com \
  quay.io/jwesterl/rhhi-cplusplus:<sha>
```

Needs two repo secrets: `QUAY_USERNAME` / `QUAY_PASSWORD` (Quay robot
account with Write on `jwesterl/rhhi-cplusplus`).

### Enterprise extensions

The flows above use public infrastructure. Two Red Hat products are
the graduation path when that stops being acceptable:

- [Trusted Artifact Signer](https://developers.redhat.com/products/trusted-artifact-signer) —
  self-hosted Sigstore (Fulcio + Rekor + TSA + TUF). Use it when the
  public Sigstore instance is off the table (air-gapped builds,
  compliance regimes that forbid publishing build metadata to a public
  transparency log, or an org-owned trust root). The cosign commands in
  this repo keep the same shape; only the trust root and OIDC issuer
  change. Prerequisite: runs on OpenShift, so treat it as the
  production rollout, not a quick swap.
- [Trusted Profile Analyzer](https://developers.redhat.com/products/trusted-profile-analyzer) —
  centralized SBOM ingestion with vulnerability/VEX correlation and
  policy enforcement. It productizes the manual triage in the
  supply-chain section above (grype output → CSAF VEX → verdict).
  Overkill for a single demo image; the natural home for this workflow
  once it spans many images and teams.

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
- Only `openssl-libs` is installed, not the `openssl` CLI: `libcurl`
  already Requires `libssl.so.3`/`libcrypto.so.3`, so `dnf` pulls the
  shared libraries automatically and the 1 MB `/usr/bin/openssl`
  multi-tool never lands in the image. Same logic applies to any lib —
  depend on the soname, not the tool package.
- Client hardening in `main.cpp`: redirects capped at 5
  (`CURLOPT_MAXREDIRS`) and restricted to http/https
  (`CURLOPT_REDIR_PROTOCOLS_STR`) so a malicious endpoint can't bounce
  the client into `file://` or internal protocols.
- Verified: app returns `HTTP status: 200`, `libcurl.so.4`/`libssl.so.3`
  present, RPM DB updated at `/usr/lib/sysimage/rpm/rpmdb.sqlite`, and no
  `dnf`/`rpm` binaries in the final image.
