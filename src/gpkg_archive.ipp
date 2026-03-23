#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <fstream>
#include <functional>
#include <lzma.h>
#include <map>
#include <set>
#include <sstream>
#include <stdint.h>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <utime.h>
#include <vector>
#include <zlib.h>
#include <zstd.h>

namespace GpkgArchive {

constexpr size_t TAR_BLOCK_SIZE = 512;

enum class TarEntryType {
    Regular,
    Directory,
    Symlink,
    Hardlink,
    Unsupported,
};

struct TarEntry {
    std::string path;
    std::string link_target;
    TarEntryType type = TarEntryType::Unsupported;
    mode_t mode = 0644;
    uint64_t size = 0;
    uint64_t data_offset = 0;
};

struct TarExtractOptions {
    size_t strip_components = 0;
    bool materialize_hardlinks = true;
};

struct TarSource {
    std::string archive_path;
    std::string source_path;
    TarEntryType type = TarEntryType::Unsupported;
    mode_t mode = 0644;
    std::string link_target;
};

bool path_has_suffix(const std::string& path, const std::string& suffix) {
    return path.size() >= suffix.size() &&
           path.compare(path.size() - suffix.size(), suffix.size(), suffix) == 0;
}

bool is_all_zero_block(const char* buffer, size_t size) {
    for (size_t i = 0; i < size; ++i) {
        if (buffer[i] != '\0') return false;
    }
    return true;
}

uint64_t align_up_tar(uint64_t value) {
    return (value + static_cast<uint64_t>(TAR_BLOCK_SIZE - 1)) &
           ~static_cast<uint64_t>(TAR_BLOCK_SIZE - 1);
}

std::string trim_tar_string(const char* data, size_t size) {
    size_t end = 0;
    while (end < size && data[end] != '\0') ++end;
    std::string out(data, end);
    while (!out.empty() && out.back() == ' ') out.pop_back();
    return out;
}

bool parse_tar_number(const char* data, size_t size, uint64_t& out) {
    if (size == 0) {
        out = 0;
        return true;
    }

    const unsigned char* bytes = reinterpret_cast<const unsigned char*>(data);
    if (bytes[0] & 0x80) {
        uint64_t value = 0;
        for (size_t i = 0; i < size; ++i) {
            unsigned char byte = bytes[i];
            if (i == 0) byte &= 0x7f;
            value = (value << 8) | static_cast<uint64_t>(byte);
        }
        out = value;
        return true;
    }

    std::string text(data, size);
    size_t first = text.find_first_of("01234567");
    if (first == std::string::npos) {
        out = 0;
        return true;
    }

    size_t last = text.find_last_of("01234567");
    std::string digits = text.substr(first, last - first + 1);
    out = 0;
    for (char ch : digits) {
        out = (out << 3) | static_cast<uint64_t>(ch - '0');
    }
    return true;
}

void write_tar_octal(char* dest, size_t size, uint64_t value) {
    std::snprintf(dest, size, "%0*llo", static_cast<int>(size - 1), static_cast<unsigned long long>(value));
}

bool read_exact(std::ifstream& in, char* buffer, size_t size) {
    in.read(buffer, static_cast<std::streamsize>(size));
    return in.good() || static_cast<size_t>(in.gcount()) == size;
}

bool copy_regular_file(const std::string& source_path, const std::string& dest_path, mode_t mode, std::string* error_out = nullptr) {
    std::ifstream input(source_path, std::ios::binary);
    if (!input) {
        if (error_out) *error_out = "failed to open hardlink source " + source_path;
        return false;
    }

    int fd = open(dest_path.c_str(), O_CREAT | O_TRUNC | O_WRONLY, mode & 07777);
    if (fd < 0) {
        if (error_out) *error_out = "failed to open " + dest_path;
        return false;
    }

    char buffer[32768];
    bool ok = true;
    while (input.read(buffer, sizeof(buffer)) || input.gcount() > 0) {
        size_t bytes = static_cast<size_t>(input.gcount());
        size_t written_total = 0;
        while (written_total < bytes) {
            ssize_t written = write(fd, buffer + written_total, bytes - written_total);
            if (written <= 0) {
                ok = false;
                break;
            }
            written_total += static_cast<size_t>(written);
        }
        if (!ok) break;
    }

    close(fd);
    if (!ok || !input.eof()) {
        unlink(dest_path.c_str());
        if (error_out) *error_out = "failed to materialize hardlink " + dest_path;
        return false;
    }
    chmod(dest_path.c_str(), mode & 07777);
    return true;
}

bool ensure_directory(const std::string& path) {
    if (path.empty()) return false;
    if (path == "/") return true;

    struct stat st {};
    if (stat(path.c_str(), &st) == 0) return S_ISDIR(st.st_mode);

    size_t slash = path.find_last_of('/');
    if (slash != std::string::npos && slash > 0) {
        if (!ensure_directory(path.substr(0, slash))) return false;
    }

    if (mkdir(path.c_str(), 0755) == 0) return true;
    return errno == EEXIST;
}

bool ensure_parent_directory(const std::string& path) {
    size_t slash = path.find_last_of('/');
    if (slash == std::string::npos) return true;
    if (slash == 0) return ensure_directory("/");
    return ensure_directory(path.substr(0, slash));
}

bool remove_existing_path(const std::string& path) {
    struct stat st {};
    if (lstat(path.c_str(), &st) != 0) return errno == ENOENT;
    if (S_ISDIR(st.st_mode)) return rmdir(path.c_str()) == 0;
    return unlink(path.c_str()) == 0;
}

std::vector<std::string> split_path_components(const std::string& path) {
    std::vector<std::string> parts;
    std::stringstream ss(path);
    std::string part;
    while (std::getline(ss, part, '/')) {
        if (part.empty() || part == ".") continue;
        parts.push_back(part);
    }
    return parts;
}

bool sanitize_relative_path(const std::string& raw_path, std::string& out_path) {
    std::string normalized = raw_path;
    while (!normalized.empty() && normalized[0] == '/') normalized.erase(normalized.begin());
    while (normalized.rfind("./", 0) == 0) normalized.erase(0, 2);
    if (normalized == "." || normalized == "./") normalized.clear();

    std::vector<std::string> clean_parts;
    for (const auto& part : split_path_components(normalized)) {
        if (part == "..") return false;
        clean_parts.push_back(part);
    }

    out_path.clear();
    for (size_t i = 0; i < clean_parts.size(); ++i) {
        if (i > 0) out_path += "/";
        out_path += clean_parts[i];
    }
    return true;
}

bool strip_path_components(const std::string& raw_path, size_t strip_components, std::string& out_path) {
    std::string sanitized;
    if (!sanitize_relative_path(raw_path, sanitized)) return false;

    std::vector<std::string> parts = split_path_components(sanitized);
    if (parts.size() < strip_components) {
        out_path.clear();
        return true;
    }

    out_path.clear();
    for (size_t i = strip_components; i < parts.size(); ++i) {
        if (!out_path.empty()) out_path += "/";
        out_path += parts[i];
    }
    return true;
}

std::map<std::string, std::string> parse_pax_records(const std::string& payload) {
    std::map<std::string, std::string> records;
    size_t pos = 0;
    while (pos < payload.size()) {
        size_t space = payload.find(' ', pos);
        if (space == std::string::npos) break;
        size_t record_len = static_cast<size_t>(std::strtoull(payload.substr(pos, space - pos).c_str(), nullptr, 10));
        if (record_len == 0 || pos + record_len > payload.size()) break;
        std::string record = payload.substr(space + 1, record_len - (space - pos) - 1);
        if (!record.empty() && record.back() == '\n') record.pop_back();
        size_t equals = record.find('=');
        if (equals != std::string::npos) {
            records[record.substr(0, equals)] = record.substr(equals + 1);
        }
        pos += record_len;
    }
    return records;
}

std::string build_pax_payload(const std::map<std::string, std::string>& records) {
    std::string payload;
    for (const auto& entry : records) {
        std::string body = entry.first + "=" + entry.second + "\n";
        size_t digits = 1;
        while (true) {
            size_t total = digits + 1 + body.size();
            size_t new_digits = std::to_string(total).size();
            if (new_digits == digits) {
                payload += std::to_string(total) + " " + body;
                break;
            }
            digits = new_digits;
        }
    }
    return payload;
}

bool split_ustar_name_prefix(const std::string& path, std::string& name_out, std::string& prefix_out) {
    if (path.size() <= 100) {
        name_out = path;
        prefix_out.clear();
        return true;
    }

    size_t slash = path.rfind('/');
    while (slash != std::string::npos) {
        std::string prefix = path.substr(0, slash);
        std::string name = path.substr(slash + 1);
        if (name.size() <= 100 && prefix.size() <= 155) {
            name_out = name;
            prefix_out = prefix;
            return true;
        }
        if (slash == 0) break;
        slash = path.rfind('/', slash - 1);
    }

    return false;
}

bool read_file_to_bytes(const std::string& path, std::vector<char>& out, std::string* error_out = nullptr) {
    out.clear();
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        if (error_out) *error_out = "could not open " + path;
        return false;
    }
    input.seekg(0, std::ios::end);
    std::streamoff size = input.tellg();
    if (size < 0) {
        if (error_out) *error_out = "could not read size of " + path;
        return false;
    }
    input.seekg(0, std::ios::beg);
    out.resize(static_cast<size_t>(size));
    if (!out.empty()) {
        input.read(out.data(), size);
        if (!input) {
            if (error_out) *error_out = "failed to read " + path;
            return false;
        }
    }
    return true;
}

