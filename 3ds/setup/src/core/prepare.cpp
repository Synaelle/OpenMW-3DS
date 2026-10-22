#include "core/prepare.hpp"

#include "core/bsawriter.hpp"
#include "core/cooker.hpp"
#include "core/esm.hpp"
#include "core/fallbacks.hpp"

#include <fstream>
#include <stdexcept>

namespace setup
{
    namespace fs = std::filesystem;

    namespace
    {
        // The content the port loads in 0.1.0: Morrowind alone (the expansions come with 0.2.0).
        const std::vector<std::string> kContent = { "Morrowind.esm" };
        const std::vector<std::string> kArchives = { "Morrowind.bsa" };
        // Folders the game would create on first use: creating a folder takes ~1.5 s on the console's SD card.
        const std::vector<std::string> kEmptyFolders = { "cache", "saves", "screenshots", "logs" };

        // Each step's share of the whole, roughly its time (the textures take about half a minute).
        constexpr double kShareCheck = 0.01, kSharePort = 0.03, kShareConfig = 0.01, kShareSound = 0.15,
                         kShareData = 0.15, kShareTextures = 0.65;

        void writeLines(const fs::path& path, const std::vector<std::string>& lines)
        {
            std::ofstream out(path, std::ios::binary | std::ios::trunc);
            if (!out)
                throw std::runtime_error("can't write " + path.string());
            for (const std::string& line : lines)
                out << line << '\n';
            if (!out.flush())
                throw std::runtime_error("writing " + path.string() + " failed");
        }

        // Copies with a progress callback in bytes (fs::copy_file has none).
        void copyFile(const fs::path& from, const fs::path& to, const std::function<void(uint64_t)>& done)
        {
            std::ifstream in(from, std::ios::binary);
            if (!in)
                throw std::runtime_error("can't read " + from.string());
            const fs::path part = to.string() + ".part";
            {
                std::ofstream out(part, std::ios::binary | std::ios::trunc);
                if (!out)
                    throw std::runtime_error("can't write " + part.string());
                std::vector<char> buffer(4 << 20);
                uint64_t copied = 0;
                while (in)
                {
                    in.read(buffer.data(), std::streamsize(buffer.size()));
                    const std::streamsize got = in.gcount();
                    if (got <= 0)
                        break;
                    out.write(buffer.data(), got);
                    copied += uint64_t(got);
                    done(copied);
                }
                if (!out.flush())
                    throw std::runtime_error("writing " + part.string() + " failed");
            }
            fs::rename(part, to);
        }
    }

    std::vector<std::string> checkMorrowind(const fs::path& morrowind)
    {
        std::vector<std::string> problems;
        const fs::path dataFiles = morrowind / "Data Files";
        if (!fs::is_directory(dataFiles))
        {
            problems.push_back("No \"Data Files\" folder in " + morrowind.string() + ".");
            return problems;
        }
        for (const std::vector<std::string>* list : { &kContent, &kArchives })
            for (const std::string& file : *list)
                if (!fs::is_regular_file(dataFiles / file))
                    problems.push_back(file + " is missing from Data Files.");
        for (const char* folder : { "Sound", "Music" })
            if (!fs::is_directory(dataFiles / folder))
                problems.push_back(std::string("The ") + folder + " folder is missing from Data Files.");
        if (!fs::is_regular_file(morrowind / "Morrowind.ini"))
            problems.push_back("Morrowind.ini is missing.");
        // The English game only in 0.1.0 (other languages later: docs/PLANNED_FEATURES.md).
        if (problems.empty())
        {
            const std::string language = esmLanguage(dataFiles / "Morrowind.esm");
            if (language.empty())
                problems.push_back("This Morrowind's language isn't recognised: this version needs the English game.");
            else if (language != "English")
                problems.push_back("This Morrowind is in " + language
                    + ": this version needs the English game. Other languages come in a later version.");
        }
        return problems;
    }

