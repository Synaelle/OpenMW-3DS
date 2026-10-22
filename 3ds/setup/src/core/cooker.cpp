#include "core/cooker.hpp"

#include "core/bleed.hpp"
#include "core/decode.hpp"
#include "core/image.hpp"
#include "core/pica.hpp"
#include "core/resample.hpp"
#include "core/text.hpp"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <mutex>
#include <optional>
#include <regex>
#include <set>
#include <stdexcept>
#include <thread>

// The port's tools/cook_textures.py in C++, step for step (see its docstring for the why of each choice).

namespace setup
{
    namespace fs = std::filesystem;

    namespace
    {
        constexpr uint32_t kVersion = 4;
        constexpr size_t kHeaderSize = 32;
        constexpr size_t kEntrySize = 32;
        constexpr size_t kDataAlign = 128;

        constexpr uint8_t kFlagAlpha = 1;
        constexpr uint8_t kFlagAlphaBinary = 2;
        constexpr uint8_t kFlagPadded = 4;
        constexpr uint8_t kFlagAtlas = 8;
        constexpr uint8_t kFlagAtlasTransposed = 16;
        constexpr uint8_t kFlagAtlasHalved = 32;

        const std::string kAtlasHalvedPrefix = "textures/menu_icon_";
        const std::vector<std::string> kUiPrefixes
            = { "icons/", "bookart/", "textures/omw_", "textures/levelup/", "textures/birthsigns/" };
        const std::vector<std::string> kUiTextureNames
            = { "menu_", "tx_menu", "tx_cursor", "compass", "target", "scroll", "detect_", "menu" };
        constexpr int kUiMaxSize = 256;
        const std::map<std::string, int> kUiBig = { { "textures/menu_morrowind", 512 } };
        const std::string kAtlasKey = "ui-atlas/";
        constexpr int kAtlasWidth = 512;
        constexpr int kAtlasHeight = 256;
        constexpr int kAtlasGutter = 1;
        const std::vector<std::string> kAtlasExcluded
            = { "icons/", "bookart/", "textures/levelup/", "textures/birthsigns/" };
        constexpr int kAtlasMaxSide = 512;
        constexpr int kAtlasMaxArea = 128 * 64;
        const std::vector<std::string> kExcluded = { "textures/omw_map_color_palette", "textures/omw/water_nm" };
        const std::string kGlyphPrefix = "textures/omw_steam_button_";
        constexpr int kGlyphSize = 32;
        const std::vector<std::string> kPrefixes = { "textures/", "icons/", "bookart/" };
        const std::vector<std::string> kImageExtensions = { ".dds", ".tga", ".bmp", ".png", ".jpg", ".jpeg" };
        constexpr int kMaxSize = 256;

        const char* const kCodeTextures[] = {
#include "ui_code_textures.inc"
        };

        bool startsWith(const std::string& s, const std::string& prefix)
        {
            return s.compare(0, prefix.size(), prefix) == 0;
        }

        bool startsWithAny(const std::string& s, const std::vector<std::string>& prefixes)
        {
            return std::any_of(prefixes.begin(), prefixes.end(), [&](const std::string& p) { return startsWith(s, p); });
        }

        bool endsWith(const std::string& s, const std::string& suffix)
        {
            return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
        }

        bool endsWithAny(const std::string& s, const std::vector<std::string>& suffixes)
        {
            return std::any_of(suffixes.begin(), suffixes.end(), [&](const std::string& x) { return endsWith(s, x); });
        }

        std::string normalize(std::string path)
        {
            std::replace(path.begin(), path.end(), '\\', '/');
            return toLowerAscii(path);
        }

