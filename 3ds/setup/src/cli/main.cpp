// The setup app's command-line tool: the same preparation as the window, for scripts and testing.
//
//     openmw3ds-setup-cli --morrowind "<Morrowind folder>" --output <folder> --port <port files> [--version 0.1.1]
//
// Developer commands (the texture cooker alone, against the port's Python cooker):
//
//     openmw3ds-setup-cli cook --morrowind <folder> --port <port files> --out <pack> [--limit N] [--threads N]
//     openmw3ds-setup-cli dump --morrowind <folder> --port <port files> --key <texture key> --out <folder>

#include "core/cooker.hpp"
#include "core/prepare.hpp"

#include <chrono>
#include <cstdio>
#include <exception>
#include <map>
#include <string>

namespace
{
    int usage()
    {
        std::fputs("usage: openmw3ds-setup-cli --morrowind <Morrowind folder> --output <folder> --port <port files> "
                   "[--version <version>]\n"
                   "       openmw3ds-setup-cli cook --morrowind <folder> --port <port files> --out <pack> "
                   "[--limit N] [--threads N]\n"
                   "       openmw3ds-setup-cli dump --morrowind <folder> --port <port files> --key <key> "
                   "--out <folder>\n",
            stderr);
        return 1;
    }

    void printProgress(const setup::Progress& p)
    {
        static std::string lastStep;
        static int lastPercent = -1;
        const int percent = int(p.fraction * 100.0);
        if (p.step == lastStep && percent == lastPercent)
            return;
        lastStep = p.step;
        lastPercent = percent;
        std::fprintf(stderr, "\r%3d%%  %-40s", percent, p.step.c_str());
        if (p.fraction >= 1.0)
            std::fputc('\n', stderr);
    }
}

// Wide arguments on Windows (-municode): the narrow ones are in the ANSI code page, which can't hold every folder
// name.
#ifdef _WIN32
using ArgChar = wchar_t;
#define ARG(text) L##text
int wmain(int argc, wchar_t** argv)
#else
using ArgChar = char;
#define ARG(text) text
int main(int argc, char** argv)
#endif
{
    std::basic_string<ArgChar> command;
    int first = 1;
    if (argc > 1 && argv[1][0] != ArgChar('-'))
    {
        command = argv[1];
        first = 2;
    }
    std::map<std::basic_string<ArgChar>, std::filesystem::path> args;
    for (int i = first; i < argc; ++i)
    {
        if (i + 1 >= argc)
            return usage();
        args[argv[i]] = argv[i + 1];
        ++i;
    }
    const auto get = [&](const ArgChar* name) {
        const auto found = args.find(name);
        return found == args.end() ? std::filesystem::path() : found->second;
    };

    try
    {
        if (command.empty())
        {
            setup::Options options;
            options.morrowind = get(ARG("--morrowind"));
            options.output = get(ARG("--output"));
            options.port = get(ARG("--port"));
            options.version = args.count(ARG("--version")) ? get(ARG("--version")).string() : "0.1.1";
            if (options.morrowind.empty() || options.output.empty() || options.port.empty())
                return usage();
            setup::prepare(options, printProgress);
            return 0;
        }

        setup::CookOptions cook;
        cook.dataFiles = get(ARG("--morrowind")) / "Data Files";
        cook.engineVfs = get(ARG("--port")) / "openmw" / "resources" / "vfs";
        cook.portInterface = get(ARG("--port")) / "openmw" / "ui3ds";
        cook.out = get(ARG("--out"));
        if (args.count(ARG("--limit")))
            cook.limit = std::stoul(get(ARG("--limit")).string());
        cook.keyList = get(ARG("--keys"));
        if (args.count(ARG("--threads")))
            cook.threads = std::stoi(get(ARG("--threads")).string());
        if (get(ARG("--morrowind")).empty() || get(ARG("--port")).empty() || cook.out.empty())
            return usage();

        if (command == ARG("cook"))
        {
            const auto start = std::chrono::steady_clock::now();
            const setup::CookSummary summary = setup::cookTextures(cook, [](size_t done, size_t total) {
                if (done % 100 == 0 || done == total)
                    std::fprintf(stderr, "\r  %zu / %zu", done, total);
            });
            const double seconds
                = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
            std::fprintf(stderr, "\n%zu textures (%zu in the UI atlas), %.1f MB in %.1f s\n", summary.textures,
                summary.atlasImages, double(summary.bytes) / 1048576.0, seconds);
            return 0;
        }
        if (command == ARG("dump"))
        {
            setup::dumpTextureSteps(cook, get(ARG("--key")).generic_string(), cook.out);
            return 0;
        }
        return usage();
    }
    catch (const std::exception& e)
    {
        std::fprintf(stderr, "\n%s\n", e.what());
        return 2;
    }
}
