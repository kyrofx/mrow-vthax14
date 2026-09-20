# Mixxx

`bitedj/` contains the BiteDJ Mixxx fork. Its **Assist** tab uses the built-in
agent for library sync, suggestions, rolling setlists and crowd feedback.
Agent sources are embedded as Qt resources at build time; Mixxx starts a private
Python 3.9+ worker and owns its lifetime. No web server or extra launch is needed.

Open **Assist → Models** to enter an OpenRouter key and select models at runtime,
or provision a private configuration during build/deploy. See [the agent guide](../harness/README.md) and
[the integration contract](../harness/docs/mixxx-integration.md).

The Pi build environment is defined in `bitedj/compose.yaml`; the device build
instructions are in [build.md](../RPI/bitedj_docs/build.md).
Build from this MROW checkout, or set `BITEDJ_AGENT_SOURCE_DIR` to its
`harness/src` directory at CMake configure time. No credentials enter CMake.