bool write_bytes_to_file(const std::string& path, const std::vector<char>& data, mode_t mode = 0644, std::string* error_out = nullptr) {
    if (!ensure_parent_directory(path)) {
        if (error_out) *error_out = "could not create parent directory for " + path;
        return false;
    }

    int fd = open(path.c_str(), O_CREAT | O_TRUNC | O_WRONLY, mode & 07777);
    if (fd < 0) {
        if (error_out) *error_out = "could not open " + path;
        return false;
    }

    size_t written_total = 0;
    while (written_total < data.size()) {
        ssize_t written = write(fd, data.data() + written_total, data.size() - written_total);
        if (written <= 0) {
            close(fd);
            unlink(path.c_str());
            if (error_out) *error_out = "failed while writing " + path;
            return false;
        }
        written_total += static_cast<size_t>(written);
    }
    close(fd);
    chmod(path.c_str(), mode & 07777);
    return true;
}

std::string lzma_error_string(lzma_ret ret) {
    switch (ret) {
        case LZMA_OK: return "ok";
        case LZMA_STREAM_END: return "stream end";
        case LZMA_MEM_ERROR: return "out of memory";
        case LZMA_FORMAT_ERROR: return "input is not a valid .xz/.lzma stream";
        case LZMA_OPTIONS_ERROR: return "unsupported compression options";
        case LZMA_DATA_ERROR: return "corrupt compressed data";
        case LZMA_BUF_ERROR: return "truncated compressed data";
        default: return "liblzma error";
    }
}

