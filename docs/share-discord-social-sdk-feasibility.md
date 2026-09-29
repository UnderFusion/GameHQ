# Discord Social SDK contact sharing: feasibility (plan t15)

Research date: **2026-09-29**, from Discord's own developer documentation.
Nothing here was tried against a real application: no Discord application,
credentials or SDK binaries exist for GameHQ, so no proof of concept was
built. Re-check the linked pages before t16 starts; the "last updated" dates
are quoted so drift is visible.

Each finding is labelled:

- **Confirmed**: the documentation states it.
- **Limitation**: the documentation states or clearly implies a restriction.
- **Unverified**: not found or not readable; do not build on it.

## Summary for t16

| Question | Answer |
| --- | --- |
| Send text DMs to a friend | Confirmed (`Client::SendUserMessage`). |
| Discover Discord friends | Confirmed (`GetRelationships`, presence scopes). |
| Send a local PNG/JPG/MP4 | **Not supported.** No attachment or upload API exists in `Client` or `MessageHandle`. |
| GameHQ eligible for production communication features | **Unverified, likely no.** The approval criteria are written for games. |

Consequence: a native Integrated Discord provider **cannot send the capture
itself** today. t16 should consume this as a capability flag (the provider
reports no `Image`/`Video` capability, or is not offered) rather than block
the Share workstream. Discord Desktop hand-off (t14) stays the media path.

## Findings

### Media / attachments

- **Limitation.** `Client` lists `SendUserMessage`, `SendUserMessageWithMetadata`,
  `SendLobbyMessage` and `SendLobbyMessageWithMetadata`. None takes a file, and
  the class reference has no upload or attachment method.
- **Limitation.** The DM guide says the SDK handles text; non-text content
  received from Discord (images, videos, embeds, polls) is only surfaced via
  `MessageHandle::AdditionalContent()` so a game can show a placeholder.
  `AdditionalContent` exposes type, title and count. Its setters exist but the
  documentation does not describe any use for sending.
- **Limitation.** Message content is limited to 2,000 characters. Metadata is
  string key/value pairs only, not media.
- **Possible fallback** (not attachments): send a text link to media hosted
  elsewhere. That needs an explicit, user-approved relay of the capture to a
  third-party host, which is a privacy decision for the owner and is out of
  scope here.

### Friend and relationship discovery

- **Confirmed.** `Client::GetRelationships` and `GetRelationshipsByGroup`
  read relationships. It needs the default presence scopes.
- **Confirmed.** Discord friendships persist across Discord and are limited to
  1,000 accepted friends per account. That is an account limit, not a
  documented cap on what `GetRelationships` returns. Game friendships exist
  only inside the application.
- **Limitation.** The guide requires explicit user consent for any friend
  request; requests must never be sent or accepted automatically. Share only
  needs to read.

### Direct messages

- **Confirmed.** `SendUserMessage(recipientId, text, callback)`; messages may
  only be sent in response to a user action, never automatically.
- **Confirmed.** Needs the communication scopes.
- **Limitation.** Messages between provisional accounts or non-friends are
  ephemeral and do not persist. DM history needs both players to have played
  the application.
- **Limitation.** Unapproved applications are rate limited. The pages read on
  2026-09-29 (communication features and the DM guide) state 100 DMs per
  2 hours per application, not per user; treat that exact number as
  time-sensitive and re-check it. Higher limits need Discord approval.

### OAuth and scopes

- **Confirmed** (page last updated 2025-07-21; re-check):
  - `Client::GetDefaultPresenceScopes` = `openid sdk.social_layer_presence`
    (friends list, presence).
  - `Client::GetDefaultCommunicationScopes` = `openid sdk.social_layer`
    (DMs, lobbies, linked channels). Described as limited access.
- The scope strings above are what the SDK helpers return; use the helpers
  rather than hard-coding them.

### Eligibility and approval

- **Confirmed** (communication-features page, last updated 2026-07-07):
  production communication features need a "Comms Access" application in the
  Developer Portal. Required: Discord account linking, Rich Presence with
  Discord joins, full access to Discord friends, and voice or text
  communications (linked channels). Flows must be complete including denial and
  failure states; account linking within two clicks of the social features; a
  1-5 minute video of the full user flow; age-restricted user protection
  confirmation.
- **Limitation.** The criteria are written for games. The page does not say
  non-games are excluded, but GameHQ (a capture gallery and overlay) does not
  offer joinable sessions or linked channels, so meeting "Rich Presence with
  Discord joins" would mean building features unrelated to sharing.
- **Unverified.** The Discord Social SDK Terms could not be read (HTTP 403 from
  the fetch tool), so any wording that restricts use to games, or restricts
  redistributing the SDK library, is not confirmed. Read it before any build.

## Decision

1. Do not build native Discord media sending. The API cannot do it.
2. Keep Discord Desktop hand-off (t14) as the supported media path.
3. t16 (Integrated Discord) becomes a capability-driven, optional provider. It
   may be worth building only for text-only or "notify a friend" use, and only
   if the owner accepts the Comms Access approval work. Recommend deferring it
   until the owner decides.
4. Owner input needed for t16: a Discord application id, and a decision on
   whether to apply for Comms Access.

## Sources (fetched 2026-09-29)

- Communication features: https://docs.discord.com/developers/discord-social-sdk/core-concepts/communication-features
- Sending direct messages: https://docs.discord.com/developers/discord-social-sdk/development-guides/sending-direct-messages
- Managing relationships: https://docs.discord.com/developers/discord-social-sdk/development-guides/managing-relationships
- OAuth2 scopes: https://docs.discord.com/developers/discord-social-sdk/core-concepts/oauth2-scopes
- `Client`: https://discord.com/developers/docs/social-sdk/classdiscordpp_1_1Client.html
- `MessageHandle`: https://discord.com/developers/docs/social-sdk/classdiscordpp_1_1MessageHandle.html
- `AdditionalContent`: https://discord.com/developers/docs/social-sdk/classdiscordpp_1_1AdditionalContent.html
- Terms (unreadable, 403): https://support-dev.discord.com/hc/en-us/articles/30225844245271-Discord-Social-SDK-Terms
