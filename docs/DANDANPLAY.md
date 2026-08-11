# 弹弹play Open Danmaku Network access statement

This document describes the planned API usage for the Yanami application.
Yanami is currently under active development and has not published a stable
release.

## Project identity

- **Application:** Yanami
- **Project type:** Open-source, non-commercial desktop media player
- **License:** GPL-3.0-or-later
- **Platforms:** Windows, macOS, and Linux
- **Primary purpose:** Play media from a user's own Emby server
- **Danmaku purpose:** Optionally match a selected video to an episode and
  display comments while that video is playing

Yanami identifies and credits the
[弹弹play Open Danmaku Network](https://www.dandanplay.com/) in the public
README and in the application's settings UI.

## API operations

Yanami plans to use only the following public application APIs:

| Endpoint | Trigger | Local policy |
| --- | --- | --- |
| `POST /api/v2/match` | User starts a directly playable media item that has no saved association | At most once for that Emby server/item/media-source tuple unless the user requests rematching |
| `GET /api/v2/search/episodes` | Future manual-match UI explicitly submitted by the user | No background search, discovery, indexing, or bulk calls |
| `GET /api/v2/comment/{episodeId}?withRelated=true` | The selected episode has no fresh cached response, or the user explicitly refreshes it | Six-hour fresh cache and seven-day stale offline fallback |

Yanami does not use the batch-match endpoint, perform bulk comment downloads,
mirror the database, or crawl search results. It does not currently call comment
submission, user account, follow, or playback-history endpoints.

## Matching data

The match request contains the media filename without its directory, the file
size, duration in seconds, and the MD5 hash of the first 16 MiB. For remote Emby
media, Yanami makes a range request to the user's Emby server and hashes those
bytes locally. Video contents are not uploaded to 弹弹play.

The selected `episodeId` is saved locally against the Emby server ID, item ID,
and media-source ID so subsequent playback does not repeat the match request.

## Authentication and credential handling

All direct requests use the documented client signature headers:

- `X-AppId`
- `X-Timestamp`
- `X-Signature = Base64(SHA256(AppId + Timestamp + lowercasePath + AppSecret))`

Development builds use developer-provided credentials stored in the operating
system credential vault. Protected GitHub Actions release builds can inject the
approved Yanami application credential from repository secrets. Generated
client data is obfuscated, but the project treats client-side credentials as
recoverable and will monitor and rotate them when necessary.

## Usage and commercial policy

- API calls are made only in response to actual playback or an explicit manual
  action.
- The integration is non-commercial and is available to all users without a
  separate fee or subscription.
- 弹弹play data is not resold, republished as a database, or advertised as a
  paid product feature.
- API errors degrade gracefully to cached comments or video playback without
  danmaku.

The implementation will be updated if the official API agreement, quotas, or
access permissions change.
