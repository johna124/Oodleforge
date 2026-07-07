#include "common.h"
#include "aes.h"
#include <cstdlib>
#include <zlib.h>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <atomic>
#include <vector>
#include <fstream>
#include <algorithm>
#include <stdexcept>
#include <cstring>
#include <iostream>
#include <sstream>
#include <iomanip>
#include <array>
#include <deque>
#include <set>
#include <chrono>

#ifdef __AVX2__
#include <immintrin.h>
#endif

namespace Logger {
    bool is_debug_enabled = false;
    std::ofstream debug_log;
    std::mutex log_mutex;
}

void* AES_Context_Create(const uint8_t* key) {
    if (!key) return nullptr;
    AES_ctx* ctx = (AES_ctx*)calloc(1, sizeof(AES_ctx));
    if (ctx) AES_init_ctx(ctx, key);
    return ctx;
}
void AES_Context_Destroy(void* ctx) { if (ctx) free(ctx); }
void AES_Context_Decrypt(void* ctx, uint8_t* buf, uint32_t len) {
    if (ctx && buf && len > 0) AES_CBC_decrypt_buffer((AES_ctx*)ctx, buf, len);
}
void AES_Context_Encrypt(void* ctx, uint8_t* buf, uint32_t len) {
    if (ctx && buf && len > 0) AES_CBC_encrypt_buffer((AES_ctx*)ctx, buf, len);
}

OodleLZ_Decompress_t* OodleLZ_Decompress = nullptr;
OodleLZ_Compress_t* OodleLZ_Compress   = nullptr;

bool LoadOodle() {
#ifdef _WIN32
    HMODULE hModule = LoadLibraryA("oo2core_9_win64.dll");
    if (!hModule) hModule = LoadLibraryA("oo2core_8_win64.dll");
    if (!hModule) return false;
    OodleLZ_Decompress = reinterpret_cast<OodleLZ_Decompress_t*>(GetProcAddress(hModule, "OodleLZ_Decompress"));
    OodleLZ_Compress   = reinterpret_cast<OodleLZ_Compress_t*>(GetProcAddress(hModule, "OodleLZ_Compress"));
#else
    void* hModule = dlopen("./liboo2core.so.9", RTLD_NOW);
    if (!hModule) hModule = dlopen("liboo2core.so.9", RTLD_NOW);
    if (!hModule) hModule = dlopen("liboo2core.so.8", RTLD_NOW);
    if (!hModule) return false;
    OodleLZ_Decompress = reinterpret_cast<OodleLZ_Decompress_t*>(dlsym(hModule, "OodleLZ_Decompress"));
    OodleLZ_Compress   = reinterpret_cast<OodleLZ_Compress_t*>(dlsym(hModule, "OodleLZ_Compress"));
#endif
    return (OodleLZ_Decompress != nullptr && OodleLZ_Compress != nullptr);
}

int64_t CompressAndVerify(int32_t method, int32_t level, const uint8_t* src, uint32_t usize,
    const uint8_t* expected, uint32_t expected_size, std::vector<uint8_t>& temp_buf,
    const OodleLZ_CompressOptions* opts, void* scratchMem, int64_t scratchSize)
{
    if (temp_buf.size() < usize * 2 + 4096) temp_buf.resize(usize * 2 + 4096);
    int64_t res = OodleLZ_Compress(method, src, usize, temp_buf.data(), level, (void*)opts, nullptr, nullptr, scratchMem, scratchSize);
    if (res > 0 && res == static_cast<int64_t>(expected_size)) {
        if (std::memcmp(expected, temp_buf.data(), expected_size) == 0) return res;
    }
    return -1;
}

bool TryMatchBlock(const std::shared_ptr<BlockTask>& task,
    const std::vector<int32_t>& all_methods,
    const std::vector<int32_t>& all_levels,
    std::set<std::pair<int32_t, int32_t>>& cache,
    std::shared_mutex& cache_mutex,
    int32_t& out_method, int32_t& out_level,
    const OodleLZ_CompressOptions* opts)
{
    thread_local std::vector<uint8_t> local_comp_buf;
    thread_local std::vector<uint8_t> scratch_mem;
    const size_t SCRATCH_SIZE = 8 * 1024 * 1024; 
    if (scratch_mem.size() < SCRATCH_SIZE) scratch_mem.resize(SCRATCH_SIZE);
    size_t required_size = std::max(static_cast<size_t>(task->usize) * 2, static_cast<size_t>(256 * 1024));
    if (local_comp_buf.size() < required_size) local_comp_buf.resize(required_size);

    {
        std::shared_lock<std::shared_mutex> lock(cache_mutex);
        for (const auto& pair : cache) {
            if (CompressAndVerify(pair.first, pair.second, task->dec_data.data(), task->usize,
                task->raw_win_buf.data(), task->csize, local_comp_buf, opts,
                scratch_mem.data(), scratch_mem.size()) > 0) {
                out_method = pair.first;
                out_level = pair.second;
                return true;
            }
        }
    }
    for (int32_t method : all_methods) {
        for (int32_t level : all_levels) {
            if (CompressAndVerify(method, level, task->dec_data.data(), task->usize,
                task->raw_win_buf.data(), task->csize, local_comp_buf, opts,
                scratch_mem.data(), scratch_mem.size()) > 0) {
                std::unique_lock<std::shared_mutex> lock(cache_mutex);
                cache.insert({method, level});
                out_method = method;
                out_level = level;
                return true;
            }
        }
    }
    return false;
}

