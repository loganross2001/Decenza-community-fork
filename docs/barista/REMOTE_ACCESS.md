# Remote access — let Claude read your DE1 config + shot history over the network

**Goal (owner's ask, 2026-09-09):** use Decenza's built-in network server so Claude can read the real
machine config and shot history *remotely* — instead of the owner hand-copying data or Claude being
blind to the real device. This note is the one-step-tomorrow recipe. Grounded in `src/network/shotserver*`.

## The situation in one paragraph

Decenza already ships an in-app **web server (ShotServer)** and an **MCP server** — both are exactly this
capability. The real coffee data lives on the **Samsung tablet**, not this Mac (the Mac copy is a sandbox:
7 test/"Live DYE" shots in `~/Library/Application Support/DecentEspresso/Decenza/shots.db`). Claude runs on
the Mac, so to read the *real* data the tablet's server has to be reachable from the Mac over your LAN.
Claude cannot enable the server or clear its login for you — that's the one manual step.

## What the owner does (once, ~2 minutes)

1. **Turn the server on** in the app on the tablet. (`shotServer/enabled` defaults to **off**; default port
   **8888**.) The app's Connections/AI settings has the toggle and shows the address.
2. **Find the tablet's address** — its LAN IP + port, e.g. `192.168.1.42:8888`. (The app also advertises over
   mDNS/UDP discovery, but an explicit IP is simplest.)
3. **Pick how Claude authenticates** — auth is gated by `isSecurityEnabled()`. Easiest first:
   - **(a) Security OFF (LAN-only, simplest):** nothing else needed — every endpoint is open `curl`.
     Fine on a trusted home network; don't expose the port to the internet.
   - **(b) MCP API key (works with security ON):** the MCP route accepts `Authorization: Bearer <key>`
     instead of a login. Enable **Settings → AI → MCP Server**; it has an API key — send Claude that key.
   - **(c) TOTP (web security ON):** the web pages use a TOTP-code login → session cookie. This is the
     hard path for Claude (needs a fresh 6-digit code each session); prefer (a) or (b).

**Send Claude two things:** the tablet `IP:port`, and either "security is off" **or** the MCP API key.

## What Claude does then (the read path, grounded in the routes)

Web REST (plain `curl`; add `-k` if HTTPS/self-signed; add `-H "Authorization: Bearer <key>"` only for
`/mcp` under option (b)):

| What | Endpoint |
|---|---|
| Shot history (JSON) | `GET /api/shots` |
| Full database (SQLite download) | `GET /api/database` (a.k.a. `/database.db`) |
| Settings / config | `GET /api/settings/...` |
| Live machine state / power | `GET /api/telemetry`, `GET /api/power/status` |
| Beans / recipes / equipment | `GET /api/bags`, `/api/recipes`, `/api/equipment` |
| Human pages | `/shots`, `/beans`, `/recipes`, `/equipment` |

Example once the address is known and security is off:

```sh
curl -s http://<tablet-ip>:8888/api/shots | head
curl -s http://<tablet-ip>:8888/api/database -o de1.db   # then read locally with sqlite3
```

MCP as tools in a Claude Code session (option (b)) — add to `.mcp.json` (fill in real values; do NOT
commit a real key):

```json
{ "mcpServers": { "decenza": {
  "type": "http", "url": "http://<tablet-ip>:8888/mcp",
  "headers": { "Authorization": "Bearer <mcp-api-key>" } } } }
```

This gives Claude the purpose-built tools (read machine state, browse/analyze shots, dial-in context,
and — at higher access levels — change profiles/settings). See `docs/CLAUDE_MD/MCP_SERVER.md`.

## Proven locally 2026-09-09

The read path is confirmed against the Mac's own databases (same schema the web API serves):
`shots.db` → `shots` (7 rows: dose/yield/enjoyment/profile), plus `equipment_items`, `equipment_packages`,
`coffee_bags`, `recipes`; `assistant.db` → barista KB. So the mechanism works; only the network hop to the
tablet is owner-gated. Claude did NOT launch the app or change any app settings.

## Security note

The server binds `0.0.0.0:<port>`. Option (a) leaves it unauthenticated — keep it to a trusted LAN and
never port-forward it. Options (b)/(c) require a secret. HTTPS mode uses a self-signed cert (`curl -k`).
