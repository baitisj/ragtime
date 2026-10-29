// j2k_at IN.ppm BYTES OUT.j2k OUT.ppm: Ragtime's own JPEG 2000 encoder at a budget.
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

#include "ragtime/image/Codec.h"

using namespace ragtime;

static bool readPpm(const char* path, picture::Image& im)
{
    std::ifstream f(path, std::ios::binary);
    std::string magic;
    int w, h, maxv;
    f >> magic >> w >> h >> maxv;
    f.get();
    if (magic != "P6" || maxv != 255) return false;
    im = picture::Image::blank(w, h, 3);
    f.read((char*)im.pixels.data(), (std::streamsize)im.pixels.size());
    return (bool)f;
}

static void writePpm(const char* path, const picture::Image& im)
{
    std::ofstream f(path, std::ios::binary);
    if (im.channels == 3)
    {
        f << "P6\n" << im.width << " " << im.height << "\n255\n";
        f.write((const char*)im.pixels.data(), (std::streamsize)im.pixels.size());
    }
    else
    {
        f << "P5\n" << im.width << " " << im.height << "\n255\n";
        f.write((const char*)im.pixels.data(), (std::streamsize)im.pixels.size());
    }
}

int main(int argc, char** argv)
{
    if (argc != 5) return 2;
    picture::Image in, out;
    if (!readPpm(argv[1], in)) { std::cerr << "bad ppm\n"; return 1; }
    image::Coded coded;
    std::string error;
    if (!image::encode(in, std::strtoull(argv[2], nullptr, 10), coded, error)) { std::cerr << error << "\n"; return 1; }
    std::ofstream(argv[3], std::ios::binary).write((const char*)coded.bytes.data(), (std::streamsize)coded.bytes.size());
    if (!image::decode(coded.bytes.data(), coded.bytes.size(), out, error)) { std::cerr << error << "\n"; return 1; }
    writePpm(argv[4], out);
    std::cout << coded.bytes.size() << " bytes, " << coded.layers << " layers, " << coded.resolutions << " resolutions\n";
    return 0;
}