        // os.path.splitext's root: the path without its last extension (a leading dot of the name isn't one).
        std::string stemKey(const std::string& path)
        {
            const size_t slash = path.rfind('/');
            const size_t nameStart = slash == std::string::npos ? 0 : slash + 1;
            size_t dot = path.rfind('.');
            if (dot == std::string::npos || dot < nameStart)
                return path;
            size_t firstNonDot = nameStart;
            while (firstNonDot < path.size() && path[firstNonDot] == '.')
                ++firstNonDot;
            if (dot < firstNonDot)
                return path;
            return path.substr(0, dot);
        }

        uint64_t fnv1a64(const std::string& text)
        {
            uint64_t h = 0xCBF29CE484222325ull;
            for (const char c : text)
            {
                h ^= uint8_t(c);
                h *= 0x100000001B3ull;
            }
            return h;
        }

        struct Source
        {
            fs::path file;
            uint64_t offset = 0;
            uint64_t size = 0;
        };

        uint32_t get32(const uint8_t* p)
        {
            return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
        }

        std::map<std::string, Source> readBsaIndex(const fs::path& path)
        {
            std::ifstream f(path, std::ios::binary);
            if (!f)
                throw std::runtime_error("can't read " + path.string());
            uint8_t header[12];
            f.read(reinterpret_cast<char*>(header), 12);
            const uint32_t version = get32(header), hashOffset = get32(header + 4), count = get32(header + 8);
            if (!f || version != 0x100)
                throw std::runtime_error(path.string() + " is not a Morrowind archive");
            std::vector<uint8_t> records(size_t(count) * 8), nameOffsets(size_t(count) * 4);
            f.read(reinterpret_cast<char*>(records.data()), std::streamsize(records.size()));
            f.read(reinterpret_cast<char*>(nameOffsets.data()), std::streamsize(nameOffsets.size()));
            std::vector<char> names(size_t(hashOffset) - 12 * size_t(count));
            f.read(names.data(), std::streamsize(names.size()));
            if (!f)
                throw std::runtime_error(path.string() + " is truncated");
            const uint64_t dataStart = 12 + uint64_t(hashOffset) + 8 * uint64_t(count);
            std::map<std::string, Source> files;
            for (uint32_t i = 0; i < count; ++i)
            {
                const uint32_t size = get32(&records[i * 8]), offset = get32(&records[i * 8 + 4]);
                const uint32_t nameOffset = get32(&nameOffsets[i * 4]);
                const char* name = &names.at(nameOffset);
                files[normalize(name)] = { path, dataStart + offset, size };
            }
            return files;
        }

        // name -> source, the VFS order: OpenMW's resources, the game's archives, its loose files, the port's
        // interface. Then one texture per key, .dds preferred.
        std::map<std::string, std::pair<std::string, Source>> collectSources(const CookOptions& options)
        {
            std::map<std::string, Source> sources;
            const auto walk = [&](const fs::path& folder) {
                if (folder.empty() || !fs::is_directory(folder))
                    return;
                for (const fs::directory_entry& item : fs::recursive_directory_iterator(folder))
                {
                    if (!item.is_regular_file())
                        continue;
                    const std::string name = normalize(fs::relative(item.path(), folder).generic_string());
                    if (startsWithAny(name, kPrefixes) && endsWithAny(name, kImageExtensions))
                        sources[name] = { item.path(), 0, uint64_t(item.file_size()) };
                }
            };
            walk(options.engineVfs);
            for (const std::string& archive : options.archives)
                for (auto& [name, source] : readBsaIndex(options.dataFiles / archive))
                    if (startsWithAny(name, kPrefixes) && endsWithAny(name, kImageExtensions))
                        sources[name] = source;
            walk(options.dataFiles);
            walk(options.portInterface);

            std::map<std::string, std::pair<std::string, Source>> byKey;
            for (const auto& [name, source] : sources)
            {
                const std::string key = stemKey(name);
                if (startsWithAny(key, kExcluded))
                    continue;
                const auto found = byKey.find(key);
                if (found == byKey.end() || endsWith(name, ".dds"))
                    byKey[key] = { name, source };
            }
            return byKey;
        }

