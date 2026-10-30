# Security policy

## Reporting a vulnerability

Report a vulnerability privately through
[GitHub Security Advisories](https://github.com/sean3z/crocket/security/advisories/new).
The report goes to the maintainers and is not a public issue. Please include:

- the crocket version, git commit, or image tag you ran
- what goes wrong, and how to reproduce it (a request, a crashing input, or a
  failing configuration)
- the impact you see (for example a crash, a memory error, a request that
  bypasses a check, or a response that leaks data)

Do not open a public GitHub issue for a vulnerability that is not yet fixed, and
do not put a crashing input in a public place.

The repository does not publish a separate security email address. The advisory
form is the way to report.

## Supported versions

Security fixes ship on the latest stable release (the `X.Y.Z` tag on the
[stable channel](README.md#versions-and-channels)). `main` and experimental
(`X.Y.Z-rc.N`) tags are signed the same way, but they are not a supported
release. Older tags are not maintained.

## Scope

In scope:

- crocket itself: the HTTP/1.1, HTTP/2, and gRPC request path, TLS as crocket
  configures it, and the parsers that read untrusted bytes (JSON, query strings,
  routes, headers, and protobuf)
- the pinned [h2o](https://github.com/h2o/h2o) engine (`cmake/h2o.cmake`) when a
  request handled by crocket can reach the bug
- `ghcr.io/sean3z/crocket-hello`, and the signatures and software bills of
  materials this repository publishes for it

Out of scope:

- an application built with crocket, including its handlers, SQL, secrets, and
  certificates
- `ghcr.io/sean3z/crocket-toolchain`, which is a development image, except where
  the same flaw is shipped inside `crocket-hello`
- a bug in OpenSSL, zlib, Ubuntu, GCC, or libpqxx that crocket does not expose
  (report it to that project; if crocket's use of it is the problem, report it
  here)
- a large volume of otherwise valid traffic
- a feature the roadmap has not built yet, such as authentication or rate
  limiting. Not having that feature is not, by itself, a vulnerability
- a bug that is not a security issue. Open a public GitHub issue for those

## Response

We aim to:

- acknowledge a report within 3 business days
- say whether we can confirm it within 10 business days
- ship a fix, or a mitigation you can apply, within 90 days of confirming an
  issue we accept

Those are targets. A report about a vulnerability that is already being used
against crocket is handled first.

## Disclosure and credit

Please keep the details private until a fix is released, unless we agree on a
different date. We will coordinate that date with you. It is usually the day the
fix ships.

The fix is a normal release, noted in [CHANGELOG.md](CHANGELOG.md), and a
GitHub Security Advisory. We credit the reporter in both, unless you ask us not
to.

## Signed images and SBOMs

The [release workflow](.github/workflows/release.yml) signs every
`ghcr.io/sean3z/crocket-hello` image it pushes (the
[stable, latest, and experimental tags](README.md#versions-and-channels)) with
[Sigstore](https://www.sigstore.dev/) cosign. Signing is keyless: the certificate
comes from the public Fulcio instance, the workflow's GitHub Actions identity,
and the signature is recorded in the public Rekor log. There is no long-lived
signing key.

The signed subject is the image digest the tags point at. Buildx also attaches
its own provenance manifest, so that digest is an image index. Verify the tag
you pull. A moved tag (`:stable`, `:latest`, `:experimental`, `:0.4`) verifies
as whatever digest it points at now, which was signed when that image was
pushed.

The same job scans that digest with [Syft](https://github.com/anchore/syft) and
attests a CycloneDX JSON document and an SPDX JSON document to it. The scan is
the hello image's packages (the Ubuntu base and libraries such as OpenSSL), not
the headers an application compiles itself. h2o is compiled into crocket and
pinned in `cmake/h2o.cmake`; it is not its own package in the image. The file
list of every Debian package is left out so the document stays a package bill
of materials.

On a `X.Y.Z` or `X.Y.Z-rc.N` tag, those two files are also attached to the
GitHub Release, named `crocket-hello-<tag>.cdx.json` and
`crocket-hello-<tag>.spdx.json`. `:latest` has no GitHub Release. Its bill of
materials is the attestation on the image, and the workflow run keeps a copy as
the `crocket-hello-sbom` artifact.

A build-provenance attestation is stored for the same digest.
`gh attestation verify` checks that one.

### Check the signature

Install [cosign 3](https://github.com/sigstore/cosign) (the release workflow
installs that major version) and run:

```bash
cosign verify \
  --certificate-oidc-issuer https://token.actions.githubusercontent.com \
  --certificate-identity-regexp '^https://github.com/sean3z/crocket/\.github/workflows/release\.yml@refs/(heads/main|tags/[0-9]+\.[0-9]+\.[0-9]+(-rc\.[0-9]+)?)$' \
  ghcr.io/sean3z/crocket-hello:0.4.0
```

The same command works for `:stable`, `:0.4`, `:latest`, `:sha-<short>`, and an
experimental tag. The identity is this repository's `release.yml`, on `main` or
on a version tag.

### Check the software bill of materials

The release asset is the file Syft wrote. For the 0.4.0 release:

```bash
gh release download 0.4.0 --repo sean3z/crocket \
  --pattern 'crocket-hello-0.4.0.cdx.json' \
  --pattern 'crocket-hello-0.4.0.spdx.json'
```

The attestation is what binds that bill of materials to the image. `--type cyclonedx`
is the predicate `https://cyclonedx.org/bom`, and `--type spdxjson` is
`https://spdx.dev/Document`:

```bash
cosign verify-attestation \
  --type cyclonedx \
  --certificate-oidc-issuer https://token.actions.githubusercontent.com \
  --certificate-identity-regexp '^https://github.com/sean3z/crocket/\.github/workflows/release\.yml@refs/(heads/main|tags/[0-9]+\.[0-9]+\.[0-9]+(-rc\.[0-9]+)?)$' \
  ghcr.io/sean3z/crocket-hello:0.4.0
```

Use `--type spdxjson` for the SPDX document. `cosign download attestation` with
`--predicate-type https://cyclonedx.org/bom` prints the signed statement. The
release asset is the raw document.

### Check the build provenance

```bash
gh attestation verify oci://ghcr.io/sean3z/crocket-hello:0.4.0 \
  --repo sean3z/crocket \
  --signer-workflow sean3z/crocket/.github/workflows/release.yml
```

## Fuzzing

Parsers that read untrusted bytes have fuzz targets you can run locally
(`./dev fuzz`; see [Fuzzing](README.md#fuzzing)). Each target also runs its
corpus in `ctest`. Continuous fuzzing in CI is planned on the way to 1.0
([Roadmap to 1.0](https://github.com/sean3z/crocket/issues/37)) and is not
running yet. A crash from the fuzzer is a vulnerability: report it as above.