static inline bool IsValidOodleCodec(uint8_t b1) {
    uint8_t base_b1 = b1 & 0x7F;
    return (base_b1 == 0x06 || base_b1 == 0x0A || base_b1 == 0x0C ||
            base_b1 == 0x0D || base_b1 == 0x0B);
}

uint32_t GetOodleBlockSize(const uint8_t* hdr, size_t available_len, uint8_t& codec_out) {
    if (available_len < Config::MIN_OODLE_BLOCK_SIZE) return 0;
    uint8_t b0 = hdr[0];
    if (b0 != 0x8C && b0 != 0xCC && b0 != 0x0C && b0 != 0x4C) return 0;
    bool compressed = (b0 == 0x8C || b0 == 0x0C);
    uint8_t b1 = hdr[1];
    if (compressed) {
        if (b1 == 0x06 || b1 == 0x86) codec_out = 8;
        else if (b1 == 0x0A || b1 == 0x8A) codec_out = 9;
        else if (b1 == 0x0C || b1 == 0x8C) codec_out = 11;
        else if (b1 == 0x0D || b1 == 0x8D) codec_out = 12;
        else if (b1 == 0x0B || b1 == 0x8B) codec_out = 13;
        else return 0;
        uint32_t header_size = (b1 & 0x80) ? 9 : 6;
        if (available_len < header_size) return 0;
        uint32_t csize = (static_cast<uint32_t>(hdr[2]) << 16) |
                         (static_cast<uint32_t>(hdr[3]) << 8) |
                         static_cast<uint32_t>(hdr[4]);
        csize += header_size;
        if (csize < Config::MIN_OODLE_BLOCK_SIZE || csize > 8 * 1024 * 1024) return 0;
        return csize;
    } else {
        if (b1 == 0x06) codec_out = 8;
        else if (b1 == 0x0A) codec_out = 9;
        else if (b1 == 0x0C) codec_out = 11;
        else return 0;
        return 262146;
    }
}

// ----- ThreadSafeReader --------------------------------------------------
struct ThreadSafeReader::Impl {
    std::string path_;
#ifdef _WIN32
    HANDLE hFile = INVALID_HANDLE_VALUE;
#else
    int fd_ = -1;
#endif
    Impl(const std::string& path) : path_(path) {
#ifdef _WIN32
        hFile = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
#else
        fd_ = open(path.c_str(), O_RDONLY);
#endif
    }
    ~Impl() {
#ifdef _WIN32
        if (hFile != INVALID_HANDLE_VALUE) CloseHandle(hFile);
#else
        if (fd_ != -1) close(fd_);
#endif
    }
    bool is_open() const {
#ifdef _WIN32
        return hFile != INVALID_HANDLE_VALUE;
#else
        return fd_ != -1;
#endif
    }
    uint64_t get_size() const {
#ifdef _WIN32
        LARGE_INTEGER size; return GetFileSizeEx(hFile, &size) ? size.QuadPart : 0;
#else
        struct stat st; return fstat(fd_, &st) == 0 ? st.st_size : 0;
#endif
    }
    size_t pread(char* dest, size_t count, uint64_t offset) const {
        size_t total_read = 0;
        int retry_count = 0;
        while (total_read < count) {
#ifdef _WIN32
            OVERLAPPED ol = {0}; ol.Offset = static_cast<DWORD>(offset + total_read); ol.OffsetHigh = static_cast<DWORD>((offset + total_read) >> 32);
            DWORD bytes_read = 0;
            BOOL success = ReadFile(hFile, dest + total_read, static_cast<DWORD>(count - total_read), &bytes_read, &ol);
            if (!success) {
                DWORD err = GetLastError();
                if (err == ERROR_HANDLE_EOF) break;
                if ((err == ERROR_LOCK_VIOLATION || err == ERROR_SEM_TIMEOUT) && retry_count++ < 10) {
                    Sleep(1); continue;
                }
                break;
            }
            if (bytes_read == 0) break;
            total_read += bytes_read;
#else
            ssize_t r = ::pread(fd_, dest + total_read, count - total_read, static_cast<off_t>(offset + total_read));
            if (r < 0) { if (errno == EINTR) continue; break; }
            if (r == 0) break;
            total_read += r;
#endif
        }
        return total_read;
    }
};

ThreadSafeReader::ThreadSafeReader(const std::string& path) : pImpl_(std::make_unique<Impl>(path)) {}
ThreadSafeReader::~ThreadSafeReader() = default;
bool ThreadSafeReader::is_open() const { return pImpl_->is_open(); }
uint64_t ThreadSafeReader::get_size() const { return pImpl_->get_size(); }
size_t ThreadSafeReader::pread(char* dest, size_t count, uint64_t offset) const { return pImpl_->pread(dest, count, offset); }

