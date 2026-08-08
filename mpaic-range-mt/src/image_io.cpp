#include "image_io.hpp"
#include <fstream>
#include <iostream>
#include <regex>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

namespace {

constexpr size_t IO_CHUNK_BYTES = 4u << 20; // 4 MiB streaming chunk

// Create a temp file in `hint_dir` (fallbacks: /tmp, "."), sized to `bytes`, and return
// an open fd. The file is unlinked immediately so it auto-cleans on close/crash while the
// mapping/fd keeps the inode alive.
int make_temp_file(const std::string& hint_dir, size_t bytes) {
    std::vector<std::string> dirs;
    if (!hint_dir.empty()) dirs.push_back(hint_dir);
    dirs.push_back("/tmp");
    dirs.push_back(".");
    for (const auto& d : dirs) {
        std::string tmpl = d + "/.cacx_tmp_XXXXXX";
        std::vector<char> buf(tmpl.begin(), tmpl.end());
        buf.push_back('\0');
        int fd = mkstemp(buf.data());
        if (fd < 0) continue;
        unlink(buf.data());
        if (bytes > 0 && ftruncate(fd, static_cast<off_t>(bytes)) != 0) {
            close(fd);
            continue;
        }
        return fd;
    }
    return -1;
}

} // namespace

MappedU16::~MappedU16() { release(); }

void MappedU16::release() {
    if (map_ && map_ != MAP_FAILED) {
        munmap(map_, map_bytes_);
    }
    map_ = nullptr;
    map_bytes_ = 0;
    if (fd_ >= 0) {
        close(fd_);
        fd_ = -1;
    }
    ptr_ = nullptr;
    count_ = 0;
}

void MappedU16::adopt_heap(std::vector<uint16_t>&& v) {
    release();
    heap_ = std::move(v);
    ptr_ = heap_.data();
    count_ = heap_.size();
}

void MappedU16::adopt_mmap(uint16_t* ptr, size_t count, void* map, size_t map_bytes, int fd) {
    release();
    ptr_ = ptr;
    count_ = count;
    map_ = map;
    map_bytes_ = map_bytes;
    fd_ = fd;
}

bool map_raw_native(const std::string& filename, uint32_t width, uint32_t height,
                    uint32_t channels, PixelFormat format,
                    const std::string& hint_dir, MappedU16& out) {
    size_t count = static_cast<size_t>(width) * height * channels;
    if (count == 0) {
        out.adopt_heap({});
        return true;
    }
    size_t bytes = count * sizeof(uint16_t);

    std::ifstream in(filename, std::ios::binary);
    if (!in) {
        std::cerr << "Erro: nao foi possivel abrir " << filename << "\n";
        return false;
    }

    int fd = make_temp_file(hint_dir, bytes);
    bool use_mmap = (fd >= 0);
    void* map = MAP_FAILED;
    uint16_t* dst = nullptr;
    std::vector<uint16_t> heap;
    if (use_mmap) {
        map = mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (map == MAP_FAILED) {
            close(fd);
            fd = -1;
            use_mmap = false;
        } else {
            dst = static_cast<uint16_t*>(map);
        }
    }
    if (!use_mmap) {
        try {
            heap.resize(count);
        } catch (...) {
            std::cerr << "Erro: memoria insuficiente para carregar imagem\n";
            return false;
        }
        dst = heap.data();
    }

    // Stream-convert the raw file into native-endian uint16 samples.
    if (format == PixelFormat::UINT8) {
        std::vector<uint8_t> chunk(IO_CHUNK_BYTES);
        size_t written = 0;
        while (written < count) {
            size_t want = std::min(IO_CHUNK_BYTES, count - written);
            in.read(reinterpret_cast<char*>(chunk.data()), static_cast<std::streamsize>(want));
            std::streamsize got = in.gcount();
            for (std::streamsize i = 0; i < got; ++i) dst[written + static_cast<size_t>(i)] = chunk[static_cast<size_t>(i)];
            for (std::streamsize i = got; i < static_cast<std::streamsize>(want); ++i) dst[written + static_cast<size_t>(i)] = 0;
            written += want;
            if (!in && written < count) { for (size_t i = written; i < count; ++i) dst[i] = 0; break; }
        }
    } else {
        const bool be = (format == PixelFormat::UINT16_BE);
        size_t chunk_samples = IO_CHUNK_BYTES / 2;
        std::vector<uint8_t> chunk(chunk_samples * 2);
        size_t written = 0;
        while (written < count) {
            size_t want = std::min(chunk_samples, count - written);
            in.read(reinterpret_cast<char*>(chunk.data()), static_cast<std::streamsize>(want * 2));
            std::streamsize got_bytes = in.gcount();
            size_t got = static_cast<size_t>(got_bytes) / 2;
            for (size_t i = 0; i < got; ++i) {
                uint8_t b0 = chunk[i * 2], b1 = chunk[i * 2 + 1];
                dst[written + i] = be ? (static_cast<uint16_t>(b0) << 8) | b1
                                      : (static_cast<uint16_t>(b1) << 8) | b0;
            }
            for (size_t i = got; i < want; ++i) dst[written + i] = 0;
            written += want;
            if (!in && written < count) { for (size_t i = written; i < count; ++i) dst[i] = 0; break; }
        }
    }

    if (use_mmap) {
        out.adopt_mmap(dst, count, map, bytes, fd);
    } else {
        out.adopt_heap(std::move(heap));
    }
    return true;
}

