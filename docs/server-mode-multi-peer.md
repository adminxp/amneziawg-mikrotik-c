# Server mode multi-peer MAC1 design

## Problem

`AWG_MODE=server` historically reused the reverse-mode outbound transform and therefore had only one global outbound MAC1 key (`cfg->mac1key_out`). In practice that key came from `AWG_CLIENT_PUB`.

That was good enough for two legacy scenarios:

1. **Normal / reverse 1:1**
   - There is only one remote peer key anyway.
2. **Proxy-only 1:N server mode**
   - The server-side proxy could emit outbound AWG handshake responses using a placeholder `AWG_CLIENT_PUB`.
   - This still worked because the receiving **normal** client-side proxy recalculated inbound MAC1 again before handing the packet to the local WireGuard interface.

## Why direct AmneziaWG 2.0 clients failed

A direct AmneziaWG 2.0 client receives the packet produced by the server-side proxy **directly**. There is no extra client-side proxy hop that can fix MAC1 afterwards.

So for a direct client, the server must rewrite an outbound WG handshake response with the MAC1 key derived from **that specific client public key**.

A single global `AWG_CLIENT_PUB` is not enough once multiple direct clients share the same server.

## New peer model

Server mode now supports an explicit direct-peer list:

- `AWG_CLIENT_PUBS`
- `AWG_CLIENT_PUBS_FILE`

These contain the **real WireGuard public keys** of direct AmneziaWG clients.

At startup the proxy precomputes the response MAC1 key for every listed peer.

On outbound `WG_HANDSHAKE_RESPONSE` in `AWG_MODE=server`:

1. The proxy first looks for a cached resolved peer in the session entry.
2. If there is no cached peer yet, it takes the **original standard-WG response** from the local WG server.
3. It recomputes the response MAC1 for every configured direct peer candidate.
4. The peer whose recomputed MAC1 matches the packet's original MAC1 is selected.
5. That peer index is cached in the session entry.
6. The packet is then transformed to AWG using that exact peer-specific outbound MAC1 key.

Session routing itself stays unchanged: address routing still uses the existing sender/receiver index table.

## Legacy fallback remains

`AWG_CLIENT_PUB` still exists.

- In `normal` and `reverse` modes it keeps its old meaning.
- In `server` mode it remains the **legacy fallback** key.

If no explicit direct peer matches an outbound WG response, the proxy falls back to `AWG_CLIENT_PUB`.

This preserves:

- old single-peer server setups
- placeholder-based proxy-only 1:N deployments

## Operational meaning

- **Direct AmneziaWG 2.0 client** in server mode:
  - must have its real public key present in `AWG_CLIENT_PUBS` or `AWG_CLIENT_PUBS_FILE`
- **Proxy-only client**:
  - may still rely on the legacy fallback placeholder in `AWG_CLIENT_PUB`

## Migration

Old behavior:

- `AWG_CLIENT_PUB` in server mode acted like a required but effectively global outbound key

New behavior:

- `AWG_CLIENT_PUB` = legacy single-peer / proxy-only fallback
- `AWG_CLIENT_PUBS` / `AWG_CLIENT_PUBS_FILE` = explicit direct-client peer list for multi-peer server mode

If your deployment only uses legacy normal-proxy clients, it can keep working as before.
If any client connects directly as AmneziaWG 2.0, add its real public key to the explicit peer list.
