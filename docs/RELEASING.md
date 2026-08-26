# Release workflow

Yanami separates source validation, package production, and public publishing.
No workflow in this repository can create a tag or GitHub Release.

## Version model

- `VERSION` and the Cargo workspace version identify the development line as
  `0.2.0-dev.0`.
- A successful package workflow on `main` derives an immutable candidate such
  as `0.2.0-dev.42`, where `42` is the `Desktop packages` workflow run number.
- The same version is embedded in the application, package names,
  `BUILD_INFO.json`, and the intended tag `v0.2.0-dev.42`.
- Re-running the same workflow run rebuilds the same commit and version. A new
  workflow run receives a new candidate version.

## Repository setup

Configure these GitHub settings before relying on the chain:

1. Protect `main`; require pull requests and the stable aggregate check
   (`CI / Required`) to pass before merge. Individual platform jobs may be
   path-routed, so branch protection must depend on the aggregate rather than
   every conditional job. Do not allow the required check to be bypassed.
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
2. Open a pull request. `CI` classifies the changed paths and runs only the
   applicable gates. Product desktop changes receive complete C++/QML builds
   and tests on Windows x86_64, Linux x86_64, macOS arm64, and macOS x86_64;
   Rust, workflow, CodeQL, package-install, and hosted performance checks are
   enabled only when their inputs changed. Documentation-only changes retain
   the stable aggregate check without allocating a platform build runner.
3. Merge only after the required checks pass.
4. The successful `CI` push run on `main` triggers `Desktop packages` for that
   exact commit. A manual dispatch from `main` is also available for recovery.
5. When packaged inputs changed, the workflow builds and audits four packages,
   then uploads `Yanami-<version>-all-platforms`. It contains the packages,
   per-file checksums, `SHA256SUMS.txt`, and `release-manifest.json`; artifacts
   expire after 30 days. Documentation, performance-policy, and CI-governance
   changes still validate immutable metadata but skip the four package builds.

Linux and macOS packaging is CI-validated but remains experimental until it is
smoke-tested on real machines. Windows and Linux candidates are unsigned;
macOS candidates are ad-hoc signed but not Developer ID signed or notarized.

## Manual performance and playback acceptance

The Windows hosted performance smoke inside `CI` is intentionally limited to
deterministic correctness and catastrophe checks. Its offscreen/software
renderer cannot certify a real GPU, native Presents, hardware decoding, or
Anime4K output. Yanami therefore has no self-hosted performance jobs, scheduled
lab jobs, or manual strict-matrix dispatch in GitHub Actions.

Before publishing a release candidate, an operator must complete and retain a
manual fixed-machine acceptance bundle:

1. Use an isolated Windows machine matching
   `perf/environments/windows-reference-v1.json`; record the concrete CPU, GPU,
   driver, display, power-plan, toolchain, and font-set fingerprint.
2. Provision every asset and SHA-256 in `PlaybackMedia-v1`, plus all three
   Anime4K presets and the approved evidence normalizer in
   `UpscalingModelPack-v1`, through a reviewed performance-policy change. A
   `not-provisioned` contract blocks certification.
3. Exercise real playback and Anime4K performance, balanced, and quality modes
   through the production Qt OpenGL + libmpv OpenGL path. Retain PresentMon,
   pixel captures, mpv telemetry, GPU/VRAM counters, runtime traces, and the
   normalizer output. Hosted/offscreen data is not acceptable.
4. Collect four complete `Release` PerfResults on the same machine in
   merge-base/candidate/merge-base/candidate (`A-B-A-B`) order. Evaluate them
   with `scripts/performance/run-gate.ps1`, `-Mode enforce`, both commit SHAs,
   and all seven suites. Missing fixtures, measurements, raw evidence, or a
   valid A-B-A-B comparison must remain `infra-invalid` or `fail`.
5. Install and launch each packaged platform artifact on a real target machine;
   on Windows also verify playback, controller navigation, and enabling and
   disabling Anime4K during playback. Record the tested package SHA-256.

The strict performance contracts and example commands are documented in
`perf/README.md`. These are human release approvals, not claims made by GitHub
Hosted runners.

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