// ----- FastStreamWriter --------------------------------------------------
struct FastStreamWriter::Impl {
    std::ofstream file;
    std::vector<char> active_buf;
    std::vector<char> flush_buf;
    std::vector<char> disk_buf;
    size_t buffer_size;
    std::mutex mtx;
    std::condition_variable cv;
    std::thread flush_thread;
    bool flush_ready;
    bool done;
    std::atomic<bool> io_error;
    std::string last_error;
    
    std::atomic<uint64_t> total_bytes_written{0};

    Impl(size_t buf_sz = 32 * 1024 * 1024)
        : buffer_size(buf_sz), flush_ready(false), done(false), io_error(false) {
        active_buf.reserve(buffer_size);
        flush_buf.reserve(buffer_size);
        disk_buf.reserve(buffer_size);
    }
    ~Impl() { close(); }
    
    bool open(const std::string& path) {
        file.open(path, std::ios::binary);
        if (!file.is_open()) {
            last_error = "Failed to open output file: " + path;
            io_error.store(true, std::memory_order_release);
            return false;
        }
        flush_thread = std::thread(&Impl::flush_worker, this);
        return true;
    }
    void check_io() {
        if (io_error.load(std::memory_order_acquire))
            throw std::runtime_error("Async I/O Error: " + last_error);
    }
    void write(const char* data, size_t len) {
        if (io_error.load(std::memory_order_acquire)) return;
        
        total_bytes_written.fetch_add(len, std::memory_order_relaxed);
        
        std::unique_lock<std::mutex> lock(mtx);
        size_t offset = 0;
        const size_t FRACTURE_SIZE = 16 * 1024 * 1024;
        while (offset < len) {
            cv.wait(lock, [this]() {
                return !flush_ready || done || io_error.load(std::memory_order_acquire);
            });
            if (done || io_error.load(std::memory_order_acquire)) {
                check_io();
                return;
            }
            size_t space_left = buffer_size - active_buf.size();
            size_t chunk_to_write = std::min({len - offset, FRACTURE_SIZE, space_left});
            if (chunk_to_write == 0) {
                std::swap(flush_buf, active_buf);
                flush_ready = true;
                cv.notify_all();
                continue;
            }
            active_buf.insert(active_buf.end(), data + offset, data + offset + chunk_to_write);
            offset += chunk_to_write;
        }
    }
    void flush() {
        std::unique_lock<std::mutex> lock(mtx);
        if (io_error.load(std::memory_order_acquire)) return;
        cv.wait(lock, [this]() { return !flush_ready || done || io_error.load(std::memory_order_acquire); });
        if (io_error.load(std::memory_order_acquire)) return;
        if (!active_buf.empty()) {
            std::swap(flush_buf, active_buf);
            flush_ready = true;
            cv.notify_all();
        }
        cv.wait(lock, [this]() { return !flush_ready || done || io_error.load(std::memory_order_acquire); });
        if (io_error.load(std::memory_order_acquire)) return;
    }
    void close() {
        {
            std::unique_lock<std::mutex> lock(mtx);
            if (done) return;
            if (!active_buf.empty() && !io_error.load(std::memory_order_acquire)) {
                cv.wait(lock, [this]() { return !flush_ready || done || io_error.load(std::memory_order_acquire); });
                if (!io_error.load(std::memory_order_acquire)) {
                    std::swap(flush_buf, active_buf);
                    flush_ready = true;
                }
            }
            done = true;
            cv.notify_all();
        }
        if (flush_thread.joinable()) flush_thread.join();
        if (file.is_open()) file.close();
    }
    void flush_worker() {
        while (true) {
            std::unique_lock<std::mutex> lock(mtx);
            cv.wait(lock, [this]() { return flush_ready || done; });
            if (flush_ready) {
                std::swap(disk_buf, flush_buf);
                flush_ready = false;
                lock.unlock();
                cv.notify_all();
                if (!disk_buf.empty()) {
                    file.write(disk_buf.data(), disk_buf.size());
                    if (file.bad()) {
                        std::lock_guard<std::mutex> err_lock(mtx);
                        last_error = "Disk write failed. Disk full or hardware fault detected.";
                        io_error.store(true, std::memory_order_release);
                        done = true;
                        cv.notify_all();
                        return;
                    }
                    disk_buf.clear();
                }
            } else if (done) {
                break;
            }
        }
    }
};

FastStreamWriter::FastStreamWriter() : pImpl_(std::make_unique<Impl>()) {}
FastStreamWriter::~FastStreamWriter() = default;
bool FastStreamWriter::open(const std::string& path, size_t size) { return pImpl_->open(path); }
void FastStreamWriter::write(const char* data, size_t len) { pImpl_->write(data, len); }
void FastStreamWriter::flush() { pImpl_->flush(); }
void FastStreamWriter::close() { pImpl_->close(); }
bool FastStreamWriter::has_error() const { return pImpl_->io_error.load(std::memory_order_acquire); }

std::string FastStreamWriter::get_last_error() const {
    std::lock_guard<std::mutex> lock(pImpl_->mtx);
    return pImpl_->last_error;
}

void FastStreamWriter::clear_error() {
    pImpl_->io_error.store(false, std::memory_order_release);
    std::lock_guard<std::mutex> lock(pImpl_->mtx);
    pImpl_->last_error.clear();
}