bool map_create_writable(size_t count, const std::string& hint_dir, MappedU16& out) {
    if (count == 0) {
        out.adopt_heap({});
        return true;
    }
    size_t bytes = count * sizeof(uint16_t);
    int fd = make_temp_file(hint_dir, bytes);
    if (fd >= 0) {
        void* map = mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (map != MAP_FAILED) {
            out.adopt_mmap(static_cast<uint16_t*>(map), count, map, bytes, fd);
            return true;
        }
        close(fd);
    }
    try {
        std::vector<uint16_t> heap(count);
        out.adopt_heap(std::move(heap));
        return true;
    } catch (...) {
        std::cerr << "Erro: memoria insuficiente para imagem descodificada\n";
        return false;
    }
}

bool write_raw_stream(const std::string& filename, const uint16_t* pixels, size_t count,
                      const ImageInfo& info) {
    std::ofstream file(filename, std::ios::binary);
    if (!file) return false;

    if (info.format == PixelFormat::UINT8) {
        size_t chunk_samples = IO_CHUNK_BYTES;
        std::vector<uint8_t> buffer(chunk_samples);
        size_t done = 0;
        while (done < count) {
            size_t n = std::min(chunk_samples, count - done);
            for (size_t i = 0; i < n; ++i) buffer[i] = static_cast<uint8_t>(pixels[done + i] & 0xFF);
            file.write(reinterpret_cast<char*>(buffer.data()), static_cast<std::streamsize>(n));
            done += n;
        }
    } else {
        const bool be = (info.format == PixelFormat::UINT16_BE);
        size_t chunk_samples = IO_CHUNK_BYTES / 2;
        std::vector<uint8_t> buffer(chunk_samples * 2);
        size_t done = 0;
        while (done < count) {
            size_t n = std::min(chunk_samples, count - done);
            for (size_t i = 0; i < n; ++i) {
                uint16_t v = pixels[done + i];
                if (be) { buffer[i * 2] = (v >> 8) & 0xFF; buffer[i * 2 + 1] = v & 0xFF; }
                else    { buffer[i * 2] = v & 0xFF; buffer[i * 2 + 1] = (v >> 8) & 0xFF; }
            }
            file.write(reinterpret_cast<char*>(buffer.data()), static_cast<std::streamsize>(n * 2));
            done += n;
        }
    }
    return static_cast<bool>(file);
}