    void prepare(const Options& options, const std::function<void(const Progress&)>& progress)
    {
        double base = 0;
        const auto report = [&](const std::string& step, double withinStep, double share) {
            if (options.cancel != nullptr && options.cancel->load())
                throw Cancelled();
            if (progress)
                progress({ step, base + share * withinStep });
        };

        report("Checking the Morrowind folder", 0, kShareCheck);
        const std::vector<std::string> problems = checkMorrowind(options.morrowind);
        if (!problems.empty())
            throw std::runtime_error(problems.front());
        const fs::path dataFiles = options.morrowind / "Data Files";
        const fs::path openmw = options.output / "openmw";
        for (const std::string& folder : kEmptyFolders)
            fs::create_directories(openmw / folder);
        fs::create_directories(openmw / "config");
        fs::create_directories(openmw / "data");
        fs::create_directories(openmw / "cooked");
        base += kShareCheck;

        // The port's own files (the engine's resources, the 3DS interface, settings, the CIA), as the release ships
        // them.
        report("Copying the port's files", 0, kSharePort);
        if (!fs::is_directory(options.port))
            throw std::runtime_error("The port's files are missing: " + options.port.string());
        // The default output is the port folder itself (the game's files are added to it): nothing to copy then.
        std::error_code same;
        if (!fs::equivalent(options.port, options.output, same))
            fs::copy(options.port, options.output,
                fs::copy_options::recursive | fs::copy_options::overwrite_existing);
        base += kSharePort;

        // The player's openmw.cfg: the data folders, the content, the encoding and Morrowind.ini's fallback values.
        report("Reading Morrowind.ini", 0, kShareConfig);
        std::vector<std::string> cfg = {
            "# OpenMW 3DS user configuration (written by the setup app)",
            "data=\"/openmw/data\"",
            "data=\"/openmw/ui3ds\"",
        };
        for (const std::string& content : kContent)
            cfg.push_back("content=" + content);
        for (const std::string& archive : kArchives)
            cfg.push_back("fallback-archive=" + archive);
        cfg.push_back("fallback-archive=sound3ds.bsa");
        cfg.push_back("encoding=win1252");
        cfg.push_back("# fallback values imported from Morrowind.ini by the setup app");
        for (std::string& line : readFallbacks(options.morrowind / "Morrowind.ini"))
            cfg.push_back(std::move(line));
        writeLines(openmw / "config" / "openmw.cfg", cfg);
        base += kShareConfig;

        report("Packing the sounds", 0, kShareSound);
        packSoundArchive(dataFiles, { "Sound", "Music" }, openmw / "data" / "sound3ds.bsa",
            [&](uint64_t done, uint64_t total) {
                report("Packing the sounds", total ? double(done) / double(total) : 1.0, kShareSound);
            });
        base += kShareSound;

        // The game's own files, copied as they are.
        uint64_t total = 0, copiedBefore = 0;
        std::vector<std::string> files = kContent;
        files.insert(files.end(), kArchives.begin(), kArchives.end());
        for (const std::string& file : files)
            total += fs::file_size(dataFiles / file);
        for (const std::string& file : files)
        {
            const std::string step = "Copying " + file;
            copyFile(dataFiles / file, openmw / "data" / file,
                [&](uint64_t copied) { report(step, double(copiedBefore + copied) / double(total), kShareData); });
            copiedBefore += fs::file_size(dataFiles / file);
        }
        base += kShareData;

        // The textures, cooked for the 3DS's GPU: the game's, OpenMW's own and the port's interface.
        report("Preparing the textures", 0, kShareTextures);
        CookOptions cook;
        cook.dataFiles = dataFiles;
        cook.archives = kArchives;
        cook.engineVfs = options.port / "openmw" / "resources" / "vfs";
        cook.portInterface = options.port / "openmw" / "ui3ds";
        cook.out = openmw / "cooked" / "textures.o3tp";
        cook.cancel = options.cancel;
        // Called from the cooking threads: only reports, cancelling goes through cook.cancel.
        cookTextures(cook, [&](size_t done, size_t total) {
            if (progress)
                progress({ "Preparing the textures",
                    base + kShareTextures * (total ? double(done) / double(total) : 1.0) });
        });
        if (options.cancel != nullptr && options.cancel->load())
            throw Cancelled();
        base += kShareTextures;

        writeLines(openmw / "VERSION", { options.version });
        if (progress)
            progress({ "Done", 1.0 });
    }
}
