# STUN server (external prerequisite — no binaries committed)

The game client performs NAT traversal against a STUN server on UDP 3478/3479.
The HTTP API hands the client our own STUN server via
`battle/get_stun_server_info` (configured with `STUN_HOST` / `STUN_PORTS` in
`.env`).

This repo carries wiring and config only. The reference deployment used
**STUNTMAN `stunserver`** (Apache-2.0, <https://www.stunprotocol.org/>):

- Linux: `apt install stuntman-server` or build from source.
- Windows: build with Cygwin/MinGW from the STUNTMAN source release.

A basic-mode config template is provided at `stun/stun.conf.example`
(UDP, IPv4 + IPv6, ports 3478/3479). Run e.g.:

```
stunserver --config stun/stun.conf.example
```

Verifying that the server answers binding requests on 3478/3479 is a manual
step done in the analysis VM with the real binary — not covered by CI.
