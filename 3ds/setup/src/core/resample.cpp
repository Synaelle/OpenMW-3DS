#include "core/resample.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

// Pillow's libImaging/Resample.c and the RGBA <-> RGBa conversions of Convert.c, kept to the same arithmetic so the
// results are the same bytes.

namespace setup
{
    namespace
    {
        constexpr int kPrecisionBits = 32 - 8 - 2;
        constexpr double kPi = 3.14159265358979323846;

        double bicubic(double x)
        {
            constexpr double a = -0.5;
            if (x < 0.0)
                x = -x;
            if (x < 1.0)
                return ((a + 2.0) * x - (a + 3.0)) * x * x + 1;
            if (x < 2.0)
                return (((x - 5) * x + 8) * x - 4) * a;
            return 0.0;
        }

        double sinc(double x)
        {
            if (x == 0.0)
                return 1.0;
            x = x * kPi;
            return std::sin(x) / x;
        }

        double lanczos(double x)
        {
            if (-3.0 <= x && x < 3.0)
                return sinc(x) * sinc(x / 3);
            return 0.0;
        }

        struct Coefficients
        {
            int ksize = 0;
            std::vector<int> bounds; // xmin, count per output texel
            std::vector<int32_t> k; // ksize per output texel
        };

        Coefficients precompute(int inSize, int outSize, Filter filter)
        {
            double (*fn)(double) = filter == Filter::Lanczos ? lanczos : bicubic;
            const double filterSupport = filter == Filter::Lanczos ? 3.0 : 2.0;
            const float in0 = 0.f, in1 = float(inSize);
            double scale = double(in1 - in0) / outSize;
            double filterscale = scale < 1.0 ? 1.0 : scale;
            const double support = filterSupport * filterscale;
            Coefficients c;
            c.ksize = int(std::ceil(support)) * 2 + 1;
            c.bounds.resize(size_t(outSize) * 2);
            std::vector<double> kk(size_t(outSize) * size_t(c.ksize), 0.0);
            const double invFilterscale = 1.0 / filterscale;
            for (int xx = 0; xx < outSize; ++xx)
            {
                const double center = in0 + (xx + 0.5) * scale;
                double ww = 0.0;
                int xmin = int(center - support + 0.5);
                if (xmin < 0)
                    xmin = 0;
                int xmax = int(center + support + 0.5);
                if (xmax > inSize)
                    xmax = inSize;
                xmax -= xmin;
                double* k = &kk[size_t(xx) * size_t(c.ksize)];
                int x = 0;
                for (; x < xmax; ++x)
                {
                    const double w = fn((x + xmin - center + 0.5) * invFilterscale);
                    k[x] = w;
                    ww += w;
                }
                if (ww != 0.0)
                    for (x = 0; x < xmax; ++x)
                        k[x] /= ww;
                for (; x < c.ksize; ++x)
                    k[x] = 0;
                c.bounds[size_t(xx) * 2] = xmin;
                c.bounds[size_t(xx) * 2 + 1] = xmax;
            }
            c.k.resize(kk.size());
            for (size_t i = 0; i < kk.size(); ++i)
                c.k[i] = kk[i] < 0 ? int32_t(-0.5 + kk[i] * (1 << kPrecisionBits))
                                   : int32_t(0.5 + kk[i] * (1 << kPrecisionBits));
            return c;
        }

        uint8_t clip8(int in)
        {
            const int v = in >> kPrecisionBits;
            return uint8_t(v < 0 ? 0 : v > 255 ? 255 : v);
        }

        Image horizontal(const Image& in, int outWidth, Filter filter)
        {
            const Coefficients c = precompute(in.width, outWidth, filter);
            Image out(outWidth, in.height);
            for (int y = 0; y < in.height; ++y)
            {
                const uint8_t* line = in.at(0, y);
                for (int xx = 0; xx < outWidth; ++xx)
                {
                    const int xmin = c.bounds[size_t(xx) * 2], xmax = c.bounds[size_t(xx) * 2 + 1];
                    const int32_t* k = &c.k[size_t(xx) * size_t(c.ksize)];
                    int ss[4] = { 1 << (kPrecisionBits - 1), 1 << (kPrecisionBits - 1), 1 << (kPrecisionBits - 1),
                        1 << (kPrecisionBits - 1) };
                    for (int x = 0; x < xmax; ++x)
                        for (int ch = 0; ch < 4; ++ch)
                            ss[ch] += line[(x + xmin) * 4 + ch] * k[x];
                    uint8_t* o = out.at(xx, y);
                    for (int ch = 0; ch < 4; ++ch)
                        o[ch] = clip8(ss[ch]);
                }
            }
            return out;
        }