bool decompress_gzip_file(const std::string& input_path, const std::string& output_path, std::string* error_out = nullptr) {
    if (error_out) error_out->clear();
    gzFile input = gzopen(input_path.c_str(), "rb");
    if (!input) {
        if (error_out) *error_out = "could not open gzip archive";
        return false;
    }

    std::ofstream output(output_path, std::ios::binary);
    if (!output) {
        gzclose(input);
        if (error_out) *error_out = "could not open decompression target";
        return false;
    }

    char buffer[32768];
    int bytes_read = 0;
    while ((bytes_read = gzread(input, buffer, sizeof(buffer))) > 0) {
        output.write(buffer, bytes_read);
        if (!output) {
            gzclose(input);
            if (error_out) *error_out = "failed while writing decompressed archive";
            return false;
        }
    }

    if (bytes_read < 0) {
        int errnum = Z_OK;
        const char* message = gzerror(input, &errnum);
        gzclose(input);
        if (error_out) *error_out = (message && *message) ? message : "gzip decompression failed";
        return false;
    }

    gzclose(input);
    return true;
}

bool decompress_xz_file(const std::string& input_path, const std::string& output_path, std::string* error_out = nullptr) {
    if (error_out) error_out->clear();

    std::ifstream input(input_path, std::ios::binary);
    if (!input) {
        if (error_out) *error_out = "could not open compressed archive";
        return false;
    }

    std::ofstream output(output_path, std::ios::binary);
    if (!output) {
        if (error_out) *error_out = "could not open decompression target";
        return false;
    }

    lzma_stream stream = LZMA_STREAM_INIT;
    lzma_ret init_ret = lzma_auto_decoder(&stream, UINT64_MAX, LZMA_CONCATENATED);
    if (init_ret != LZMA_OK) {
        if (error_out) *error_out = lzma_error_string(init_ret);
        return false;
    }

    bool success = false;
    bool input_finished = false;
    uint8_t input_buffer[32768];
    uint8_t output_buffer[32768];

    while (true) {
        if (stream.avail_in == 0 && !input_finished) {
            input.read(reinterpret_cast<char*>(input_buffer), sizeof(input_buffer));
            stream.next_in = input_buffer;
            stream.avail_in = static_cast<size_t>(input.gcount());
            if (input.bad()) {
                if (error_out) *error_out = "failed while reading compressed archive";
                break;
            }
            input_finished = input.eof();
        }

        stream.next_out = output_buffer;
        stream.avail_out = sizeof(output_buffer);
        lzma_ret ret = lzma_code(&stream, input_finished ? LZMA_FINISH : LZMA_RUN);

        size_t produced = sizeof(output_buffer) - stream.avail_out;
        if (produced > 0) {
            output.write(reinterpret_cast<const char*>(output_buffer), produced);
            if (!output) {
                if (error_out) *error_out = "failed while writing decompressed archive";
                break;
            }
        }

        if (ret == LZMA_STREAM_END) {
            success = true;
            break;
        }
        if (ret != LZMA_OK) {
            if (error_out) *error_out = lzma_error_string(ret);
            break;
        }
        if (input_finished && stream.avail_in == 0 && produced == 0) {
            if (error_out) *error_out = "truncated compressed archive";
            break;
        }
    }

    lzma_end(&stream);
    return success;
}