uint64_t FastStreamWriter::tellp() const {
    return pImpl_->total_bytes_written.load(std::memory_order_relaxed);
}

void FastStreamWriter::seekp(uint64_t pos) {
    if (pImpl_->io_error.load(std::memory_order_acquire)) return;
    pImpl_->check_io();
    std::unique_lock<std::mutex> lock(pImpl_->mtx);
    pImpl_->cv.wait(lock, [this]() {
        return !pImpl_->flush_ready || pImpl_->done || pImpl_->io_error.load(std::memory_order_acquire);
    });
    if (pImpl_->io_error.load(std::memory_order_acquire)) return;
    if (!pImpl_->active_buf.empty()) {
        std::swap(pImpl_->flush_buf, pImpl_->active_buf);
        pImpl_->flush_ready = true;
        pImpl_->cv.notify_all();
    }
    pImpl_->cv.wait(lock, [this]() {
        return !pImpl_->flush_ready || pImpl_->done || pImpl_->io_error.load(std::memory_order_acquire);
    });
    if (pImpl_->io_error.load(std::memory_order_acquire)) return;
    pImpl_->check_io();
    
    pImpl_->file.clear();
    pImpl_->file.seekp(static_cast<std::streamoff>(pos));
    
    // [FIX] Update atomic counter on seek to ensure lock-free tellp() stays perfectly accurate.
    if (pImpl_->file.good()) {
        pImpl_->total_bytes_written.store(pos, std::memory_order_relaxed);
    } else {
        pImpl_->last_error = "seekp() failed at offset " + std::to_string(pos);
        pImpl_->io_error.store(true, std::memory_order_release);
        pImpl_->cv.notify_all();
    }
}

// ----- BlockScanner ------------------------------------------------------
BlockScanner::BlockScanner(ThreadSafeReader& reader, uint64_t file_size, bool use_aes, const std::vector<uint8_t>& aes_key, size_t win_size)
    : reader_(reader), file_size_(file_size), use_aes_(use_aes), aes_key_(aes_key) {
    if (use_aes_) win_size = (win_size + 15) & ~15;
    win_size_ = win_size;
    win_buf_.reset(new uint8_t[win_size]);
    if (use_aes_) {
        win_buf_dec_.reset(new uint8_t[win_size]);
        aes_ctx_ = AESContextPtr(AES_Context_Create(aes_key_.data()));
    }
}
BlockScanner::~BlockScanner() = default;

bool BlockScanner::ensure_window(uint64_t pos, size_t needed) {
    if (pos < win_start_ || pos + needed > win_start_ + win_len_) {
        uint64_t aligned_pos = pos & ~15;
        if (aligned_pos >= win_start_ && aligned_pos < win_start_ + win_len_) {
            size_t keep = static_cast<size_t>((win_start_ + win_len_) - aligned_pos);
            std::memmove(win_buf_.get(), &win_buf_[aligned_pos - win_start_], keep);
            if (use_aes_) std::memmove(win_buf_dec_.get(), &win_buf_dec_[aligned_pos - win_start_], keep);
            win_start_ = aligned_pos;
            size_t bytes_to_read = win_size_ - keep;
            size_t read_bytes = reader_.pread(reinterpret_cast<char*>(&win_buf_[keep]), bytes_to_read, win_start_ + keep);
            if (use_aes_ && read_bytes > 0) {
                size_t decrypt_len = (read_bytes) & ~15;
                if (decrypt_len > 0) {
                    std::memcpy(&win_buf_dec_[keep], &win_buf_[keep], decrypt_len);
                    AES_Context_Decrypt(aes_ctx_.get(), &win_buf_dec_[keep], static_cast<uint32_t>(decrypt_len));
                }
            }
            win_len_ = keep + read_bytes;
        } else {
            win_start_ = aligned_pos;
            size_t read_bytes = reader_.pread(reinterpret_cast<char*>(win_buf_.get()), win_size_, win_start_);
            if (use_aes_ && read_bytes > 0) {
                size_t decrypt_len = (read_bytes) & ~15;
                if (decrypt_len > 0) {
                    std::memcpy(win_buf_dec_.get(), win_buf_.get(), decrypt_len);
                    AES_Context_Decrypt(aes_ctx_.get(), win_buf_dec_.get(), static_cast<uint32_t>(decrypt_len));
                }
            }
            win_len_ = read_bytes;
        }
    }
    return win_len_ > 0;
}

