# BuffPlum Release Process

BuffPlum Foundation Sunshine versions use:

```text
vYYYY.MDD-buffplum.N
```

Example: `v2026.719-buffplum.1`.

## Publishing

1. Merge validated changes into the fork's `master` branch.
2. Create a prerelease from `master` with the next BuffPlum tag.
3. The `Build and Release` workflow builds the Windows installer and portable ZIP, runs the configured checks, generates checksums, and attaches the files to that release.
4. Verify that the installer, portable ZIP, `SHA256SUMS.txt`, and `checksums.json` are present.
5. Promote the prerelease to a normal release only after the assets are complete.

The prerelease-first flow prevents clients from discovering an empty release through the GitHub latest-release API.

## Package Trust

- Packages produced by the standard fork workflow are unsigned.
- Optional drivers from inaccessible upstream private repositories are omitted instead of failing the build.
- Release notes must link to [SECURITY.md](../SECURITY.md) and state that full-disk transfer is intended only for personal use on a trusted LAN.
- The SignPath workflow is manual and should only be used after valid signing credentials have been configured for this repository.

## Upstream Sync

Upstream repositories remain read-only sources for compatible fixes and security updates. Sync upstream into a temporary integration branch, run the file-transfer smoke test and WebUI checks, then merge into `master`. Fork-specific features are maintained in this repository and are not proposed upstream.