bool decompress_zstd_file(const std::string& input_path, const std::string& output_path, std::string* error_out = nullptr) {
    if (error_out) error_out->clear();

    std::ifstream input(input_path, std::ios::binary);
    if (!input) {
        if (error_out) *error_out = "could not open zstd archive";
        return false;
    }

    std::ofstream output(output_path, std::ios::binary);
    if (!output) {
        if (error_out) *error_out = "could not open decompression target";
        return false;
    }

    ZSTD_DStream* stream = ZSTD_createDStream();
    if (!stream) {
        if (error_out) *error_out = "could not allocate zstd decompressor";
        return false;
    }

    size_t init_ret = ZSTD_initDStream(stream);
    if (ZSTD_isError(init_ret)) {
        if (error_out) *error_out = ZSTD_getErrorName(init_ret);
        ZSTD_freeDStream(stream);
        return false;
    }

    char input_buffer[32768];
    char output_buffer[32768];
    size_t last_ret = 1;

    while (input.read(input_buffer, sizeof(input_buffer)) || input.gcount() > 0) {
        ZSTD_inBuffer in_buffer = {input_buffer, static_cast<size_t>(input.gcount()), 0};
        while (in_buffer.pos < in_buffer.size) {
            ZSTD_outBuffer out_buffer = {output_buffer, sizeof(output_buffer), 0};
            last_ret = ZSTD_decompressStream(stream, &out_buffer, &in_buffer);
            if (ZSTD_isError(last_ret)) {
                if (error_out) *error_out = ZSTD_getErrorName(last_ret);
                ZSTD_freeDStream(stream);
                return false;
            }
            if (out_buffer.pos > 0) {
                output.write(output_buffer, out_buffer.pos);
                if (!output) {
                    if (error_out) *error_out = "failed while writing decompressed archive";
                    ZSTD_freeDStream(stream);
                    return false;
                }
            }
        }
    }

    ZSTD_freeDStream(stream);
    if (!input.eof()) {
        if (error_out) *error_out = "failed while reading compressed archive";
        return false;
    }
    if (last_ret != 0) {
        if (error_out) *error_out = "truncated compressed archive";
        return false;
    }
    return true;
}

bool compress_zstd_file(const std::string& input_path, const std::string& output_path, int level = 10, std::string* error_out = nullptr) {
    if (error_out) error_out->clear();

    std::ifstream input(input_path, std::ios::binary);
    if (!input) {
        if (error_out) *error_out = "could not open compression source";
        return false;
    }

    std::ofstream output(output_path, std::ios::binary);
    if (!output) {
        if (error_out) *error_out = "could not open compression target";
        return false;
    }

    ZSTD_CStream* stream = ZSTD_createCStream();
    if (!stream) {
        if (error_out) *error_out = "could not allocate zstd compressor";
        return false;
    }

    size_t init_ret = ZSTD_initCStream(stream, level);
    if (ZSTD_isError(init_ret)) {
        if (error_out) *error_out = ZSTD_getErrorName(init_ret);
        ZSTD_freeCStream(stream);
        return false;
    }

    std::vector<char> input_buffer(ZSTD_CStreamInSize());
    std::vector<char> output_buffer(ZSTD_CStreamOutSize());

    while (input.read(input_buffer.data(), static_cast<std::streamsize>(input_buffer.size())) || input.gcount() > 0) {
        ZSTD_inBuffer in = {input_buffer.data(), static_cast<size_t>(input.gcount()), 0};
        while (in.pos < in.size) {
            ZSTD_outBuffer out = {output_buffer.data(), output_buffer.size(), 0};
            size_t ret = ZSTD_compressStream(stream, &out, &in);
            if (ZSTD_isError(ret)) {
                if (error_out) *error_out = ZSTD_getErrorName(ret);
                ZSTD_freeCStream(stream);
                return false;
            }
            if (out.pos > 0) {
                output.write(output_buffer.data(), static_cast<std::streamsize>(out.pos));
                if (!output) {
                    if (error_out) *error_out = "failed while writing compressed archive";
                    ZSTD_freeCStream(stream);
                    return false;
                }
            }
        }
    }

    size_t remaining = 0;
    do {
        ZSTD_outBuffer out = {output_buffer.data(), output_buffer.size(), 0};
        remaining = ZSTD_endStream(stream, &out);
        if (ZSTD_isError(remaining)) {
            if (error_out) *error_out = ZSTD_getErrorName(remaining);
            ZSTD_freeCStream(stream);
            return false;
        }
        if (out.pos > 0) {
            output.write(output_buffer.data(), static_cast<std::streamsize>(out.pos));
            if (!output) {
                if (error_out) *error_out = "failed while writing compressed archive";
                ZSTD_freeCStream(stream);
                return false;
            }
        }
    } while (remaining != 0);

    ZSTD_freeCStream(stream);
    return true;
}