        Image vertical(const Image& in, int outHeight, Filter filter)
        {
            const Coefficients c = precompute(in.height, outHeight, filter);
            Image out(in.width, outHeight);
            for (int yy = 0; yy < outHeight; ++yy)
            {
                const int ymin = c.bounds[size_t(yy) * 2], ymax = c.bounds[size_t(yy) * 2 + 1];
                const int32_t* k = &c.k[size_t(yy) * size_t(c.ksize)];
                for (int xx = 0; xx < in.width; ++xx)
                {
                    int ss[4] = { 1 << (kPrecisionBits - 1), 1 << (kPrecisionBits - 1), 1 << (kPrecisionBits - 1),
                        1 << (kPrecisionBits - 1) };
                    for (int y = 0; y < ymax; ++y)
                    {
                        const uint8_t* p = in.at(xx, y + ymin);
                        for (int ch = 0; ch < 4; ++ch)
                            ss[ch] += p[ch] * k[y];
                    }
                    uint8_t* o = out.at(xx, yy);
                    for (int ch = 0; ch < 4; ++ch)
                        o[ch] = clip8(ss[ch]);
                }
            }
            return out;
        }

        // Convert.c's MULDIV255.
        uint8_t mulDiv255(unsigned a, unsigned b)
        {
            const unsigned tmp = a * b + 128;
            return uint8_t(((tmp >> 8) + tmp) >> 8);
        }

        Image nearest(const Image& in, int width, int height)
        {
            // Pillow's affine nearest for a plain resize: the source texel at (x + 0.5) * scale, truncated.
            Image out(width, height);
            const double sx = double(in.width) / width, sy = double(in.height) / height;
            for (int y = 0; y < height; ++y)
            {
                const int yin = std::min(in.height - 1, int((y + 0.5) * sy));
                for (int x = 0; x < width; ++x)
                {
                    const int xin = std::min(in.width - 1, int((x + 0.5) * sx));
                    std::copy_n(in.at(xin, yin), 4, out.at(x, y));
                }
            }
            return out;
        }
    }

    Image resize(const Image& image, int width, int height, Filter filter)
    {
        if (width == image.width && height == image.height)
            return image;
        if (image.mode == SourceMode::Palette)
        {
            Image out = nearest(image, width, height);
            out.mode = image.mode;
            return out;
        }
        // RGBA -> RGBa
        Image work(image.width, image.height);
        for (size_t i = 0; i < image.pixels.size(); i += 4)
        {
            const unsigned alpha = image.pixels[i + 3];
            work.pixels[i] = mulDiv255(image.pixels[i], alpha);
            work.pixels[i + 1] = mulDiv255(image.pixels[i + 1], alpha);
            work.pixels[i + 2] = mulDiv255(image.pixels[i + 2], alpha);
            work.pixels[i + 3] = uint8_t(alpha);
        }
        if (image.height > image.width * 100 && height < image.height)
        {
            // Image.resize's very tall images: the height first, then the width (two resizes of the RGBa image).
            if (height != image.height)
                work = vertical(work, height, filter);
            if (width != image.width)
                work = horizontal(work, width, filter);
        }
        else
        {
            if (width != image.width)
                work = horizontal(work, width, filter);
            if (height != image.height)
                work = vertical(work, height, filter);
        }
        // RGBa -> RGBA
        for (size_t i = 0; i < work.pixels.size(); i += 4)
        {
            const unsigned alpha = work.pixels[i + 3];
            if (alpha != 255 && alpha != 0)
                for (int c = 0; c < 3; ++c)
                {
                    const unsigned v = (255 * unsigned(work.pixels[i + c])) / alpha;
                    work.pixels[i + c] = uint8_t(v > 255 ? 255 : v);
                }
        }
        work.mode = image.mode;
        return work;
    }
}
