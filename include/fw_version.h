/*
 * fw_version.h: the build's firmware version string.
 *
 * Reported in the /status heartbeat as `fw_version`, where the server compares
 * it against the latest available build to decide whether an update can be
 * offered, and printed in the setup portal's footer so a support request can
 * quote what is actually running.
 *
 * The value is stamped by CI via -DTESSERAE_FW_VERSION=\"...\" (see
 * .github/workflows/): push builds get an auto-incrementing "0.0.<build>",
 * tagged releases get the tag's semantic version with the leading "v" stripped
 * (tag v1.2.0 -> "1.2.0"), matching tesserae-device-firmware. A local build with
 * no stamp reports "0.0.0-dev", which the server treats as "never offer an
 * update to this".
 *
 * Plain SemVer with NO leading "v" is part of the contract: the server parses
 * it, and a "v" prefix silently fails the comparison rather than erroring.
 */
#pragma once

#ifdef TESSERAE_FW_VERSION
#define FW_VERSION  TESSERAE_FW_VERSION
#else
#define FW_VERSION  "0.0.0-dev"
#endif
