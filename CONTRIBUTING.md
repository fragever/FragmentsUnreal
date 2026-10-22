# Contributing to FragmentsUnreal

Thank you for helping improve open interoperability for AECO workflows.

## Issues and pull requests

Search existing issues first. Include Unreal Engine version, target platform,
plugin revision, model characteristics, reproduction steps, logs, and a minimal
sample when safe. Do not upload confidential BIM models or personal data.

For pull requests:

1. Keep the change focused and explain the user-visible result.
2. Update documentation and tests when behavior or public APIs change.
3. Run relevant Unreal automation tests and a representative import.
4. Describe frame-time, memory, import-time, threading, or compatibility impact.
5. Preserve third-party code and notices unless an intentional update is
   documented.

Follow Unreal Engine naming, reflection, module, threading, and game-thread
rules. Keep expensive work off the game thread and route UObject/render-state
operations safely.

## Developer Certificate of Origin

Contributions are accepted under Apache License 2.0. By submitting a pull
request, you certify that you have the right to submit the work and agree to the
[Developer Certificate of Origin](https://developercertificate.org/). Sign
commits with:

```text
Signed-off-by: Your Name <you@example.com>
```

Use GitHub Issues for reproducible bugs and feature proposals. Use
[SECURITY.md](SECURITY.md) for vulnerabilities.