bool read_raw(const std::string& filename, std::vector<uint16_t>& pixels,
              uint32_t width, uint32_t height, uint32_t channels, PixelFormat format) {
    std::ifstream file(filename, std::ios::binary);
    if (!file) {
        std::cerr << "Error: could not open " << filename << "\n";
        return false;
    }
    
    size_t total = (size_t)width * height * channels;
    pixels.resize(total);
    
    if (format == PixelFormat::UINT8) {
        std::vector<uint8_t> buffer(total);
        file.read(reinterpret_cast<char*>(buffer.data()), total);
        for (size_t i = 0; i < total; i++) pixels[i] = buffer[i];
    } else {
        std::vector<uint8_t> buffer(total * 2);
        file.read(reinterpret_cast<char*>(buffer.data()), total * 2);
        for (size_t i = 0; i < total; i++) {
            uint8_t b0 = buffer[i * 2], b1 = buffer[i * 2 + 1];
            pixels[i] = (format == PixelFormat::UINT16_BE) ? 
                        ((uint16_t)b0 << 8) | b1 : ((uint16_t)b1 << 8) | b0;
        }
    }
    return true;
}

bool write_raw(const std::string& filename, const std::vector<uint16_t>& pixels, const ImageInfo& info) {
    std::ofstream file(filename, std::ios::binary);
    if (!file) return false;
    
    if (info.format == PixelFormat::UINT8) {
        std::vector<uint8_t> buffer(pixels.size());
        for (size_t i = 0; i < pixels.size(); i++) buffer[i] = (uint8_t)(pixels[i] & 0xFF);
        file.write(reinterpret_cast<char*>(buffer.data()), buffer.size());
    } else {
        std::vector<uint8_t> buffer(pixels.size() * 2);
        for (size_t i = 0; i < pixels.size(); i++) {
            if (info.format == PixelFormat::UINT16_BE) {
                buffer[i * 2] = (pixels[i] >> 8) & 0xFF;
                buffer[i * 2 + 1] = pixels[i] & 0xFF;
            } else {
                buffer[i * 2] = pixels[i] & 0xFF;
                buffer[i * 2 + 1] = (pixels[i] >> 8) & 0xFF;
            }
        }
        file.write(reinterpret_cast<char*>(buffer.data()), buffer.size());
    }
    return true;
}

bool read_image(const std::string& filename, std::vector<uint16_t>& pixels, ImageInfo& info,
                uint32_t width, uint32_t height, int bits, bool big_endian, uint32_t channels) {
    PixelFormat format;
    if (bits == 8) {
        format = PixelFormat::UINT8;
        info.max_value = 255;
    } else if (bits == 16) {
        format = big_endian ? PixelFormat::UINT16_BE : PixelFormat::UINT16_LE;
        info.max_value = 65535;
    } else {
        std::cerr << "Erro: bits deve ser 8 ou 16\n";
        return false;
    }
    
    info.width = width;
    info.height = height;
    info.channels = channels;
    info.format = format;
    
    return read_raw(filename, pixels, width, height, channels, format);
}

bool parse_raw_geometry(const std::string& filename, uint32_t& width, uint32_t& height,
                       uint32_t& channels, int& bits, bool& big_endian) {
    std::string basename = filename;
    size_t slash = filename.rfind('/');
    if (slash != std::string::npos) basename = filename.substr(slash + 1);

    std::regex pattern(R"(u(8|16)(be|le)?-(\d+)x(\d+)x(\d+)\.raw$)", std::regex::icase);
    std::smatch match;
    if (!std::regex_search(basename, match, pattern)) return false;

    bits = std::stoi(match[1].str());
    std::string endian_str = match[2].str();
    if (endian_str.empty()) endian_str = "le";
    big_endian = (endian_str == "be" || endian_str == "BE");
    channels = std::stoi(match[3].str());
    height = std::stoi(match[4].str());
    width = std::stoi(match[5].str());
    return true;
}
