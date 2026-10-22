#include "core/update.hpp"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <regex>
#include <sstream>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winhttp.h>
#endif

namespace setup
{
    namespace
    {
        // GitHub's API: GET /repos/<owner>/<name>/releases/latest (a user agent is required).
        std::string fetchLatest()
        {
            // Testing: OPENMW3DS_SETUP_REPOSITORY asks another repository (the real one has no release yet).
            const char* override = std::getenv("OPENMW3DS_SETUP_REPOSITORY");
            const std::string path
                = std::string("/repos/") + (override != nullptr ? override : kRepository) + "/releases/latest";
#ifdef _WIN32
            std::string body;
            HINTERNET session = WinHttpOpen(L"openmw3ds-setup", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
            if (session == nullptr)
                return body;
            WinHttpSetTimeouts(session, 4000, 4000, 4000, 4000);
            HINTERNET connection = WinHttpConnect(session, L"api.github.com", INTERNET_DEFAULT_HTTPS_PORT, 0);
            const std::wstring widePath(path.begin(), path.end());
            HINTERNET request = connection == nullptr ? nullptr
                                                      : WinHttpOpenRequest(connection, L"GET", widePath.c_str(), nullptr,
                                                          WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                                          WINHTTP_FLAG_SECURE);
            if (request != nullptr
                && WinHttpSendRequest(request, L"Accept: application/vnd.github+json\r\n", DWORD(-1),
                    WINHTTP_NO_REQUEST_DATA, 0, 0, 0)
                && WinHttpReceiveResponse(request, nullptr))
            {
                DWORD available = 0;
                while (WinHttpQueryDataAvailable(request, &available) && available > 0 && body.size() < (1 << 20))
                {
                    std::string chunk(available, '\0');
                    DWORD read = 0;
                    if (!WinHttpReadData(request, chunk.data(), available, &read) || read == 0)
                        break;
                    body.append(chunk.data(), read);
                }
            }
            if (request != nullptr)
                WinHttpCloseHandle(request);
            if (connection != nullptr)
                WinHttpCloseHandle(connection);
            WinHttpCloseHandle(session);
            return body;
#else
            std::string body;
            const std::string command = "curl -s -m 5 -H 'Accept: application/vnd.github+json' 'https://api.github.com"
                + path + "' 2>/dev/null";
            if (FILE* pipe = popen(command.c_str(), "r"))
            {
                std::array<char, 4096> buffer;
                size_t got;
                while ((got = fread(buffer.data(), 1, buffer.size(), pipe)) > 0 && body.size() < (1 << 20))
                    body.append(buffer.data(), got);
                pclose(pipe);
            }
            return body;
#endif
        }

        bool parse(const std::string& version, int out[3])
        {
            return std::sscanf(version.c_str(), "%d.%d.%d", &out[0], &out[1], &out[2]) == 3;
        }
    }

    std::string latestReleaseVersion()
    {
        const std::string body = fetchLatest();
        std::smatch match;
        static const std::regex tag(R"re("tag_name"\s*:\s*"v?(\d+\.\d+\.\d+))re");
        if (std::regex_search(body, match, tag))
            return match[1].str();
        return {};
    }

    bool olderThan(const std::string& a, const std::string& b)
    {
        int x[3], y[3];
        if (!parse(a, x) || !parse(b, y))
            return false;
        for (int i = 0; i < 3; ++i)
            if (x[i] != y[i])
                return x[i] < y[i];
        return false;
    }
}