        // The textures the MyGUI skins and layouts reference (texture="..."), and the ones the interface code names
        // (embedded): only these go in the UI atlas.
        std::set<std::string> referencedTextures(const std::vector<fs::path>& folders)
        {
            std::set<std::string> names;
            for (const char* name : kCodeTextures)
                names.insert(name);
            const std::regex pattern(R"re(texture\s*=\s*"([^"]+)")re", std::regex::icase);
            for (const fs::path& folder : folders)
            {
                if (!fs::is_directory(folder))
                    continue;
                for (const fs::directory_entry& item : fs::directory_iterator(folder))
                {
                    const std::string file = item.path().filename().string();
                    if (!endsWith(file, ".xml") && !endsWith(file, ".layout") && !endsWith(file, ".skin"))
                        continue;
                    std::ifstream in(item.path(), std::ios::binary);
                    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
                    for (std::sregex_iterator it(text.begin(), text.end(), pattern), end; it != end; ++it)
                        names.insert(stemKey(normalize((*it)[1].str())));
                }
            }
            return names;
        }

        bool isUi(const std::string& name)
        {
            if (startsWithAny(name, kUiPrefixes))
                return true;
            if (startsWith(name, "textures/") && name.find('/', 9) == std::string::npos)
                return startsWithAny(name.substr(9), kUiTextureNames);
            return false;
        }

        bool isAtlasCandidate(const std::string& key, int width, int height)
        {
            if (startsWithAny(key, kAtlasExcluded))
                return false;
            return std::max(width, height) <= kAtlasMaxSide && width * height <= kAtlasMaxArea;
        }

        int pow2Size(int n, int maxSize)
        {
            int p = 8;
            while (p < n && p < maxSize)
                p *= 2;
            if (p > 8 && p > n && (p - n) > (n - p / 2))
                p /= 2;
            return std::max(8, std::min(p, maxSize));
        }

        int pow2Ceil(int n, int maxSize)
        {
            int p = 8;
            while (p < n && p < maxSize)
                p *= 2;
            return p;
        }

        std::vector<uint8_t> readSource(const Source& source)
        {
            std::ifstream f(source.file, std::ios::binary);
            if (!f)
                throw std::runtime_error("can't read " + source.file.string());
            f.seekg(std::streamoff(source.offset));
            std::vector<uint8_t> data(source.size);
            f.read(reinterpret_cast<char*>(data.data()), std::streamsize(data.size()));
            if (!f)
                throw std::runtime_error("can't read " + source.file.string());
            return data;
        }

        struct Cooked
        {
            std::string key, name;
            std::vector<uint8_t> data;
            int width = 0, height = 0, srcWidth = 0, srcHeight = 0;
            PicaFormat format = PicaFormat::Etc1;
            int levels = 0;
            uint8_t flags = 0;
            std::optional<Image> atlasTexels;
        };

        struct Steps
        {
            Image decoded;
            Image encoded; // what goes to the encoder (flipped)
        };

        Cooked cookOne(const std::string& key, const std::string& name, const std::vector<uint8_t>& raw,
            bool referenced, Steps* steps = nullptr)
        {
            Image image = decodeImage(raw, name);
            if (steps)
                steps->decoded = image;
            if (startsWith(key, kGlyphPrefix) && std::max(image.width, image.height) > kGlyphSize)
                image = resize(image, kGlyphSize, kGlyphSize, Filter::Lanczos);
            const int srcW = image.width, srcH = image.height;
            const bool ui = isUi(key) || referenced;

            if (endsWith(name, ".tga") && tgaIgnoresAlpha(raw))
                for (size_t i = 3; i < image.pixels.size(); i += 4)
                    image.pixels[i] = 255;
            Image rgba = std::move(image);
            rgba.mode = SourceMode::Rgba;

            uint8_t lo = 255;
            bool partial = false;
            for (size_t i = 3; i < rgba.pixels.size(); i += 4)
            {
                lo = std::min(lo, rgba.pixels[i]);
                partial |= rgba.pixels[i] != 0 && rgba.pixels[i] != 255;
            }
            const bool hasAlpha = lo < 255;
            uint8_t flags = 0;
            if (hasAlpha)
            {
                flags |= kFlagAlpha;
                if (!partial)
                    flags |= kFlagAlphaBinary;
            }

            int w = 0, h = 0;
            PicaFormat format;
            bool resampled = false;
            if (ui)
            {
                const auto big = kUiBig.find(key);
                const int uiMax = big == kUiBig.end() ? kUiMaxSize : big->second;
                resampled = std::max(srcW, srcH) > uiMax;
                w = pow2Ceil(std::min(srcW, uiMax), uiMax);
                h = pow2Ceil(std::min(srcH, uiMax), uiMax);
                format = std::max(w, h) <= 128 ? PicaFormat::Rgba8 : hasAlpha ? PicaFormat::Etc1A4 : PicaFormat::Etc1;
                if (hasAlpha)
                    rgba = bleedTransparent(rgba, format == PicaFormat::Rgba8);
                if (resampled)
                    rgba = resize(rgba, w, h, Filter::Lanczos);
                else if (w != srcW || h != srcH)
                {
                    rgba = paddedEdge(rgba, w, h);
                    flags |= kFlagPadded;
                }
            }
            else
            {
                w = pow2Size(srcW, kMaxSize);
                h = pow2Size(srcH, kMaxSize);
                if (w != srcW || h != srcH)
                    rgba = resize(rgba, w, h, (w < srcW || h < srcH) ? Filter::Lanczos : Filter::Bicubic);
                const bool smallRaw = endsWith(name, ".dds") && ddsIsUncompressed(raw) && std::max(w, h) <= 128;
                format = smallRaw ? PicaFormat::Rgba8 : hasAlpha ? PicaFormat::Etc1A4 : PicaFormat::Etc1;
                if (hasAlpha)
                    rgba = bleedTransparent(rgba, format == PicaFormat::Rgba8);
            }

            Cooked cooked;
            if (ui && referenced && isAtlasCandidate(key, srcW, srcH))
            {
                Image texels = resampled ? rgba : cropped(rgba, srcW, srcH);
                if (startsWith(key, kAtlasHalvedPrefix) && std::max(srcW, srcH) > 32)
                {
                    texels = resize(texels, srcW / 2, srcH / 2, Filter::Lanczos);
                    flags |= kFlagAtlasHalved;
                }
                if (texels.height > texels.width && texels.height > 64)
                {
                    texels = transposed(texels);
                    flags |= kFlagAtlasTransposed;
                }
                flags |= kFlagAtlas;
                cooked.atlasTexels = std::move(texels);
            }

            const Image flipped = flipVertical(rgba);
            if (steps)
                steps->encoded = flipped;
            int levels = 0;
            cooked.data = encodePica(flipped, format, !ui, levels);
            cooked.key = key;
            cooked.name = name;
            cooked.width = w;
            cooked.height = h;
            cooked.srcWidth = srcW;
            cooked.srcHeight = srcH;
            cooked.format = format;
            cooked.levels = levels;
            cooked.flags = flags;
            return cooked;
        }

        void atlasGutters(int width, int height, int& gx, int& gy)
        {
            gx = width >= kAtlasWidth ? 0 : kAtlasGutter;
            gy = height >= kAtlasHeight ? 0 : kAtlasGutter;
        }

        struct Placement
        {
            int x = 0, y = 0;
        };

        // Guillotine packing of the one atlas page (pack_atlas): the long strips first, then the tallest, the
        // widest; each cell takes the free rectangle with the smallest shorter leftover, turned on its side only
        // when that is the only way it fits.
        void packAtlas(const std::vector<std::pair<std::string, const Image*>>& cells,
            std::map<std::string, Placement>& placements, std::set<std::string>& turned)
        {
            struct Rect
            {
                int x, y, w, h;
            };
            struct Sized
            {
                std::string key;
                int w, h, gx, gy;
            };
            std::vector<Rect> free = { { 0, 0, kAtlasWidth, kAtlasHeight } };
            std::vector<Sized> sized;
            for (const auto& [key, texels] : cells)
            {
                int gx, gy;
                atlasGutters(texels->width, texels->height, gx, gy);
                sized.push_back({ key, texels->width + 2 * gx, texels->height + 2 * gy, gx, gy });
            }
            std::sort(sized.begin(), sized.end(), [](const Sized& a, const Sized& b) {
                const int ga = a.w > kAtlasWidth / 4 ? 0 : 1, gb = b.w > kAtlasWidth / 4 ? 0 : 1;
                if (ga != gb)
                    return ga < gb;
                if (a.h != b.h)
                    return a.h > b.h;
                if (a.w != b.w)
                    return a.w > b.w;
                return a.key < b.key;
            });
            for (Sized cell : sized)
            {
                bool found = false;
                std::pair<int, int> bestScore;
                size_t bestIndex = 0;
                bool bestTurned = false;
                for (const bool turn : { false, true })
                {
                    if (turn && cell.w == cell.h)
                        break;
                    const int cw = turn ? cell.h : cell.w, ch = turn ? cell.w : cell.h;
                    for (size_t i = 0; i < free.size(); ++i)
                    {
                        const Rect& f = free[i];
                        if (cw <= f.w && ch <= f.h)
                        {
                            const std::pair<int, int> score
                                = { std::min(f.w - cw, f.h - ch), std::max(f.w - cw, f.h - ch) };
                            if (!found || score < bestScore)
                            {
                                found = true;
                                bestScore = score;
                                bestIndex = i;
                                bestTurned = turn;
                            }
                        }
                    }
                    if (found)
                        break;
                }
                if (!found)
                    throw std::runtime_error("the UI atlas is full at " + cell.key);
                if (bestTurned)
                {
                    turned.insert(cell.key);
                    std::swap(cell.w, cell.h);
                    std::swap(cell.gx, cell.gy);
                }
                const Rect f = free[bestIndex];
                free.erase(free.begin() + std::ptrdiff_t(bestIndex));
                placements[cell.key] = { f.x + cell.gx, f.y + cell.gy };
                if (f.w - cell.w > f.h - cell.h)
                {
                    free.push_back({ f.x + cell.w, f.y, f.w - cell.w, f.h });
                    free.push_back({ f.x, f.y + cell.h, cell.w, f.h - cell.h });
                }
                else
                {
                    free.push_back({ f.x, f.y + cell.h, f.w, f.h - cell.h });
                    free.push_back({ f.x + cell.w, f.y, f.w - cell.w, cell.h });
                }
                free.erase(std::remove_if(free.begin(), free.end(), [](const Rect& r) { return r.w <= 0 || r.h <= 0; }),
                    free.end());
            }
        }

        struct Entry
        {
            uint64_t hash;
            std::string key;
            std::vector<uint8_t> data;
            int width, height, srcWidth, srcHeight;
            PicaFormat format;
            int levels;
            uint8_t flags;
            uint8_t atlas;
            uint32_t atlasPos;
        };

        void put(std::vector<uint8_t>& out, uint64_t value, int bytes)
        {
            for (int i = 0; i < bytes; ++i)
                out.push_back(uint8_t(value >> (8 * i)));
        }
    }

    CookSummary cookTextures(const CookOptions& options, const std::function<void(size_t, size_t)>& progress)
    {
        initPicaEncoder();
        const auto sources = collectSources(options);
        std::set<std::string> referenced
            = referencedTextures({ options.engineVfs / "mygui", options.portInterface / "mygui" });
        for (const auto& [key, source] : sources)
            if (startsWith(key, kAtlasHalvedPrefix))
                referenced.insert(key);

        std::vector<std::string> keys;
        for (const auto& [key, source] : sources)
            keys.push_back(key);
        if (options.limit != 0 && keys.size() > options.limit)
            keys.resize(options.limit);

        std::vector<std::optional<Cooked>> results(keys.size());
        std::atomic<size_t> next{ 0 }, done{ 0 };
        std::mutex errorMutex;
        std::string error;
        const unsigned threadCount
            = options.threads > 0 ? unsigned(options.threads) : std::max(1u, std::thread::hardware_concurrency());
        std::mutex progressMutex;
        const auto worker = [&] {
            while (true)
            {
                const size_t i = next++;
                if (i >= keys.size() || (options.cancel != nullptr && options.cancel->load()))
                    return;
                {
                    std::lock_guard<std::mutex> lock(errorMutex);
                    if (!error.empty())
                        return;
                }
                const auto& [name, source] = sources.at(keys[i]);
                try
                {
                    results[i] = cookOne(keys[i], name, readSource(source), referenced.count(keys[i]) != 0);
                }
                catch (const std::exception& e)
                {
                    std::lock_guard<std::mutex> lock(errorMutex);
                    if (error.empty())
                        error = name + ": " + e.what();
                    return;
                }
                const size_t finished = ++done;
                if (progress)
                {
                    std::lock_guard<std::mutex> lock(progressMutex);
                    progress(finished, keys.size());
                }
            }
        };
        std::vector<std::thread> threads;
        for (unsigned t = 0; t < threadCount; ++t)
            threads.emplace_back(worker);
        for (std::thread& t : threads)
            t.join();
        if (options.cancel != nullptr && options.cancel->load())
            return {};
        if (!error.empty())
            throw std::runtime_error("texture " + error);

        // The UI atlas.
        std::vector<std::pair<std::string, const Image*>> cells;
        for (const auto& r : results)
            if (r->atlasTexels)
                cells.push_back({ r->key, &*r->atlasTexels });
        std::map<std::string, Placement> placements;
        std::set<std::string> turned;
        packAtlas(cells, placements, turned);
        Image canvas(kAtlasWidth, kAtlasHeight);
        for (const auto& [key, texelsPtr] : cells)
        {
            const Image texels = turned.count(key) ? transposed(*texelsPtr) : *texelsPtr;
            int gx, gy;
            atlasGutters(texels.width, texels.height, gx, gy);
            const Image padded = paddedEdgeAround(texels, gx, gy, gx, gy);
            const Placement p = placements[key];
            for (int y = 0; y < padded.height; ++y)
                std::memcpy(canvas.at(p.x - gx, p.y - gy + y), padded.at(0, y), size_t(padded.width) * 4);
        }

        std::vector<Entry> entries;
        for (auto& r : results)
        {
            const auto placement = placements.find(r->key);
            const Placement p = placement == placements.end() ? Placement() : placement->second;
            uint8_t flags = r->flags;
            if (turned.count(r->key))
                flags |= kFlagAtlasTransposed;
            entries.push_back({ fnv1a64(r->key), r->key, std::move(r->data), r->width, r->height, r->srcWidth,
                r->srcHeight, r->format, r->levels, flags, 0, uint32_t(p.x) | (uint32_t(p.y) << 16) });
        }
        {
            int levels = 0;
            const std::string key = kAtlasKey + "0";
            entries.push_back({ fnv1a64(key), key, encodePica(flipVertical(canvas), PicaFormat::Rgba8, false, levels),
                kAtlasWidth, kAtlasHeight, kAtlasWidth, kAtlasHeight, PicaFormat::Rgba8, 1, kFlagAlpha, 0, 0 });
        }
        std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) { return a.hash < b.hash; });
        for (size_t i = 1; i < entries.size(); ++i)
            if (entries[i].hash == entries[i - 1].hash)
                throw std::runtime_error("texture name hash collision: " + entries[i - 1].key + " / " + entries[i].key);

