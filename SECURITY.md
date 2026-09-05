# Security

This app manages local sign-in sessions. Report security issues privately to the repository maintainer; do not attach real auth files, encrypted profiles, account identifiers, sign-in URLs or unredacted logs.

Windows profiles use DPAPI. macOS and Linux profiles use AES-256-GCM with a per-user key in Keychain or Secret Service. Linux has no plaintext-key fallback. Losing the OS key store means signing in again; copying profile files between operating systems does not transfer usable sessions.

Active Codex `auth.json` and rollback backups contain credentials in Codex's normal format. Login temporarily writes auth data into a private directory. Normal shutdown cleans it up; an OS crash or forced process kill may leave a directory behind.

Only `auth.openai.com` HTTPS login links returned by the local App Server are opened automatically. The app sends a small set of local JSON-RPC requests, not workspace documents. The installed Codex CLI controls its own network communication.

Windows CLI candidates require a valid OpenAI Authenticode signature. Unix candidates must be executable files owned by the user or root and not writable by group/others. This is not a publisher signature guarantee on Linux; install the CLI from OpenAI's official distribution. macOS desktop discovery requires the OpenAI signing team.

Do not publish `data/`, build output, authentication files, OS credential-store keys or local caches. Use a clean checkout and the CMake install target to prepare distributable artifacts.

The macOS/Linux ports require native validation before their first release. Build success alone does not validate Keychain/Secret Service access, browser callbacks, desktop shutdown or account switching.