bool BlockScanner::find_next_magic(uint64_t& pos, uint64_t limit) {
    while (pos < limit && pos < file_size_) {
        if (!ensure_window(pos, 16)) return false;
        size_t internal_idx = static_cast<size_t>(pos - win_start_);
        size_t max_check = std::min(win_len_ - internal_idx, static_cast<size_t>(file_size_ - pos));
        if (max_check == 0) { pos++; continue; }
        
        const uint8_t* search_start = use_aes_ ? &win_buf_dec_[internal_idx] : &win_buf_[internal_idx];
        size_t found_offset = max_check;

#ifdef __AVX2__
        const __m256i magic_8C = _mm256_set1_epi8(0x8C);
        const __m256i magic_CC = _mm256_set1_epi8(0xCC);
        const __m256i magic_0C = _mm256_set1_epi8(0x0C);
        const __m256i magic_4C = _mm256_set1_epi8(0x4C);
        
        size_t i = 0;
        for (; i + 32 <= max_check; i += 32) {
            __m256i data = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(search_start + i));
            int mask_8C = _mm256_movemask_epi8(_mm256_cmpeq_epi8(data, magic_8C));
            int mask_CC = _mm256_movemask_epi8(_mm256_cmpeq_epi8(data, magic_CC));
            int mask_0C = _mm256_movemask_epi8(_mm256_cmpeq_epi8(data, magic_0C));
            int mask_4C = _mm256_movemask_epi8(_mm256_cmpeq_epi8(data, magic_4C));
            
            int combined = mask_8C | mask_CC | mask_0C | mask_4C;
            if (combined) {
                found_offset = i + __builtin_ctz(combined);
                break;
            }
        }
        if (found_offset == max_check && i < max_check) {
            for (size_t j = i; j < max_check; j++) {
                uint8_t b = search_start[j];
                if (b == 0x8C || b == 0xCC || b == 0x0C || b == 0x4C) {
                    found_offset = j; break;
                }
            }
        }
#else
        for (size_t j = 0; j < max_check; j++) {
            uint8_t b = search_start[j];
            if (b == 0x8C || b == 0xCC || b == 0x0C || b == 0x4C) {
                found_offset = j; break;
            }
        }
#endif

        if (found_offset < max_check) {
            pos += found_offset;
            return true;
        }
        pos += max_check;
    }
    return false;
}

static uint32_t FindCompressedSize(const uint8_t* src, size_t max_len, uint32_t usize, uint8_t* temp_dec_buf) {
    uint8_t codec;
    uint32_t header_csize = GetOodleBlockSize(src, max_len, codec);
    if (header_csize > 0 && header_csize <= max_len) {
        if (OodleLZ_Decompress(src, header_csize, temp_dec_buf, usize, 0, 0, 0,
            nullptr, 0, nullptr, nullptr, nullptr, 0, 0, 0) == static_cast<int64_t>(usize)) {
            return header_csize;
        }
    }
    size_t high = std::min(max_len, static_cast<size_t>(16 * 1024 * 1024));
    size_t low = 8;
    uint32_t csize = 0;
    while (low <= high) {
        size_t mid = low + (high - low) / 2;
        if (OodleLZ_Decompress(src, static_cast<int>(mid), temp_dec_buf, usize, 0, 0, 0,
            nullptr, 0, nullptr, nullptr, nullptr, 0, 0, 0) == static_cast<int64_t>(usize)) {
            csize = static_cast<uint32_t>(mid);
            high = mid - 1;
        } else {
            low = mid + 1;
        }
    }
    return csize;
}

static inline uint32_t ReadBE24(const uint8_t* data) {
    return (data[0] << 16) | (data[1] << 8) | data[2];
}

uint64_t BlockScanner::WalkOodleChain(uint64_t start_pos, uint8_t& codec_out, bool& is_valid) {
    uint64_t offset = start_pos;
    uint64_t c_size_total = 0;
    bool first = true;
    is_valid = false;
    codec_out = 0;
    uint32_t iteration = 0;
    auto read_bytes = [&](uint64_t off, uint8_t* out, size_t count) -> bool {
        if (off + count > file_size_) return false;
        if (!ensure_window(off, count)) return false;
        if (off < win_start_ || off + count > win_start_ + win_len_) return false;
        uint8_t* src_buf = use_aes_ ? win_buf_dec_.get() : win_buf_.get();
        std::memcpy(out, &src_buf[off - win_start_], count);
        return true;
    };
    while (true) {
        if (++iteration > Config::MAX_WALK_ITERATIONS) break;
        uint64_t rem_size = file_size_ - offset;
        uint8_t hdr[16];
        if (!read_bytes(offset, hdr, 16)) break;
        if (first) {
            diagnostics_.walk_called++;
            if (rem_size < Config::MIN_OODLE_BLOCK_SIZE + 4) { diagnostics_.walk_bounds_fail++; return 0; }
            uint8_t b0 = hdr[0];
            if (b0 != 0x8C && b0 != 0xCC) { diagnostics_.walk_not_a_chain_start++; return 0; }
            uint8_t b1 = hdr[1];
            if (!IsValidOodleCodec(b1)) { diagnostics_.walk_first_consistency_fail++; return 0; }
            if ((b1 & 0x7F) == 0x06) codec_out = 8;
            else if ((b1 & 0x7F) == 0x0A) codec_out = 9;
            else if ((b1 & 0x7F) == 0x0C) codec_out = 11;
            else if ((b1 & 0x7F) == 0x0D) codec_out = 12;
            else if ((b1 & 0x7F) == 0x0B) codec_out = 13;
            else { diagnostics_.walk_first_consistency_fail++; return 0; }
        } else {
            if (rem_size < 10) break;
            uint8_t b0 = hdr[0];
            if (b0 != 0x0C && b0 != 0x4C) break;
            if (!IsValidOodleCodec(hdr[1])) break;
        }
        bool compressed_seg = (hdr[0] == 0x8C || (!first && hdr[0] == 0x0C));
        uint8_t b1 = hdr[1];
        if (compressed_seg) {
            uint32_t i = 0;
            uint32_t header_size = (b1 & 0x80) ? 9 : 6;
            if (IsValidOodleCodec(b1)) i = ReadBE24(hdr + 2) + header_size;
            else break;
            if (i == 0) break;
            if (first && i < Config::MIN_VALID_FIRST_SEGMENT) { diagnostics_.walk_first_too_small++; return 0; }
            if (c_size_total + i > file_size_ - start_pos) { diagnostics_.walk_bounds_fail++; return 0; }
            if (i == 0x00080005) { is_valid = false; return 0; }
            c_size_total += i;
            offset += i;
            first = false;
        } else {
            if (IsValidOodleCodec(b1)) {
                const uint32_t BLK_SIZE = 262144;
                if (c_size_total + BLK_SIZE + 3 <= file_size_ - start_pos) {
                    uint8_t end_bytes[2];
                    if (read_bytes(offset + BLK_SIZE + 2, end_bytes, 2)) {
                        if ((end_bytes[0] == 0x0C || end_bytes[0] == 0x4C) && end_bytes[1] == b1) {
                            c_size_total += BLK_SIZE + 2;
                            offset += BLK_SIZE + 2;
                            first = false;
                            continue;
                        }
                    }
                }
                if (!first && c_size_total + 8 <= file_size_ - start_pos) {
                    c_size_total += 10;
                    break;
                }
                break;
            } else break;
        }
    }
    if (c_size_total > 0) {
        is_valid = true;
        diagnostics_.walk_succeeded++;
        return c_size_total;
    }
    return 0;
}