        if (!options.keyList.empty())
        {
            std::ofstream list(options.keyList, std::ios::binary | std::ios::trunc);
            char hash[17];
            for (const Entry& e : entries)
            {
                std::snprintf(hash, sizeof(hash), "%016llx", static_cast<unsigned long long>(e.hash));
                list << hash << ' ' << e.key << '\n';
            }
        }

        // The pack: header, index, then each texture 128-byte aligned.
        const size_t indexOffset = kHeaderSize;
        const size_t dataOffset = (indexOffset + kEntrySize * entries.size() + kDataAlign - 1) / kDataAlign * kDataAlign;
        std::vector<uint8_t> head;
        head.insert(head.end(), { 'O', '3', 'T', 'P' });
        put(head, kVersion, 4);
        put(head, entries.size(), 4);
        put(head, indexOffset, 4);
        put(head, dataOffset, 4);
        put(head, 0, 12);
        size_t offset = dataOffset;
        for (const Entry& e : entries)
        {
            put(head, e.hash, 8);
            put(head, offset, 4);
            put(head, e.data.size(), 4);
            put(head, uint64_t(e.width), 2);
            put(head, uint64_t(e.height), 2);
            put(head, uint64_t(e.srcWidth), 2);
            put(head, uint64_t(e.srcHeight), 2);
            put(head, uint64_t(e.format), 1);
            put(head, uint64_t(e.levels), 1);
            put(head, e.flags, 1);
            put(head, e.atlas, 1);
            put(head, e.atlasPos, 4);
            offset = (offset + e.data.size() + kDataAlign - 1) / kDataAlign * kDataAlign;
        }
        head.resize(dataOffset, 0);