bool read_tar_entries(const std::string& tar_path, std::vector<TarEntry>& out_entries, std::string* error_out = nullptr) {
    out_entries.clear();
    if (error_out) error_out->clear();

    std::ifstream input(tar_path, std::ios::binary);
    if (!input) {
        if (error_out) *error_out = "could not open tar archive";
        return false;
    }

    std::map<std::string, std::string> global_pax;
    std::map<std::string, std::string> pending_pax;
    std::string pending_long_name;
    std::string pending_long_link;

    while (true) {
        char header[TAR_BLOCK_SIZE];
        if (!read_exact(input, header, sizeof(header))) break;
        if (is_all_zero_block(header, sizeof(header))) break;

        uint64_t raw_size = 0;
        if (!parse_tar_number(header + 124, 12, raw_size)) {
            if (error_out) *error_out = "failed to parse tar size";
            return false;
        }

        std::string name = trim_tar_string(header, 100);
        std::string prefix = trim_tar_string(header + 345, 155);
        if (!prefix.empty()) name = prefix + "/" + name;
        std::string link_name = trim_tar_string(header + 157, 100);
        uint64_t mode_value = 0;
        parse_tar_number(header + 100, 8, mode_value);

        char typeflag = header[156] == '\0' ? '0' : header[156];
        std::streamoff data_offset = input.tellg();

        if (typeflag == 'x' || typeflag == 'g' || typeflag == 'L' || typeflag == 'K') {
            std::string payload(raw_size, '\0');
            if (raw_size > 0) {
                input.read(&payload[0], static_cast<std::streamsize>(raw_size));
                if (!input) {
                    if (error_out) *error_out = "failed to read tar metadata payload";
                    return false;
                }
            }

            if (typeflag == 'x') pending_pax = parse_pax_records(payload);
            else if (typeflag == 'g') global_pax = parse_pax_records(payload);
            else if (typeflag == 'L') pending_long_name = std::string(payload.c_str());
            else if (typeflag == 'K') pending_long_link = std::string(payload.c_str());

            uint64_t padding = align_up_tar(raw_size) - raw_size;
            if (padding > 0) input.seekg(static_cast<std::streamoff>(padding), std::ios::cur);
            continue;
        }

        if (pending_pax.count("path")) name = pending_pax["path"];
        if (global_pax.count("path") && pending_pax.count("path") == 0) name = global_pax["path"];
        if (!pending_long_name.empty()) name = pending_long_name;

        if (pending_pax.count("linkpath")) link_name = pending_pax["linkpath"];
        if (global_pax.count("linkpath") && pending_pax.count("linkpath") == 0) link_name = global_pax["linkpath"];
        if (!pending_long_link.empty()) link_name = pending_long_link;

        if (pending_pax.count("size")) raw_size = static_cast<uint64_t>(std::strtoull(pending_pax["size"].c_str(), nullptr, 10));
        if (global_pax.count("size") && pending_pax.count("size") == 0) {
            raw_size = static_cast<uint64_t>(std::strtoull(global_pax["size"].c_str(), nullptr, 10));
        }

        TarEntry entry;
        entry.path = name;
        entry.link_target = link_name;
        entry.mode = static_cast<mode_t>(mode_value & 07777);
        entry.size = raw_size;
        entry.data_offset = static_cast<uint64_t>(data_offset);
        if (typeflag == '0') entry.type = TarEntryType::Regular;
        else if (typeflag == '5') entry.type = TarEntryType::Directory;
        else if (typeflag == '2') entry.type = TarEntryType::Symlink;
        else if (typeflag == '1') entry.type = TarEntryType::Hardlink;
        else entry.type = TarEntryType::Unsupported;
        out_entries.push_back(entry);

        input.seekg(static_cast<std::streamoff>(align_up_tar(raw_size)), std::ios::cur);
        pending_pax.clear();
        pending_long_name.clear();
        pending_long_link.clear();
    }

    return true;
}

bool read_tar_entry_file(const std::string& tar_path, const TarEntry& entry, std::vector<char>& out_data, std::string* error_out = nullptr) {
    out_data.clear();
    if (entry.type != TarEntryType::Regular) {
        if (error_out) *error_out = "tar member is not a regular file";
        return false;
    }

    std::ifstream input(tar_path, std::ios::binary);
    if (!input) {
        if (error_out) *error_out = "could not open tar archive";
        return false;
    }
    input.seekg(static_cast<std::streamoff>(entry.data_offset), std::ios::beg);
    if (!input) {
        if (error_out) *error_out = "could not seek to tar member";
        return false;
    }

    out_data.resize(static_cast<size_t>(entry.size));
    if (!out_data.empty()) {
        input.read(out_data.data(), static_cast<std::streamsize>(entry.size));
        if (!input) {
            if (error_out) *error_out = "failed to read tar member payload";
            return false;
        }
    }
    return true;
}

bool tar_list_entries(const std::string& tar_path, std::vector<TarEntry>& out_entries, std::string* error_out = nullptr) {
    return read_tar_entries(tar_path, out_entries, error_out);
}

bool tar_read_file(const std::string& tar_path, const std::string& member_path, std::string& out_content, std::string* error_out = nullptr) {
    out_content.clear();
    std::vector<TarEntry> entries;
    if (!read_tar_entries(tar_path, entries, error_out)) return false;

    std::string target;
    if (!sanitize_relative_path(member_path, target)) {
        if (error_out) *error_out = "invalid tar member path";
        return false;
    }

    for (const auto& entry : entries) {
        std::string candidate;
        if (!sanitize_relative_path(entry.path, candidate)) continue;
        if (candidate != target) continue;

        std::vector<char> data;
        if (!read_tar_entry_file(tar_path, entry, data, error_out)) return false;
        out_content.assign(data.begin(), data.end());
        return true;
    }

    if (error_out) *error_out = "tar member not found";
    return false;
}

