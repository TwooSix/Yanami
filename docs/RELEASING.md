# Release workflow

Yanami separates source validation, package production, and public publishing.
No workflow in this repository can create a tag or GitHub Release.

## Version model

- `VERSION` and the Cargo workspace version identify the development line as
  `0.1.0-dev.0`.
- A successful package workflow on `main` derives an immutable candidate such
  as `0.1.0-dev.42`, where `42` is the `Desktop packages` workflow run number.
- The same version is embedded in the application, package names,
  `BUILD_INFO.json`, and the intended tag `v0.1.0-dev.42`.
- Re-running the same workflow run rebuilds the same commit and version. A new
  workflow run receives a new candidate version.

## Repository setup

Configure these GitHub settings before relying on the chain:

1. Protect `main`; require pull requests and every `CI` job to pass before
   merge. Do not allow required checks to be bypassed.
2. Create a protected `Release` environment restricted to `main`. Store
   `YANAMI_DANDANPLAY_APP_ID` and `YANAMI_DANDANPLAY_APP_SECRET` there. Leave
   required reviewers disabled if packaging must begin automatically after the
   merge; enabling them intentionally turns package creation into an approval
   gate.
3. Keep workflow permissions read-only by default. `Desktop packages` needs
   only read access and artifact storage; it deliberately has no
   `contents: write` permission.

## Automated candidate path

1. Commit the complete change on a feature branch and push it.
2. Open a pull request. `CI` runs Rust gates and complete C++/QML builds and
   tests on Windows x86_64, Linux x86_64, macOS arm64, and macOS x86_64.
3. Merge only after the required checks pass.
4. The successful `CI` push run on `main` triggers `Desktop packages` for that
   exact commit. A manual dispatch from `main` is also available for recovery.
5. The workflow builds and audits four packages, then uploads
   `Yanami-<version>-all-platforms`. It contains the packages, per-file
   checksums, `SHA256SUMS.txt`, and `release-manifest.json`. Artifacts expire
   after 30 days.

Linux and macOS packaging is CI-validated but remains experimental until it is
smoke-tested on real machines. Windows and Linux candidates are unsigned;
macOS candidates are ad-hoc signed but not Developer ID signed or notarized.

## Manual prerelease

Use PowerShell 7 with authenticated `gh` and `git` clients. The first command
downloads and verifies the aggregate artifact without changing remote state:

```powershell
.\scripts\publish-release.ps1 -RunId <desktop-packages-run-id>
```

Review the workflow URL, manifest, checksums, package names, and commit. Then
explicitly authorize the remote tag and prerelease creation:

```powershell
.\scripts\publish-release.ps1 -RunId <desktop-packages-run-id> -Publish
```

The publishing script refuses failed/non-main runs, mismatched hashes, an
unexpected platform set, a candidate not contained in `origin/main`, a tag
pointing elsewhere, or an existing Release. The tag provides GitHub's source
archives for the exact distributed commit.
