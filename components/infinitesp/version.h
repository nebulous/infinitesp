#pragma once
// InfinitESP firmware version, CalVer (YYYY.M.P). Authoritative identifier of the
// running build: surfaced in the boot log and in REPORT?. Advanced automatically by the
// pre-commit hook on any push that changes components/. Lives here in the component, not
// the yaml, so it reaches every user on pull+recompile regardless of how they have edited
// their local config.
#define INFINITESP_VERSION "2026.8.1"
