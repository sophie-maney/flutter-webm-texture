# Security policy

`webm_texture` decodes stickers that come from other users, so it treats
every sticker byte as hostile. The threat model, the mitigations, the latest
review's findings and the fuzzing results are in
[`docs/security.md`](docs/security.md).

## Reporting a vulnerability

Please don't open a public issue. Report it privately through GitHub's
private vulnerability reporting ("Report a vulnerability" under this
repository's **Security** tab), with a sample file if you have one and the
platform, device and plugin version. See
[`docs/security.md`](docs/security.md#reporting-a-vulnerability) for what
happens next.

## Supported versions

Security fixes go into the latest release.
