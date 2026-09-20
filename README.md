# MROW

This repository contains the three main parts of the project:

- `Mixxx/` — the project's Mixxx fork and Mixxx-specific changes.
- `RPI/` — Raspberry Pi application code, scripts, configuration, and tests.
- `harness/` — local DJ agent: Google Cloud Gemini model selection, song suggestions,
  adaptive setlists, and crowd feedback, integrated into BiteDJ's Assist tab.

Each project area has its own README with a more detailed layout.

The rebuilt Mixxx fork starts its embedded agent automatically on the Pi; no
browser, localhost URL or separate service is needed. Open **Assist → Models**
for runtime Google Cloud Gemini setup, or optionally [provision keys and models during
build/deploy](harness/README.md#optional-builddeploy-provisioning).

Agent documentation:

- [Operator workflow, key provisioning and troubleshooting](harness/README.md)
- [Native integration and command protocol](harness/docs/mixxx-integration.md)
- [Pi build instructions](RPI/bitedj_docs/build.md) and [deployment](RPI/README.md#deploying-from-a-workstation)
- [Verification results and remaining hardware checks](harness/docs/verification.md)
