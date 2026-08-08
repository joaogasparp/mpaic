#ifndef IMAGE_IO_HPP
#define IMAGE_IO_HPP

#include <vector>
#include <string>
#include <cstdint>
#include <utility>

enum class PixelFormat { UINT8, UINT16_LE, UINT16_BE };

struct ImageInfo {
    uint32_t width;
    uint32_t height;
    uint32_t channels;
    PixelFormat format;
    uint16_t max_value;
};

// RAII buffer of native-endian uint16 samples backed by a memory-mapped temp file
// (so huge images stay bounded in RAM: the OS keeps only the working set resident and
// reclaims clean file-backed pages instead of thrashing swap). Falls back to a heap
// vector when mmap is unavailable. Exposes the same data()/size()/operator[] surface as
// std::vector<uint16_t>, so the compression hot paths remain byte-for-byte unchanged.
class MappedU16 {
public:
    MappedU16() = default;
    ~MappedU16();
    MappedU16(const MappedU16&) = delete;
    MappedU16& operator=(const MappedU16&) = delete;

    uint16_t* data() { return ptr_; }
    const uint16_t* data() const { return ptr_; }
    size_t size() const { return count_; }
    uint16_t& operator[](size_t i) { return ptr_[i]; }
    const uint16_t& operator[](size_t i) const { return ptr_[i]; }

    // internal setup used by the loader helpers below
    void adopt_heap(std::vector<uint16_t>&& v);
    void adopt_mmap(uint16_t* ptr, size_t count, void* map, size_t map_bytes, int fd);

private:
    void release();
    uint16_t* ptr_ = nullptr;
    size_t count_ = 0;
    void* map_ = nullptr;
    size_t map_bytes_ = 0;
    int fd_ = -1;
    std::vector<uint16_t> heap_;
};

// Load a raw file into a native-endian mmap-backed buffer (converting endianness via a
// temp file that is unlinked immediately). Falls back to a heap buffer on failure.
bool map_raw_native(const std::string& filename, uint32_t width, uint32_t height,
                    uint32_t channels, PixelFormat format,
                    const std::string& hint_dir, MappedU16& out);

// Create a writable native-endian mmap-backed buffer of `count` samples (temp file,
// unlinked immediately). Falls back to a heap buffer on failure.
bool map_create_writable(size_t count, const std::string& hint_dir, MappedU16& out);

// Stream native-endian uint16 pixels to a raw file in the given format, in chunks, so
// output stays bounded in RAM regardless of image size.
bool write_raw_stream(const std::string& filename, const uint16_t* pixels, size_t count,
                      const ImageInfo& info);

bool read_raw(const std::string& filename, std::vector<uint16_t>& pixels,
              uint32_t width, uint32_t height, uint32_t channels, PixelFormat format);
bool write_raw(const std::string& filename, const std::vector<uint16_t>& pixels, const ImageInfo& info);
bool read_image(const std::string& filename, std::vector<uint16_t>& pixels, ImageInfo& info,
                uint32_t width, uint32_t height, int bits = 8, bool big_endian = false, uint32_t channels = 1);



bool parse_raw_geometry(const std::string& filename, uint32_t& width, uint32_t& height,
                       uint32_t& channels, int& bits, bool& big_endian);

#endif