std::shared_ptr<BlockTask> BlockScanner::extract_next_block(uint64_t& pos, uint64_t limit, const std::vector<uint32_t>& test_sizes) {
    std::vector<uint8_t> dec_buf;
    uint32_t max_usize = 4 * 1024 * 1024;
    dec_buf.resize(max_usize);
    auto skip_to_next_magic = [&](const uint8_t* current_hdr, size_t pass_sz) {
        if (pass_sz <= 1) { pos += 16; return; }
        size_t min_dist = 16;
        for (uint8_t magic : {0x8C, 0xCC, 0x0C, 0x4C}) {
            const uint8_t* f = static_cast<const uint8_t*>(std::memchr(current_hdr + 1, magic, pass_sz - 1));
            if (f) {
                size_t dist = static_cast<size_t>(f - current_hdr);
                if (dist > 0 && dist < min_dist) min_dist = dist;
            }
        }
        pos += min_dist;
    };
    while (pos < limit && pos < file_size_) {
        if (!find_next_magic(pos, limit)) break;
        size_t needed = std::min(static_cast<uint64_t>(2 * 1024 * 1024), file_size_ - pos);
        if (!ensure_window(pos, needed)) break;
        size_t available = (win_start_ + win_len_ > pos) ? static_cast<size_t>(win_start_ + win_len_ - pos) : 0;
        uint8_t* active_buf = use_aes_ ? win_buf_dec_.get() : win_buf_.get();
        uint8_t* hdr = &active_buf[pos - win_start_];
        size_t pass_size = std::min(available, static_cast<size_t>(file_size_ - pos));
        diagnostics_.magic_candidates_found++;
        uint8_t b1 = hdr[1];
        if (!IsValidOodleCodec(b1)) {
            diagnostics_.rejected_by_fast_rejection++;
            if (pass_size > 1) skip_to_next_magic(hdr, pass_size);
            else pos += 16;
            continue;
        }
        diagnostics_.passed_fast_rejection++;
        uint8_t codec = 0;
        uint32_t csize = GetOodleBlockSize(hdr, pass_size, codec);
        if (csize > 0 && csize <= pass_size) {
            int64_t test_usize = OodleLZ_Decompress(hdr, csize, dec_buf.data(), max_usize, 0, 0, 0, nullptr, 0, nullptr, nullptr, nullptr, 0, 0, 0);
            bool continuation_follows = false;
            if (test_usize > 0 && test_usize <= max_usize && csize + 2 <= pass_size) {
                uint8_t next0 = hdr[csize];
                if (next0 == 0x0C || next0 == 0x4C) {
                    if (IsValidOodleCodec(hdr[csize + 1])) continuation_follows = true;
                }
            }
            if (!continuation_follows && test_usize > 0 && test_usize <= max_usize) {
                auto task = std::make_shared<BlockTask>();
                task->pos = pos;
                task->usize = static_cast<uint32_t>(test_usize);
                task->csize = csize;
                task->matched_method = codec;
                task->is_encrypted = use_aes_ && (win_buf_[pos - win_start_] != active_buf[pos - win_start_]);
                task->raw_win_buf.assign(hdr, hdr + csize);
                dec_buf.resize(test_usize);
                task->dec_data = std::move(dec_buf);
                dec_buf.resize(max_usize);
                pos += csize;
                diagnostics_.blocks_validated++;
                return task;
            }
        }
        uint8_t walk_codec = 0;
        bool walk_valid = false;
        uint64_t walk_csize = WalkOodleChain(pos, walk_codec, walk_valid);
        if (walk_valid && walk_csize >= Config::MIN_OODLE_BLOCK_SIZE && walk_csize <= pass_size) {
            if (!ensure_window(pos, static_cast<size_t>(walk_csize))) { pos++; continue; }
            active_buf = use_aes_ ? win_buf_dec_.get() : win_buf_.get();
            hdr = &active_buf[pos - win_start_];
            size_t max_safe_out = Config::MAX_DECOMP_BUF_SIZE;
            size_t probe_cap = std::min(max_safe_out, std::max(static_cast<size_t>(max_usize), static_cast<size_t>(walk_csize) * 8));
            if (chain_probe_buf_.size() < probe_cap) chain_probe_buf_.resize(probe_cap);
            int64_t dec_size = OodleLZ_Decompress(hdr, static_cast<int>(walk_csize), chain_probe_buf_.data(),
                static_cast<int>(probe_cap), 0, 0, 0, nullptr, 0, nullptr, nullptr, nullptr, 0, 0, 0);
            if (dec_size >= Config::MIN_OODLE_BLOCK_SIZE && dec_size <= static_cast<int64_t>(probe_cap)) {
                auto task = std::make_shared<BlockTask>();
                task->pos = pos;
                task->usize = static_cast<uint32_t>(dec_size);
                task->csize = static_cast<uint32_t>(walk_csize);
                task->matched_method = walk_codec;
                task->is_encrypted = use_aes_ && (win_buf_[pos - win_start_] != active_buf[pos - win_start_]);
                task->raw_win_buf.assign(hdr, hdr + walk_csize);
                chain_probe_buf_.resize(dec_size);
                task->dec_data = std::move(chain_probe_buf_);
                chain_probe_buf_.clear();
                pos += walk_csize;
                diagnostics_.blocks_validated++;
                return task;
            }
        }
        uint32_t found_usize = 0, found_csize = 0;
        for (uint32_t usize : test_sizes) {
            if (dec_buf.size() < usize) dec_buf.resize(usize);
            int64_t dec_result = OodleLZ_Decompress(hdr, pass_size, dec_buf.data(), usize, 0, 0, 0, nullptr, 0, nullptr, nullptr, nullptr, 0, 0, 0);
            if (dec_result == static_cast<int64_t>(usize)) {
                uint32_t max_needed = static_cast<uint32_t>(std::min(static_cast<uint64_t>(usize) + 65536, static_cast<uint64_t>(pass_size)));
                found_csize = FindCompressedSize(hdr, max_needed, usize, dec_buf.data());
                if (found_csize > 0 && found_csize < usize && found_csize >= Config::MIN_OODLE_BLOCK_SIZE) {
                    found_usize = usize;
                    break;
                }
                found_csize = 0;
            }
        }
        if (found_csize > 0) {
            diagnostics_.blocks_validated++;
            auto task = std::make_shared<BlockTask>();
            task->pos = pos;
            task->usize = found_usize;
            task->csize = found_csize;
            task->matched_method = (b1 & 0x7F) == 0x06 ? 8 : (b1 & 0x7F) == 0x0A ? 9 :
                                   (b1 & 0x7F) == 0x0C ? 11 : (b1 & 0x7F) == 0x0D ? 12 : 13;
            task->is_encrypted = use_aes_ && (win_buf_[pos - win_start_] != active_buf[pos - win_start_]);
            task->raw_win_buf.assign(hdr, hdr + found_csize);
            std::vector<uint8_t> final_dec(found_usize);
            OodleLZ_Decompress(hdr, found_csize, final_dec.data(), found_usize, 0, 0, 0, nullptr, 0, nullptr, nullptr, nullptr, 0, 0, 0);
            task->dec_data = std::move(final_dec);
            pos += found_csize;
            return task;
        }
        if (pass_size > 1) skip_to_next_magic(hdr, pass_size);
        else pos += 16;
    }
    return nullptr;
}

