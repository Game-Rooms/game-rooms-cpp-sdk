# SDK Generation Prompt

You are working inside a Game Rooms SDK repository generated from the
**Game-Rooms-SDK-Template** template, which serves as a starting point for auto-generated
Game Rooms SDKs. Your job is to read the API/service documentation at the URL supplied via
the `Documentation URL` system context and turn it into a complete, working SDK inside this
repository.

## Steps

1. Fetch and thoroughly read the documentation markdown at the provided `Documentation URL`.
   If the fetch tool is unavailable, note this clearly in your final summary instead of guessing
   at the contents.
2. Identify every public endpoint, method, event, data type, and configuration option described
   in the documentation.
3. Generate or update the SDK source code for this repository so it fully implements the
   documented surface area:
   - Use the "Target SDK language/platform" value supplied in the system context to pick the
     implementation language. If the repository already contains SDK code, prefer its existing
     language/tooling conventions (package manager, folder structure, linting/build config) over
     the requested value and note the discrepancy in your summary instead of switching languages.
   - Create clear, typed models/classes for each documented data type.
   - Implement a client/entry point exposing the documented methods and events.
   - Add concise inline documentation/comments only where the intent isn't obvious from the code.
4. Update or create a `README.md` for the SDK describing installation and basic usage, with at
   least one runnable example derived from the documentation.
5. Add or update automated tests that exercise the generated SDK's public surface, using
   whatever test framework is already configured in the repo (add a minimal one if none exists).
6. Run the project's build/lint/test commands to confirm the generated code is valid. Fix any
   failures before finishing.
7. Produce a short final summary (as your response) listing the files you added or changed and
   any documented features you were unable to implement, with reasons.

## Domain notes (Game Rooms protocol docs)

Documentation for this project typically describes the **Game Rooms** wire protocol: an
HTTP API for creating/discovering rooms plus an `ecast`-style WebSocket protocol
(`{ opcode, seq, params }` from the client, `{ pc, opcode, result, re? }` from the server)
used for realtime `host`/`player` messaging around key/value "objects" (`object`, `text`,
`number`). When the fetched documentation matches this shape, make sure the generated SDK:

- Wraps the HTTP endpoints for creating a room, fetching app configs, and looking up a room
  by its 4-letter code.
- Implements a WebSocket client that can connect as either `host` or `player`, tracks the
  opcode/seq request-response cycle, and exposes typed helpers for the documented object
  operations (create/update/get/lock/relay) and any host/player lifecycle events.
- Surfaces room-not-found / room-locked / room-full conditions distinctly, matching the
  documented HTTP status codes and WebSocket error semantics.

## Constraints

- Do not fabricate documentation content you could not fetch or read.
- Do not remove existing repository files (README, LICENSE, `.gitignore`) unless they directly
  conflict with the generated SDK.
- Keep generated code idiomatic for the chosen language and free of unused scaffolding.