        fs::create_directories(options.out.parent_path());
        const fs::path part = options.out.string() + ".part";
        uint64_t size = 0;
        {
            std::ofstream file(part, std::ios::binary | std::ios::trunc);
            if (!file)
                throw std::runtime_error("can't write " + part.string());
            file.write(reinterpret_cast<const char*>(head.data()), std::streamsize(head.size()));
            size = head.size();
            static const char zeros[kDataAlign] = {};
            for (const Entry& e : entries)
            {
                const uint64_t aligned = (size + kDataAlign - 1) / kDataAlign * kDataAlign;
                file.write(zeros, std::streamsize(aligned - size));
                file.write(reinterpret_cast<const char*>(e.data.data()), std::streamsize(e.data.size()));
                size = aligned + e.data.size();
            }
            if (!file.flush())
                throw std::runtime_error("writing " + part.string() + " failed");
        }
        fs::rename(part, options.out);
        return { entries.size(), cells.size(), size };
    }

    void dumpTextureSteps(const CookOptions& options, const std::string& key, const fs::path& folder)
    {
        initPicaEncoder();
        const auto sources = collectSources(options);
        const auto found = sources.find(key);
        if (found == sources.end())
            throw std::runtime_error("no texture " + key);
        std::set<std::string> referenced
            = referencedTextures({ options.engineVfs / "mygui", options.portInterface / "mygui" });
        const auto& [name, source] = found->second;
        Steps steps;
        cookOne(key, name, readSource(source), referenced.count(key) != 0 || startsWith(key, kAtlasHalvedPrefix), &steps);
        fs::create_directories(folder);
        const auto dump = [&](const char* step, const Image& image) {
            std::ofstream out(folder / (std::string(step) + "_" + std::to_string(image.width) + "x"
                                  + std::to_string(image.height) + ".rgba"),
                std::ios::binary);
            out.write(reinterpret_cast<const char*>(image.pixels.data()), std::streamsize(image.pixels.size()));
        };
        dump("decoded", steps.decoded);
        dump("encoded", steps.encoded);
    }
}