// ----- ThreadPool --------------------------------------------------------
ThreadPool::ThreadPool(size_t threads) : stop(false) {
    for (size_t i = 0; i < threads; ++i) {
        workers.emplace_back([this] {
            for (;;) {
                std::function<void()> task;
                {
                    std::unique_lock<std::mutex> lock(this->queue_mutex);
                    this->condition.wait(lock, [this] { return this->stop || !this->tasks.empty(); });
                    if (this->stop && this->tasks.empty()) return;
                    task = std::move(this->tasks.front());
                    this->tasks.pop();
                }
                try { task(); } catch (const std::exception& e) {
                    std::cerr << "[ThreadPool] Exception: " << e.what() << "\n";
                } catch (...) {
                    std::cerr << "[ThreadPool] Unknown exception.\n";
                }
            }
        });
    }
}
void ThreadPool::shutdown() {
    { std::unique_lock<std::mutex> lock(queue_mutex); if (stop) return; stop = true; }
    condition.notify_all();
    for (std::thread& worker : workers) if (worker.joinable()) worker.join();
}
ThreadPool::~ThreadPool() { shutdown(); }

// ----- UI ----------------------------------------------------------------
UI::UI(uint64_t sz, uint32_t blks, bool v) : total_size(sz), total_blocks(blks), verbose(v) { start_time = std::chrono::steady_clock::now(); }
void UI::log(const std::string& message) {
    if (!verbose) return;
    std::lock_guard<std::mutex> lock(Logger::log_mutex);
    std::cout << "[VERBOSE] " << message << "\n";
}
void UI::set_stats(uint32_t m, uint32_t f) {
    matches.store(m, std::memory_order_relaxed);
    fails.store(f, std::memory_order_relaxed);
}
std::string UI::format_time(double total_seconds) {
    int h = static_cast<int>(total_seconds) / 3600; int m = (static_cast<int>(total_seconds) % 3600) / 60; int s = static_cast<int>(total_seconds) % 60;
    std::ostringstream oss; oss << std::setfill('0') << std::setw(2) << h << ":" << std::setfill('0') << std::setw(2) << m << ":" << std::setfill('0') << std::setw(2) << s; return oss.str();
}
void UI::update(uint64_t current_pos, uint32_t current_block, const char* label, uint64_t out_size) {
    if (verbose) return;
    auto now = std::chrono::steady_clock::now();
    double elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - start_time).count() / 1000.0;
    double progress = total_size > 0 ? static_cast<double>(current_pos) / static_cast<double>(total_size) : 0.0;
    if (progress > 1.0) progress = 1.0;
    double eta = (progress > 0.0001) ? (elapsed / progress) - elapsed : 0;
    double mbps = (current_pos / 1024.0 / 1024.0) / (elapsed + 0.001);
    uint32_t m = matches.load(std::memory_order_relaxed);
    uint32_t f = fails.load(std::memory_order_relaxed);
    std::cout << "\r\033[K" << label << " [" << std::fixed << std::setprecision(2) << progress * 100.0 << "%] "
              << "Blk: " << current_block << (total_blocks > 0 ? "/" + std::to_string(total_blocks) : "")
              << " [E:" << m << " F:" << f << "]"
              << " | " << std::setprecision(2) << mbps << " MB/s"
              << " | Time: " << format_time(elapsed)
              << " | ETA: " << format_time(eta)
              << " | Size: " << std::fixed << std::setprecision(2) << (out_size / 1048576.0) << " MB" << std::flush;
}
double UI::get_elapsed() { return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start_time).count() / 1000.0; }