bool tar_extract_to_directory(
    const std::string& tar_path,
    const std::string& output_dir,
    const TarExtractOptions& options = {},
    std::string* error_out = nullptr
) {
    if (error_out) error_out->clear();
    if (!ensure_directory(output_dir)) {
        if (error_out) *error_out = "failed to create extraction root";
        return false;
    }

    std::vector<TarEntry> entries;
    if (!read_tar_entries(tar_path, entries, error_out)) return false;

    std::vector<std::pair<TarEntry, std::string>> pending_hardlinks;
    for (const auto& entry : entries) {
        std::string stripped_path;
        if (!strip_path_components(entry.path, options.strip_components, stripped_path)) {
            if (error_out) *error_out = "unsafe archive member path: " + entry.path;
            return false;
        }
        if (stripped_path.empty()) continue;

        std::string dest_path = output_dir + "/" + stripped_path;
        if (!ensure_parent_directory(dest_path)) {
            if (error_out) *error_out = "failed to create parent directory for " + stripped_path;
            return false;
        }

        if (entry.type == TarEntryType::Directory) {
            if (!ensure_directory(dest_path)) {
                if (error_out) *error_out = "failed to create directory " + stripped_path;
                return false;
            }
            chmod(dest_path.c_str(), entry.mode & 07777);
            continue;
        }

        if (entry.type == TarEntryType::Symlink) {
            remove_existing_path(dest_path);
            if (symlink(entry.link_target.c_str(), dest_path.c_str()) != 0) {
                if (error_out) *error_out = "failed to create symlink " + stripped_path;
                return false;
            }
            continue;
        }

        if (entry.type == TarEntryType::Hardlink) {
            if (!options.materialize_hardlinks) continue;
            pending_hardlinks.push_back(std::make_pair(entry, stripped_path));
            continue;
        }

        if (entry.type != TarEntryType::Regular) {
            if (error_out) *error_out = "unsupported tar member type for " + stripped_path;
            return false;
        }

        std::vector<char> data;
        if (!read_tar_entry_file(tar_path, entry, data, error_out)) return false;
        if (!write_bytes_to_file(dest_path, data, entry.mode, error_out)) return false;
    }

    for (const auto& pending : pending_hardlinks) {
        const TarEntry& entry = pending.first;
        const std::string& stripped_path = pending.second;
        std::string target_path;
        if (!strip_path_components(entry.link_target, options.strip_components, target_path)) {
            if (error_out) *error_out = "unsafe hardlink target for " + stripped_path;
            return false;
        }
        if (target_path.empty()) {
            if (error_out) *error_out = "empty hardlink target for " + stripped_path;
            return false;
        }

        std::string source_path = output_dir + "/" + target_path;
        std::string dest_path = output_dir + "/" + stripped_path;
        if (!copy_regular_file(source_path, dest_path, entry.mode, error_out)) return false;
    }

    return true;
}

bool append_file_to_stream(std::ofstream& output, const std::string& source_path, std::string* error_out = nullptr) {
    std::ifstream input(source_path, std::ios::binary);
    if (!input) {
        if (error_out) *error_out = "could not open " + source_path;
        return false;
    }

    char buffer[32768];
    while (input.read(buffer, sizeof(buffer)) || input.gcount() > 0) {
        output.write(buffer, input.gcount());
        if (!output) {
            if (error_out) *error_out = "failed while writing tar payload";
            return false;
        }
    }
    if (!input.eof()) {
        if (error_out) *error_out = "failed while reading " + source_path;
        return false;
    }
    return true;
}

void write_tar_padding(std::ofstream& output, uint64_t size) {
    uint64_t padding = align_up_tar(size) - size;
    static const char zeroes[TAR_BLOCK_SIZE] = {};
    while (padding > 0) {
        size_t chunk = static_cast<size_t>(std::min<uint64_t>(padding, TAR_BLOCK_SIZE));
        output.write(zeroes, static_cast<std::streamsize>(chunk));
        padding -= chunk;
    }
}

bool write_tar_header(
    std::ofstream& output,
    const std::string& archive_path,
    TarEntryType type,
    mode_t mode,
    uint64_t size,
    const std::string& link_target,
    std::string* error_out = nullptr
) {
    std::array<char, TAR_BLOCK_SIZE> header {};

    std::string name = archive_path;
    std::string prefix;
    if (!split_ustar_name_prefix(archive_path, name, prefix)) {
        if (error_out) *error_out = "archive path is too long for tar header: " + archive_path;
        return false;
    }

    std::memcpy(header.data(), name.c_str(), std::min<size_t>(name.size(), 100));
    write_tar_octal(header.data() + 100, 8, mode & 07777);
    write_tar_octal(header.data() + 108, 8, 0);
    write_tar_octal(header.data() + 116, 8, 0);
    write_tar_octal(header.data() + 124, 12, size);
    write_tar_octal(header.data() + 136, 12, static_cast<uint64_t>(std::time(nullptr)));
    std::memset(header.data() + 148, ' ', 8);
    header[156] = type == TarEntryType::Directory ? '5'
                 : type == TarEntryType::Symlink ? '2'
                 : type == TarEntryType::Hardlink ? '1'
                 : '0';
    if (!link_target.empty()) {
        std::memcpy(header.data() + 157, link_target.c_str(), std::min<size_t>(link_target.size(), 100));
    }
    std::memcpy(header.data() + 257, "ustar", 5);
    std::memcpy(header.data() + 263, "00", 2);
    if (!prefix.empty()) {
        std::memcpy(header.data() + 345, prefix.c_str(), std::min<size_t>(prefix.size(), 155));
    }

    unsigned int checksum = 0;
    for (char byte : header) checksum += static_cast<unsigned char>(byte);
    std::snprintf(header.data() + 148, 8, "%06o", checksum);
    header[154] = '\0';
    header[155] = ' ';

    output.write(header.data(), static_cast<std::streamsize>(header.size()));
    if (!output) {
        if (error_out) *error_out = "failed while writing tar header";
        return false;
    }
    return true;
}

