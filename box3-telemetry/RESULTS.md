# Source sanitization verification

Scope: project-owned firmware/server/client/tool source and documentation, including the server working source copy. Third-party SDKs and dependencies are not represented as audited project-owned code.

## Changes

- Removed deployment-specific SSID, IP/VPN addresses, server DNS/label, device MAC, USB-port name, capture interface and personal filesystem defaults from owned source.
- Private development settings live outside source and are owner-only (0600). The runner rejects readable-by-group files and non-BOX3 environment keys.
- Server bind/weather settings are provided through a private EnvironmentFile; deploy no longer assumes or modifies a site's firewall layout.
- Firmware uses provisioned IP SAN identity verification rather than a compiled deployment hostname. CA/date/peer verification and TLS 1.3 remain required. NVS schema unchanged.
- Serial diagnostics default to a private directory outside the source tree; Keychain account/SSID values are no longer printed by the connection helper.
- Historical release archives, evidence, logs and flash dumps were retained privately, not presented as sanitized source releases.

## Checks

- Rust unit tests: 8 passed; Rustfmt and Clippy with warnings denied passed.
- tmux client tests: 5 passed.
- Private configuration/audit regression tests: 7 passed.
- Python source parsed successfully.
- Zephyr firmware compiled successfully. It has **not** been flashed as part of this source-only change.
- Existing server certificate accepts the privately configured IP and rejects an unrelated documentation IP under OpenSSL verification. This checks certificate compatibility, not a hardware TLS run of the new firmware.
- Strict public-source export scan and known-private-key byte scan must pass before an archive is generated.

## Boundaries

No service binary was replaced or restarted, no device provisioning/flash operation was performed, and no CA/key rotation was attempted. Running binaries still correspond to the prior deployment. A later deployment must use the supplied private configuration and its EnvironmentFile.

The scanner detects specified patterns and configured private values, not every conceivable unknown/encoded secret. Source sanitization does not revoke leaked credentials or erase external copies. Old local artifacts and operational PKI/NVS remain private and must not be shared.
