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
5. When packaged inputs changed, the workflow builds and audits the Windows,
   Linux, and two macOS platform payloads, then uploads
   `Yanami-<version>-all-platforms`. It contains every public release asset,
   a checksum sidecar for each one, `SHA256SUMS.txt`, and
   `release-manifest.json`; artifacts expire after 30 days. Documentation,
   performance-policy, and CI-governance changes still validate immutable
   metadata but skip the platform package builds.

The Windows release boundary consists of these files:

- `Yanami-<version>-Windows-x86_64-Setup.exe`: the preferred branded per-user
  setup wizard. It verifies and delegates to an embedded Velopack installer,
  lets the user choose a writable install directory and Start-menu/desktop
  shortcuts, and shows the installed path and Windows uninstall route before
  it exits;
- `Yanami-<version>-Windows-x86_64.zip`: a portable fallback;
- `io.github.TwooSix.Yanami-<version>-preview-full.nupkg` and
  `releases.preview.json`: the Velopack `preview` update feed;
- `io.github.TwooSix.Yanami-<version>-preview-delta.nupkg` when a prior
  published preview full package is available as a compatible baseline.

Packaging uses hash-pinned Velopack CLI and native `velopack_libc` 1.2.0
artifacts with package ID `io.github.TwooSix.Yanami` and a `win-x64` runtime.
The Yanami setup shell owns the explicit user choices, while the embedded
Velopack backend remains the sole owner of package state, update registration,
rollback, and uninstall. The shell uses Windows Direct2D/DirectWrite for
antialiased surfaces and text, with native controls for keyboard navigation and
accessibility. `yanami-installer-painter-tests` checks 100%, 150%, and 200%
rendering; release visual checks must also cover first-open text, pointer versus
keyboard focus, card corners, and long paths in the actual Setup executable.
Shortcuts point into the Velopack installation root so
Velopack can keep them valid across updates and remove them during uninstall.
The Start-menu choice means the searchable **All apps** entry; an unpackaged
Win32 app cannot silently add itself to the Windows **Pinned** area. CI
downloads and verifies only a published preview full package as the delta
baseline; the first Velopack
migration release legitimately has no delta. `assets.preview.json` and the
legacy `RELEASES-preview` index are build-side files and are not public release
assets. Every public file, including the Setup executable, feed, full package,
and optional delta, must have its own `.sha256` file and an exact name/hash/size
entry in `release-manifest.json`.

The setup shell's non-interactive contract is intentionally explicit; all
choices must be supplied so automation cannot inherit a changing GUI default:

```powershell
Yanami-<version>-Windows-x86_64-Setup.exe --silent `
  --install-dir "C:\Users\runner\AppData\Local\Programs\Yanami" `
  --start-menu yes --desktop no --no-launch
```

`--verify-payload` performs a read-only SHA-256 check of the embedded canonical
Velopack Setup and returns its size and digest as JSON. Release packaging runs
this check before publishing the wrapper.

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
   on Windows use Setup to verify that no system change occurs before the final
   confirmation, test both the default and a path containing spaces/non-ASCII
   characters, exercise each shortcut choice, confirm the displayed install
   location and Apps & Features registration, update from the prior published
   preview (delta when offered, full-package fallback otherwise), and perform a
   clean uninstall through Windows Settings. Also verify playback,
   controller navigation, and enabling and disabling Anime4K during playback.
   Record every tested asset SHA-256.

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

The publishing script refuses failed/non-main runs, mismatched hashes, missing
Windows installer/update assets, an unexpected asset name, a Velopack feed that
does not bind the current full and optional delta hashes/sizes, a candidate not
contained in `origin/main`, a tag pointing elsewhere, or an existing Release.
The tag provides GitHub's source archives for the exact distributed commit.