bool write_tar_pax_record(std::ofstream& output, const std::map<std::string, std::string>& records, size_t index, std::string* error_out = nullptr) {
    if (records.empty()) return true;

    std::string payload = build_pax_payload(records);
    std::string header_name = "PaxHeaders/" + std::to_string(index);
    if (!write_tar_header(output, header_name, TarEntryType::Regular, 0644, payload.size(), "", error_out)) {
        return false;
    }
    output.write(payload.data(), static_cast<std::streamsize>(payload.size()));
    if (!output) {
        if (error_out) *error_out = "failed while writing pax payload";
        return false;
    }
    write_tar_padding(output, payload.size());
    return true;
}

bool tar_create_from_sources(const std::vector<TarSource>& sources, const std::string& output_tar, std::string* error_out = nullptr) {
    if (error_out) error_out->clear();
    if (!ensure_parent_directory(output_tar)) {
        if (error_out) *error_out = "could not create parent directory for " + output_tar;
        return false;
    }

    std::ofstream output(output_tar, std::ios::binary);
    if (!output) {
        if (error_out) *error_out = "could not open " + output_tar;
        return false;
    }

    size_t pax_index = 0;
    for (const auto& source : sources) {
        std::map<std::string, std::string> pax_records;
        std::string name = source.archive_path;
        std::string prefix;
        if (!split_ustar_name_prefix(source.archive_path, name, prefix)) {
            pax_records["path"] = source.archive_path;
        }
        if (source.link_target.size() > 100) {
            pax_records["linkpath"] = source.link_target;
        }

        uint64_t size = 0;
        if (source.type == TarEntryType::Regular) {
            struct stat st {};
            if (stat(source.source_path.c_str(), &st) != 0) {
                if (error_out) *error_out = "could not stat " + source.source_path;
                return false;
            }
            size = static_cast<uint64_t>(st.st_size);
        }

        if (!write_tar_pax_record(output, pax_records, pax_index++, error_out)) return false;

        std::string header_path = pax_records.empty() ? source.archive_path : ("pax-entry-" + std::to_string(pax_index));
        std::string header_link = source.link_target.size() <= 100 ? source.link_target : "";
        if (!write_tar_header(output, header_path, source.type, source.mode, size, header_link, error_out)) return false;

        if (source.type == TarEntryType::Regular) {
            if (!append_file_to_stream(output, source.source_path, error_out)) return false;
            write_tar_padding(output, size);
        }
    }

    static const char zero_block[TAR_BLOCK_SIZE] = {};
    output.write(zero_block, TAR_BLOCK_SIZE);
    output.write(zero_block, TAR_BLOCK_SIZE);
    return static_cast<bool>(output);
}

bool collect_directory_sources(const std::string& root_dir, const std::string& relative_dir, std::vector<TarSource>& out_sources, std::string* error_out = nullptr) {
    std::string full_dir = relative_dir.empty() ? root_dir : (root_dir + "/" + relative_dir);
    DIR* dir = opendir(full_dir.c_str());
    if (!dir) {
        if (error_out) *error_out = "could not open " + full_dir;
        return false;
    }

    std::vector<std::string> names;
    struct dirent* entry = nullptr;
    while ((entry = readdir(dir)) != nullptr) {
        std::string name = entry->d_name;
        if (name == "." || name == "..") continue;
        names.push_back(name);
    }
    closedir(dir);
    std::sort(names.begin(), names.end());

    for (const auto& name : names) {
        std::string rel_path = relative_dir.empty() ? name : (relative_dir + "/" + name);
        std::string full_path = root_dir + "/" + rel_path;
        struct stat st {};
        if (lstat(full_path.c_str(), &st) != 0) {
            if (error_out) *error_out = "could not stat " + full_path;
            return false;
        }

        TarSource source;
        source.archive_path = rel_path;
        source.source_path = full_path;
        source.mode = st.st_mode & 07777;

        if (S_ISDIR(st.st_mode)) {
            source.type = TarEntryType::Directory;
            out_sources.push_back(source);
            if (!collect_directory_sources(root_dir, rel_path, out_sources, error_out)) return false;
            continue;
        }
        if (S_ISLNK(st.st_mode)) {
            std::vector<char> link_target(static_cast<size_t>(st.st_size) + 2, '\0');
            ssize_t len = readlink(full_path.c_str(), link_target.data(), link_target.size() - 1);
            if (len < 0) {
                if (error_out) *error_out = "could not read symlink " + full_path;
                return false;
            }
            source.type = TarEntryType::Symlink;
            source.link_target.assign(link_target.data(), static_cast<size_t>(len));
            out_sources.push_back(source);
            continue;
        }
        if (S_ISREG(st.st_mode)) {
            source.type = TarEntryType::Regular;
            out_sources.push_back(source);
            continue;
        }

        if (error_out) *error_out = "unsupported filesystem entry type in " + full_path;
        return false;
    }

    return true;
}

