# Security notes

## Credentials

- A developer-provided 弹弹play `AppId` is a non-secret preference and may be
  stored in SQLite. Its `AppSecret` is stored only in the operating-system
  credential vault.
- Emby access tokens are stored only in the operating-system credential vault;
  SQLite contains session indexes and user metadata, never the tokens.
- Passwords are used only for the active login request. Controllable temporary
  buffers are cleared after the request on both the Rust and Qt sides.
- Playback headers containing tokens are wrapped in `SensitiveHeaders`. Debug
  output is always redacted, and headers are never serialized to SQLite.

## Bundled release credentials

- Normal source and CI test builds contain no project 弹弹play credentials.
- The protected release workflow accepts
  `YANAMI_DANDANPLAY_APP_ID` and `YANAMI_DANDANPLAY_APP_SECRET` from GitHub
  Actions Secrets. Both must be present or the release preflight fails.
- The build script converts the values to masked byte arrays generated in
  Cargo's `OUT_DIR`; plaintext credentials are never written to the repository.
- Masking is obfuscation only. A public desktop binary must be assumed capable
  of revealing any credential it uses. Project credentials must have suitable
  quotas, usage monitoring, and a rotation plan.
- User-provided credentials override bundled credentials and remain in the
  operating-system vault. Removing the override falls back to bundled
  credentials when the release includes them.

## Network behavior

- Strict TLS verification is the default. There is no switch to ignore all
  certificate errors.
- `TlsPolicy::TrustCertificateSha256` is reserved for a future explicit
  fingerprint-confirmation flow. Until implemented, the client rejects that
  state instead of silently weakening TLS.
- Danmaku hashing requests only the first 16 MiB of a media stream and reuses
  the playback plan's authentication headers. Response bytes are not persisted.
- 弹弹play requests use the documented timestamped signature mode. Match and
  search operations are user-action driven, and comments are cached for six
  hours with a seven-day stale offline fallback.
- A danmaku API failure never interrupts video playback.

## Logging and diagnostics

- Passwords, `AppSecret` values, Emby tokens, complete authorization headers,
  and token-bearing URLs must never be logged.
- Any future crash-report upload must preview its payload to the user and apply
  an additional redaction pass before transmission.

## Known boundaries

- A significant part of the video-decoding attack surface is in the packaged
  libmpv/FFmpeg build. Release engineering must update and record those versions.
- The Qt/Rust C ABI accepts raw pointers. Every exported entry point catches
  Rust panics and validates null pointers and UTF-8. Callers must follow the
  ownership contract expressed by `yanami_string_free`.
