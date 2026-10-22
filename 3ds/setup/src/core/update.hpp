#pragma once

#include <string>

namespace setup
{
    // The project's GitHub repository (owner/name): its releases page and the update check.
    inline constexpr const char* kRepository = "Synaelle/OpenMW-3DS";

    // The latest release's version on GitHub ("0.1.1", without the tag's "v"), or empty when it can't be told (no
    // connection, no release yet). Blocks for a few seconds at most: call it from a thread.
    std::string latestReleaseVersion();

    // Whether version a is older than b (both major.minor.micro).
    bool olderThan(const std::string& a, const std::string& b);
}