// ----- Helpers ----------------------------------------------------------
std::vector<int32_t> ParseMethods(const std::string& input) {
    std::vector<int32_t> ids; std::stringstream ss(input); std::string token;
    while (std::getline(ss, token, '+')) {
        size_t start = token.find_first_not_of(" \t\r\n");
        size_t end = token.find_last_not_of(" \t\r\n");
        if (start != std::string::npos) token = token.substr(start, end - start + 1);
        std::transform(token.begin(), token.end(), token.begin(), ::tolower);
        if (token == "kraken" || token == "8") ids.push_back(8);
        else if (token == "leviathan" || token == "13") ids.push_back(13);
        else if (token == "mermaid" || token == "9") ids.push_back(9);
        else if (token == "selkie" || token == "11") ids.push_back(11);
        else if (token == "hydra" || token == "12") ids.push_back(12);
    }
    if (ids.empty()) ids.push_back(8); return ids;
}
std::vector<int32_t> ParseLevels(const std::string& input) {
    std::vector<int32_t> levels; std::stringstream ss(input); std::string token;
    while (std::getline(ss, token, '+')) {
        size_t start = token.find_first_not_of(" \t\r\n");
        size_t end = token.find_last_not_of(" \t\r\n");
        if (start != std::string::npos) token = token.substr(start, end - start + 1);
        try { levels.push_back(std::stoi(token)); } catch (...) {}
    }
    return levels;
}

std::vector<uint8_t> ParseKey(const std::string& hex) {
    if (hex.length() != 64) {
        throw std::invalid_argument("AES key must be exactly 64 hex characters.");
    }
    std::vector<uint8_t> key(32);
    for (size_t i = 0; i < 64; i += 2) {
        key[i / 2] = static_cast<uint8_t>(std::stoul(hex.substr(i, 2), nullptr, 16));
    }
    return key;
}

void ResolveAESKey(std::vector<uint8_t>& aesKey, bool& useAES, const PreHeader& hdr) {
    if (hdr.use_aes && !useAES) { aesKey.assign(hdr.aes_key, hdr.aes_key + 32); useAES = true; }
}

uint32_t CalculateCRC32(const uint8_t* data, size_t len) {
    return static_cast<uint32_t>(crc32(crc32(0L, Z_NULL, 0), data, static_cast<uInt>(len)));
}

bool write_gap(ThreadSafeReader& reader, FastStreamWriter& writer, ObjectPool<char>& pool,
    uint64_t offset, uint64_t length) {
    if (length == 0) return true;
    auto buf_handle = pool.acquire();
    auto& buf = buf_handle.get();
    uint64_t remaining = length;
    uint64_t current_offset = offset;
    while (remaining > 0) {
        size_t to_write = std::min(static_cast<uint64_t>(buf.size()), remaining);
        size_t read = reader.pread(buf.data(), to_write, current_offset);
        if (read == 0) return false;
        writer.write(buf.data(), read);
        if (writer.has_error()) return false;
        current_offset += read;
        remaining -= read;
    }
    return true;
}
