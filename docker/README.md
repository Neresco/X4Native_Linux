# Experimental GPU-free X4 host image

See [host setup and environment reference](../docs/HEADLESS_HOST.md),
[design decisions](../docs/MULTIPLAYER_DESIGN.md),
[join integration](../docs/MULTIPLAYER_JOIN.md) and
[validation/gaps](../docs/VALIDATION.md).

Build from the native repository root with `docker/Dockerfile`. The image
contains public native/mod sources and OS dependencies; it acquires licensed game
resources at runtime into private volumes. It is not an anonymous dedicated-server
download, and no licensed game files/authentication state belong in the build context.

The Compose example defaults to a bounded non-root, read-only, CPU-only run with
no published ports, host devices or restart policy. Operator `.env`/secret setup
is required. Player-facing join/host/admin interactions remain in X4 clients.
