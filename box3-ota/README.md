# BOX3 OTA stage-2 foundation

This module is a service/storage foundation, **not a deployable OTA capability**. It has no artifact import, signing, install, flash, manifest, download, or device-event HTTP endpoint. The device Unix API deliberately fails closed until the existing Rust mTLS edge has a reviewed certificate-to-device mapping. MCUboot/revert/watchdog/NVS behavior and full-image transport remain hardware-in-the-loop rollout blockers.

## Runtime contract

Build `./cmd/box3-ota` and run:

```text
box3-ota --config /absolute/path/config.json
```

There are no environment-variable or network-address defaults. Configuration is strict JSON; unknown fields fail startup. `config.example.json` contains placeholders and is not directly runnable.

The process owns:

- one SQLite database using foreign keys, WAL, `synchronous=FULL`, ordered migrations, and a bounded busy timeout;
- one admin HTTP listener and one Unix-domain HTTP listener;
- the Unix socket inode, which is removed on shutdown only if it is still the inode created by this process.

The state and runtime directories must already have deployment-appropriate ownership and mode `0750` or tighter. The socket receives the runtime directory's numeric group and mode `0660`; the container service user must be permitted to set that group. A host edge process needs membership in that same numeric group. Do not replace this with world-write, root, privileged mode, or a Docker socket mount.

SQLite WAL means a live backup must not copy only the `.db` file. No production backup workflow is implemented in this milestone; a tested consistent backup and restore drill is a rollout prerequisite.

## Admin boundary

The normal admin bind is a literal loopback address. Authentication is always required for `/admin/` using `Authorization: Bearer ...`; the token is read from a regular external file that is owner/group-readable, not writable, and inaccessible to others. Exact `Host` values are allowlisted. A present `Origin` must exactly match its allowlist. Forwarded identity headers are rejected because this milestone has no trusted reverse proxy.

The generic configuration supports an explicitly enabled container wildcard for future bridge deployments, but the supplied Docker deployment **forbids** it with `--require-loopback`. Both Dockerfile and Compose use this guard. The host-network deployment binds directly to the configured literal loopback IP/port; the exact Host allowlist and authentication remain enforced.

Read-only pages are:

- `GET /admin/devices`
- `GET /admin/releases` (honestly lists only opaque target references observed in jobs; it is not a release catalog)
- `GET /admin/jobs/{id}`

No HTTP method creates, assigns, cancels, imports, confirms, or installs anything.

## Docker deployment

`Dockerfile` pins Go 1.26.9, runs fmt/vet/race tests in the build, and copies only a static binary into a scratch runtime. `compose.yaml` runs UID/GID `10001:10001`, drops all capabilities, uses a read-only root, bounded logs/resources and a readiness healthcheck. No shell, Docker socket, PKI or signing key is mounted. The supplied Compose uses host networking because the deployment host has IPv4 forwarding disabled, like the existing weather container. Build steps use host networking to fetch checksum-verified modules; runtime binds **only** to configured host loopback under the `--require-loopback` guard. There are no published-port/NAT rules and no changes to firewall/sysctl. Host networking is not network isolation: retain auth and restrictive socket/file permissions, and do not remove the loopback guard.

1. Reserve UID/GID 10001 for this service (check for collisions before creating a non-login system account). Create persistent state and runtime directories owned by UID 10001, modes `0750`; runtime directory group must equal `BOX3_OTA_SOCKET_GID`, and that numeric group is added to the container. Do not add the existing edge to it yet: the certificate bridge is not implemented.
2. Outside the checkout, copy `config.docker.example.json`, replace the host-port placeholders, and create an independent random admin token of at least 32 bytes. Keep the configuration mode `0640` root:10001 and token mode `0440` root:10001. This token is not a signing key. Do not put its value into environment files, command arguments, images or logs.
3. Populate `deploy.env.example` outside the checkout (mode `0600`); all four mount sources must exist. The container config paths are fixed by Compose, not the host path names. Keep `shutdown_timeout` at most 30s, below Compose's 40s grace.
4. Persist runtime directory creation across host boots, e.g. `/etc/tmpfiles.d/box3-ota.conf`: `d /run/box3-ota 0750 10001 NUMERIC_SOCKET_GID -`. Apply that one file with `systemd-tmpfiles --create /etc/tmpfiles.d/box3-ota.conf`. This is only directory provisioning; the webserver runs in Docker, not systemd.
5. As an authorized Docker administrator, from this directory:

```sh
docker compose --env-file /absolute/private/deploy.env config --quiet
docker compose --env-file /absolute/private/deploy.env build
docker compose --env-file /absolute/private/deploy.env up -d --no-build
docker compose --env-file /absolute/private/deploy.env ps
```

The healthcheck executes `box3-ota --config /etc/box3-ota/config.json --check-ready` over the Unix socket. Verify unauthenticated `/admin/devices` returns 401, authenticated access returns 200, disallowed Host/Origin returns 403, and `/v1/ota/check` remains closed. Verify the existing telemetry and model independently before and after startup.

Use an SSH tunnel to the selected host loopback port. Authentication currently requires a Bearer header (e.g. a header-capable client); browser login/forms are not implemented. Never put the token in a URL. A plain browser visit without the header correctly returns 401.

For rollback, stop **only this Compose project**, preserve the state/config/token and original image, then start a previously validated image compatible with the schema. Never use `down -v` as an update/rollback procedure. A database from a newer schema is rejected rather than silently downgraded. A cleanly stopped SQLite database can be backed up with the entire private state directory; never copy only a live WAL database. Backup/restore HIL and abrupt host-reboot acceptance remain separate gates. This deployment does not restart or reconfigure telemetry, the model, VPN, DNS or firewall. The direct host-loopback bind avoids Docker bridge localhost-publication/NAT caveats. Loopback binding still is not a substitute for authentication.

## Health and shutdown

Both listeners expose `GET /health/live` and `GET /health/ready`. Liveness reports only that the process can answer. Readiness requires both listeners to have been established, a bounded SQLite query, and writable state/runtime directories; it becomes `503` before draining. Health never claims device freshness, artifact validity, MCUboot readiness, or OTA capability.

On `SIGINT`/`SIGTERM`, readiness is disabled, both servers shut down concurrently within `shutdown_timeout`, the Unix socket is cleaned up safely, and SQLite closes last. A forced timeout returns a non-zero exit.

## Durable state contract

Job mutations are Go APIs used by tests only; they are not wired to HTTP. Job creation is idempotent by `request_id` only when every immutable input matches. A partial unique index allows one active job per device. Event identity is `(job_id, boot_id, event_sequence)`; exact replay is a no-op and conflicting replay fails. Lifecycle transitions are explicit. Download and verification never imply confirmation. `boot_testing` requires the expected image hash and a boot identity different from the assignment's pre-reboot boot identity. Confirmation requires that same tested boot identity and image hash. New events must have a strictly increasing sequence within each job/boot; exact historical replays remain no-ops. Unknown/offline device state remains representable and is never converted to failure based on wall-clock timeout.
