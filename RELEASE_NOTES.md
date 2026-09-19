# roo_wifi 1.1.6

- Updated Roo dependencies in Bazel and PlatformIO: `roo_collections` 1.4.7, `roo_prefs` 1.3.2, `roo_backport` 1.2.4, and `roo_scheduler` 2.2.0.
- Updated build and test dependencies: `rules_cc` 0.2.25, `googletest` 1.18.0.bcr.1, and `roo_testing` 2.1.2, including the shared CI workflow.
- Added consolidated release history in `RELEASE_NOTES.md`.

---

# [roo_wifi 1.1.5](https://github.com/dejwk/roo_wifi/releases/tag/1.1.5)

Published 2026-08-30.

This release strengthens ESP32 Wi‑Fi connection handling, especially when switching between multiple networks or processing concurrent connection events.

### Fixed

- Correctly associates connection success and failure events with their target SSID.
- Prevents stale ESP32 scan and access-point state from overwriting an in-progress connection.
- Makes enable/disable, pause/resume, disconnect, and teardown state transitions safe and consistent.
- Clears invalid saved credentials after an authentication failure, for the affected network only.
- Clears persistent ESP32 credentials when forgetting the default network.
- Delivers scan-completed notifications even when no networks are found.
- Safely relays ESP32 Wi‑Fi events through the supplied scheduler.

### Improved

- Added lifecycle and multi-SSID regression coverage.
- Updated ESP32 Arduino integration and host-emulation documentation.
- Refreshed build, package, CI, and Roo dependency metadata.

---

# [roo_wifi 1.1.4](https://github.com/dejwk/roo_wifi/releases/tag/1.1.4)

Published 2026-02-26.

* Doxygen documentation.
* Updated dependencies. Builds cleanly.

**Full Changelog**: https://github.com/dejwk/roo_wifi/compare/1.1.3...1.1.4

---

# [roo_wifi 1.1.3](https://github.com/dejwk/roo_wifi/releases/tag/1.1.3)

Published 2026-01-06.

Updated dependencies.

**Full Changelog**: https://github.com/dejwk/roo_wifi/compare/1.1.2...1.1.3

---

# [roo_wifi 1.1.2](https://github.com/dejwk/roo_wifi/releases/tag/1.1.2)

Published 2025-11-12.

Updating dependencies only.

**Full Changelog**: https://github.com/dejwk/roo_wifi/compare/1.1.1...1.1.2

---

# [roo_wifi 1.1.1](https://github.com/dejwk/roo_wifi/releases/tag/1.1.1)

Published 2025-10-31.

Updated dependencies, added basic CI, .gitignore.

**Full Changelog**: https://github.com/dejwk/roo_wifi/compare/1.1.0...1.1.1

---

# [roo_wifi 1.1.0](https://github.com/dejwk/roo_wifi/releases/tag/1.1.0)

Published 2025-10-19.

Bazel module definition for testing.

---

# [1.0.1](https://github.com/dejwk/roo_wifi/releases/tag/1.0.1)

Published 2024-08-08.

Changed the preferences namespace, now that the library is separated out from roo_toolkit.

---

# [roo_wifi 1.0.0](https://github.com/dejwk/roo_wifi/releases/tag/1.0.0)

Published 2024-08-06.

Initial release.

---