bool tar_create_from_directory(const std::string& root_dir, const std::string& output_tar, std::string* error_out = nullptr) {
    std::vector<TarSource> sources;
    if (!collect_directory_sources(root_dir, "", sources, error_out)) return false;
    return tar_create_from_sources(sources, output_tar, error_out);
}

bool extract_ar_archive_to_directory(const std::string& ar_path, const std::string& output_dir, std::string* error_out = nullptr) {
    if (error_out) error_out->clear();
    if (!ensure_directory(output_dir)) {
        if (error_out) *error_out = "failed to create ar extraction directory";
        return false;
    }

    std::ifstream input(ar_path, std::ios::binary);
    if (!input) {
        if (error_out) *error_out = "could not open Debian archive";
        return false;
    }

    char magic[8];
    if (!read_exact(input, magic, sizeof(magic)) || std::memcmp(magic, "!<arch>\n", 8) != 0) {
        if (error_out) *error_out = "invalid Debian ar archive";
        return false;
    }

    std::string long_name_table;

    while (true) {
        char header[60];
        input.read(header, sizeof(header));
        if (input.gcount() == 0) break;
        if (static_cast<size_t>(input.gcount()) != sizeof(header)) {
            if (error_out) *error_out = "truncated ar member header";
            return false;
        }
        if (header[58] != '`' || header[59] != '\n') {
            if (error_out) *error_out = "invalid ar member trailer";
            return false;
        }

        std::string raw_name = trim_tar_string(header, 16);
        std::string size_text = trim_tar_string(header + 48, 10);
        size_t member_size = static_cast<size_t>(std::strtoull(size_text.c_str(), nullptr, 10));

        std::string resolved_name = raw_name;
        size_t payload_prefix = 0;
        if (raw_name == "//") {
            std::string table(member_size, '\0');
            if (member_size > 0) {
                input.read(&table[0], static_cast<std::streamsize>(member_size));
                if (!input) {
                    if (error_out) *error_out = "failed to read ar string table";
                    return false;
                }
            }
            long_name_table = table;
            if (member_size & 1) input.seekg(1, std::ios::cur);
            continue;
        }
        if (raw_name.rfind("#1/", 0) == 0) {
            payload_prefix = static_cast<size_t>(std::strtoull(raw_name.substr(3).c_str(), nullptr, 10));
            std::string long_name(payload_prefix, '\0');
            if (payload_prefix > 0) {
                input.read(&long_name[0], static_cast<std::streamsize>(payload_prefix));
                if (!input) {
                    if (error_out) *error_out = "failed to read ar long filename";
                    return false;
                }
            }
            resolved_name = long_name;
            member_size -= std::min(member_size, payload_prefix);
        } else if (!raw_name.empty() && raw_name[0] == '/' && raw_name.size() > 1 &&
                   raw_name.find_first_not_of("0123456789", 1) == std::string::npos &&
                   !long_name_table.empty()) {
            size_t offset = static_cast<size_t>(std::strtoull(raw_name.substr(1).c_str(), nullptr, 10));
            if (offset < long_name_table.size()) {
                size_t end = long_name_table.find("/\n", offset);
                if (end == std::string::npos) end = long_name_table.find('\n', offset);
                if (end == std::string::npos) end = long_name_table.size();
                resolved_name = long_name_table.substr(offset, end - offset);
            }
        } else if (!resolved_name.empty() && resolved_name.back() == '/') {
            resolved_name.pop_back();
        }

        std::vector<char> payload(member_size);
        if (member_size > 0) {
            input.read(payload.data(), static_cast<std::streamsize>(member_size));
            if (!input) {
                if (error_out) *error_out = "failed to read ar member payload";
                return false;
            }
        }
        if (((member_size + payload_prefix) & 1) != 0) input.seekg(1, std::ios::cur);

        std::string safe_name;
        if (!sanitize_relative_path(resolved_name, safe_name) || safe_name.empty()) {
            if (error_out) *error_out = "unsafe ar member name";
            return false;
        }

        if (!write_bytes_to_file(output_dir + "/" + safe_name, payload, 0644, error_out)) return false;
    }

    return true;
}

} // namespace GpkgArchive
