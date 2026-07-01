#include "aes.h"
#include "common.h"
#include <cstdlib>
#include <zlib.h>

namespace Logger {
    bool is_debug_enabled = false;
    std::ofstream debug_log;
    std::mutex log_mutex; // Instantiation of the log mutex
}

void* AES_Context_Create(const uint8_t* key) {
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

OodleLZ_Decompress_t OodleLZ_Decompress = nullptr;
OodleLZ_Compress_t   OodleLZ_Compress   = nullptr;

bool LoadOodle() {
#ifdef _WIN32
    HMODULE hModule = LoadLibraryA("oo2core_9_win64.dll");
    if (!hModule) hModule = LoadLibraryA("oo2core_8_win64.dll");
    if (!hModule) return false;
    OodleLZ_Decompress = reinterpret_cast<OodleLZ_Decompress_t>(GetProcAddress(hModule, "OodleLZ_Decompress"));
    OodleLZ_Compress   = reinterpret_cast<OodleLZ_Compress_t>(GetProcAddress(hModule, "OodleLZ_Compress"));
#else
    void* hModule = dlopen("./liboo2core.so.9", RTLD_NOW);
    if (!hModule) hModule = dlopen("liboo2core.so.9", RTLD_NOW);
    if (!hModule) hModule = dlopen("liboo2core.so.8", RTLD_NOW);
    if (!hModule) return false;
    OodleLZ_Decompress = reinterpret_cast<OodleLZ_Decompress_t>(dlsym(hModule, "OodleLZ_Decompress"));
    OodleLZ_Compress   = reinterpret_cast<OodleLZ_Compress_t>(dlsym(hModule, "OodleLZ_Compress"));
#endif
    return (OodleLZ_Decompress != nullptr && OodleLZ_Compress != nullptr);
}

int64_t CompressAndVerify(int32_t method, int32_t level, const uint8_t* src, uint32_t usize,
                          const uint8_t* expected, uint32_t expected_size, std::vector<uint8_t>& temp_buf,
                          const OodleLZ_CompressOptions* opts) 
{
    if (temp_buf.size() < usize * 2 + 4096) temp_buf.resize(usize * 2 + 4096);
    int64_t res = OodleLZ_Compress(method, src, usize, temp_buf.data(), level, (void*)opts, nullptr, nullptr, nullptr, 0);
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
    size_t required_size = std::max(static_cast<size_t>(task->usize) * 2, static_cast<size_t>(256 * 1024));
    std::vector<uint8_t> local_comp_buf(required_size);
    
    {
        std::shared_lock<std::shared_mutex> lock(cache_mutex);
        for (const auto& pair : cache) {
            if (CompressAndVerify(pair.first, pair.second, task->dec_data.data(), task->usize, task->raw_win_buf.data(), task->csize, local_comp_buf, opts) > 0) {
                out_method = pair.first;
                out_level = pair.second;
                return true;
            }
        }
    }
    
    for (int32_t method : all_methods) {
        for (int32_t level : all_levels) {
            if (CompressAndVerify(method, level, task->dec_data.data(), task->usize, task->raw_win_buf.data(), task->csize, local_comp_buf, opts) > 0) {
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

// ---------- GetOodleBlockSize  ----------
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
        uint32_t csize = ((hdr[2] << 16) | (hdr[3] << 8) | hdr[4]) + header_size;
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
        while (total_read < count) {
#ifdef _WIN32
            OVERLAPPED ol = {0}; ol.Offset = static_cast<DWORD>(offset + total_read); ol.OffsetHigh = static_cast<DWORD>((offset + total_read) >> 32);
            DWORD bytes_read = 0;
            BOOL success = ReadFile(hFile, dest + total_read, static_cast<DWORD>(count - total_read), &bytes_read, &ol);
            if (!success) {
                DWORD err = GetLastError();
                if (err == ERROR_HANDLE_EOF) break;
                if (err == ERROR_LOCK_VIOLATION || err == ERROR_SEM_TIMEOUT) { Sleep(1); continue; }
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

// ----- FastStreamWriter (Stable with Error Checking) ----------------------
struct FastStreamWriter::Impl {
    std::vector<uint8_t> active_buf, flush_buf, disk_buf;
    // [FIX] Buffer size reduced to 32MB to prevent RAM spikes
    size_t buffer_size = 32 * 1024 * 1024;
    uint64_t total_flushed = 0; uint64_t pending_disk_bytes = 0;
    std::ofstream file; std::thread flush_thread;
    mutable std::mutex mtx; std::mutex file_io_mtx; std::condition_variable cv;
    bool flush_ready = false; bool stop_flush = false;
    
    // [FIX] Atomic flag to catch I/O errors from the background thread safely
    std::atomic<bool> io_error{false}; 

    Impl() { active_buf.reserve(buffer_size); flush_buf.reserve(buffer_size); disk_buf.reserve(buffer_size); }
    ~Impl() { close(); }

    // [FIX] Check wrapper to instantly crash out if the file writer died
    void check_io() const {
        if (io_error) throw std::runtime_error("Background I/O write failed (e.g. disk full).");
    }

    bool open(const std::string& path, size_t = 0) {
        file.open(path, std::ios::binary | std::ios::trunc);
        if (!file) return false;
        stop_flush = false; flush_ready = false; io_error = false;
        active_buf.clear(); flush_buf.clear(); disk_buf.clear();
        total_flushed = 0; pending_disk_bytes = 0;
        
        flush_thread = std::thread([this]() {
            while (true) {
                std::unique_lock<std::mutex> lock(mtx);
                cv.wait(lock, [this]() { return flush_ready || stop_flush; });
                if (!flush_buf.empty()) {
                    std::swap(disk_buf, flush_buf);
                    pending_disk_bytes = disk_buf.size();
                    flush_ready = false; cv.notify_all(); lock.unlock();
                    size_t chunk_size = disk_buf.size();
                    { 
                        std::lock_guard<std::mutex> io_lock(file_io_mtx); 
                        if (!file.write(reinterpret_cast<const char*>(disk_buf.data()), chunk_size)) {
                            io_error = true; // [FIX] Signal the main thread on failure
                        }
                    }
                    disk_buf.clear(); lock.lock();
                    total_flushed += chunk_size; pending_disk_bytes = 0; cv.notify_all();
                } else if (stop_flush) { break; }
            }
        });
        return true;
    }

    void write(const char* data, size_t len) {
        check_io();
        std::unique_lock<std::mutex> lock(mtx);
        if (len > buffer_size) {
            cv.wait(lock, [this]() { return !flush_ready; });
            if (!active_buf.empty()) { std::swap(flush_buf, active_buf); flush_ready = true; cv.notify_all(); }
            
            // [FIX] Ordering race: wait for actual disk completion before raw write bypass
            cv.wait(lock, [this]() { return pending_disk_bytes == 0; });
            check_io();
            lock.unlock();
            
            { 
                std::lock_guard<std::mutex> io_lock(file_io_mtx); 
                if(!file.write(data, len)) io_error = true; 
            }
            lock.lock(); total_flushed += len; return;
        }
        while (active_buf.size() + len > buffer_size) {
            cv.wait(lock, [this]() { return !flush_ready; });
            check_io();
            if (!active_buf.empty()) { std::swap(flush_buf, active_buf); flush_ready = true; cv.notify_all(); }
        }
        active_buf.insert(active_buf.end(), data, data + len);
    }

    void flush() {
        check_io();
        std::unique_lock<std::mutex> lock(mtx);
        cv.wait(lock, [this]() { return !flush_ready; });
        if (!active_buf.empty()) { std::swap(flush_buf, active_buf); flush_ready = true; cv.notify_all(); }
        
        // [FIX] Guarantee disk completion on forced flush
        cv.wait(lock, [this]() { return !flush_ready && pending_disk_bytes == 0; });
        check_io();
        { 
            std::lock_guard<std::mutex> io_lock(file_io_mtx); 
            if(!file.flush()) io_error = true; 
        }
    }

    void close() {
        flush();
        { std::unique_lock<std::mutex> lock(mtx); if (!stop_flush) { stop_flush = true; cv.notify_all(); } }
        if (flush_thread.joinable()) flush_thread.join();
        if (file.is_open()) file.close();
    }

    uint64_t tellp() const {
        std::unique_lock<std::mutex> lock(mtx);
        return total_flushed + active_buf.size() + flush_buf.size() + pending_disk_bytes;
    }

    void seekp(uint64_t pos) {
        check_io();
        std::unique_lock<std::mutex> lock(mtx);
        cv.wait(lock, [this]() { return !flush_ready; });
        if (!active_buf.empty()) { std::swap(flush_buf, active_buf); flush_ready = true; cv.notify_all(); }
        cv.wait(lock, [this]() { return !flush_ready && pending_disk_bytes == 0; });
        check_io();
        { std::lock_guard<std::mutex> io_lock(file_io_mtx); file.clear(); file.seekp(static_cast<std::streamoff>(pos)); }
    }
};

FastStreamWriter::FastStreamWriter() : pImpl_(std::make_unique<Impl>()) {}
FastStreamWriter::~FastStreamWriter() { pImpl_->close(); }
bool FastStreamWriter::open(const std::string& path, size_t size) { return pImpl_->open(path, size); }
void FastStreamWriter::write(const char* data, size_t len) { pImpl_->write(data, len); }
void FastStreamWriter::flush() { pImpl_->flush(); }
void FastStreamWriter::close() { pImpl_->close(); }
uint64_t FastStreamWriter::tellp() const { return pImpl_->tellp(); }
void FastStreamWriter::seekp(uint64_t pos) { pImpl_->seekp(pos); }

// ----- BlockScanner ------------------------------------------------------
BlockScanner::BlockScanner(ThreadSafeReader& reader, uint64_t file_size, bool use_aes, const std::vector<uint8_t>& aes_key, size_t win_size)
: reader_(reader), file_size_(file_size), win_size_(win_size), use_aes_(use_aes), aes_key_(aes_key) {
    win_buf_.reset(new uint8_t[win_size]);
    if (use_aes_) { win_buf_dec_.reset(new uint8_t[win_size]); aes_ctx_ = AESContextPtr(AES_Context_Create(aes_key_.data())); }
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
                size_t aes_len = (read_bytes + 15) & ~15;
                std::memcpy(&win_buf_dec_[keep], &win_buf_[keep], read_bytes);
                AES_Context_Decrypt(aes_ctx_.get(), &win_buf_dec_[keep], static_cast<uint32_t>(aes_len));
            }
            win_len_ = keep + read_bytes;
        } else {
            win_start_ = aligned_pos;
            size_t read_bytes = reader_.pread(reinterpret_cast<char*>(win_buf_.get()), win_size_, win_start_);
            if (use_aes_ && read_bytes > 0) {
                size_t aes_len = (read_bytes + 15) & ~15;
                std::memcpy(win_buf_dec_.get(), win_buf_.get(), read_bytes);
                AES_Context_Decrypt(aes_ctx_.get(), win_buf_dec_.get(), static_cast<uint32_t>(aes_len));
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
        size_t min_shift = max_check;
        auto check_magic = [&](uint8_t magic) {
            const uint8_t* f = static_cast<const uint8_t*>(std::memchr(search_start, magic, max_check));
            if (f) { size_t shift = static_cast<size_t>(f - search_start); if (shift < min_shift) min_shift = shift; }
        };
        check_magic(0x8C); check_magic(0xCC); check_magic(0x0C); check_magic(0x4C);
        if (min_shift < max_check) { pos += min_shift; return true; }
        pos += max_check;
    }
    return false;
}

static uint32_t FindCompressedSize(const uint8_t* src, size_t max_len, uint32_t usize, uint8_t* temp_dec_buf) {
    uint32_t low = 8;   
    uint32_t high = static_cast<uint32_t>(max_len);
    uint32_t csize = 0;
    while (low <= high) {
        uint32_t mid = low + (high - low) / 2;
        if (OodleLZ_Decompress(src, mid, temp_dec_buf, usize, 0, 0, 0, nullptr, 0, nullptr, nullptr, nullptr, 0, 0, 0) == static_cast<int64_t>(usize)) {
            csize = mid;
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

// ============================================================================
// WalkOodleChain 
// ============================================================================
uint64_t BlockScanner::WalkOodleChain(uint64_t start_pos, uint8_t& codec_out, bool& is_valid) {
    uint64_t offset = start_pos;
    uint64_t c_size_total = 0;
    bool first = true;
    is_valid = false;
    codec_out = 0;
    uint32_t iteration = 0;   // safety counter

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
            bool compressed_hdr = (b0 == 0x8C);
            uint8_t b1 = hdr[1];
            uint8_t base_b1 = b1 & 0x7F;
            if (compressed_hdr) {
                if (base_b1 != 0x06 && base_b1 != 0x0A && base_b1 != 0x0C && base_b1 != 0x0D && base_b1 != 0x0B) {
                    diagnostics_.walk_first_consistency_fail++; return 0;
                }
            } else {
                if (base_b1 != 0x06 && base_b1 != 0x0A && base_b1 != 0x0C && base_b1 != 0x0D && base_b1 != 0x0B) {
                    diagnostics_.walk_first_consistency_fail++; return 0;
                }
            }
            if (base_b1 == 0x06) codec_out = 8;
            else if (base_b1 == 0x0A) codec_out = 9;
            else if (base_b1 == 0x0C) codec_out = 11;
            else if (base_b1 == 0x0D) codec_out = 12;
            else if (base_b1 == 0x0B) codec_out = 13;
            else { diagnostics_.walk_first_consistency_fail++; return 0; }
        } else {
            if (rem_size < 10) break;
            uint8_t b0 = hdr[0];
            
            // [FIX] Missed block bug resolved: only accept continuations here. 
            if (b0 != 0x0C && b0 != 0x4C) break; 
            
            uint8_t b1 = hdr[1];
            uint8_t base_b1 = b1 & 0x7F;
            if (base_b1 != 0x06 && base_b1 != 0x0A && base_b1 != 0x0C && base_b1 != 0x0D && base_b1 != 0x0B) break;
        }

        bool compressed_seg = (hdr[0] == 0x8C || (!first && hdr[0] == 0x0C));
        uint8_t b1 = hdr[1];
        uint8_t base_b1 = b1 & 0x7F;
        if (compressed_seg) {
            uint32_t i = 0;
            bool is_long = (b1 & 0x80) != 0;
            uint32_t header_size = is_long ? 9 : 6;
            if (base_b1 == 0x06 || base_b1 == 0x0A || base_b1 == 0x0C || base_b1 == 0x0D || base_b1 == 0x0B) {
                i = ReadBE24(hdr + 2) + header_size;
            } else break;

            if (i == 0) break; // Prevent zero-size loops

            if (first && i < Config::MIN_VALID_FIRST_SEGMENT) { diagnostics_.walk_first_too_small++; return 0; }
            if (c_size_total + i > file_size_ - start_pos) { diagnostics_.walk_bounds_fail++; return 0; }
            if (i == 0x00080005) i = 6;
            c_size_total += i;
            offset += i;
            first = false;
        } else {
            if (base_b1 == 0x06 || base_b1 == 0x0A || base_b1 == 0x0C || base_b1 == 0x0D || base_b1 == 0x0B) {
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


// ---------- extract_next_block ----------
std::shared_ptr<BlockTask> BlockScanner::extract_next_block(uint64_t& pos, uint64_t limit, const std::vector<uint32_t>& test_sizes) {
    std::vector<uint8_t> dec_buf;
    uint32_t max_usize = 4 * 1024 * 1024;
    dec_buf.resize(max_usize);
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
        uint8_t base_b1 = b1 & 0x7F;
        if (base_b1 != 0x06 && base_b1 != 0x0A && base_b1 != 0x0C && base_b1 != 0x0D && base_b1 != 0x0B) {
            diagnostics_.rejected_by_fast_rejection++;
            pos++;
            continue;
        }
        diagnostics_.passed_fast_rejection++;
        uint8_t codec = 0;
        uint32_t csize = GetOodleBlockSize(hdr, pass_size, codec);
    
        if (csize > 0 && csize <= pass_size) {
            // [FIX] Ensure we pass to WalkOodleChain if mult-quantum continuation follows
            bool continuation_follows = false;
            if (csize + 2 <= pass_size) {
                uint8_t next0 = hdr[csize];
                if (next0 == 0x0C || next0 == 0x4C) {
                    uint8_t next_base_b1 = hdr[csize + 1] & 0x7F;
                    if (next_base_b1 == 0x06 || next_base_b1 == 0x0A || next_base_b1 == 0x0C ||
                        next_base_b1 == 0x0D || next_base_b1 == 0x0B) {
                        continuation_follows = true;
                    }
                }
            }

            if (!continuation_follows) {
                int64_t usize = OodleLZ_Decompress(hdr, csize, dec_buf.data(), max_usize, 0, 0, 0, nullptr, 0, nullptr, nullptr, nullptr, 0, 0, 0);
                if (usize > 0 && usize <= max_usize) {
                    auto task = std::make_shared<BlockTask>();
                    task->pos = pos;
                    task->usize = static_cast<uint32_t>(usize);
                    task->csize = csize;
                    task->matched_method = codec;
                    task->is_encrypted = use_aes_ && (win_buf_[pos - win_start_] != active_buf[pos - win_start_]);
                    task->raw_win_buf.assign(hdr, hdr + csize);
                    dec_buf.resize(usize);
                    task->dec_data = std::move(dec_buf);
                    dec_buf.resize(max_usize);
                    pos += csize;
                    diagnostics_.blocks_validated++;
                    return task;
                }
            }
        }
        uint8_t walk_codec = 0;
        bool walk_valid = false;
        uint64_t walk_csize = WalkOodleChain(pos, walk_codec, walk_valid);

        if (walk_valid && walk_csize >= Config::MIN_OODLE_BLOCK_SIZE && walk_csize <= pass_size) {
            size_t probe_cap = std::max(static_cast<size_t>(max_usize), static_cast<size_t>(walk_csize) * 8);
            if (probe_cap > Config::MAX_SAFE_COMPRESSED_SIZE) probe_cap = Config::MAX_SAFE_COMPRESSED_SIZE;
            if (chain_probe_buf_.size() < probe_cap) chain_probe_buf_.resize(probe_cap);
            int64_t dec_size = OodleLZ_Decompress(hdr, static_cast<int64_t>(walk_csize), chain_probe_buf_.data(), chain_probe_buf_.size(), 0, 0, 0, nullptr, 0, nullptr, nullptr, nullptr, 0, 0, 0);
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
            task->matched_method = (base_b1 == 0x06) ? 8 :
                                   (base_b1 == 0x0A) ? 9 :
                                   (base_b1 == 0x0C) ? 11 :
                                   (base_b1 == 0x0D) ? 12 : 13;
            task->is_encrypted = use_aes_ && (win_buf_[pos - win_start_] != active_buf[pos - win_start_]);
            task->raw_win_buf.assign(hdr, hdr + found_csize);
            std::vector<uint8_t> final_dec(found_usize);
            OodleLZ_Decompress(hdr, found_csize, final_dec.data(), found_usize, 0, 0, 0, nullptr, 0, nullptr, nullptr, nullptr, 0, 0, 0);
            task->dec_data = std::move(final_dec);
            pos += found_csize;
            return task;
        }
        pos++;
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
                try { 
                    task(); 
                } catch (const std::exception& e) { 
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
    std::lock_guard<std::mutex> lock(log_mutex); 
    std::cout << "[VERBOSE] " << message << "\n"; 
}
void UI::set_stats(uint32_t m, uint32_t f) { matches = m; fails = f; }
std::string UI::format_time(double total_seconds) {
    int h = static_cast<int>(total_seconds) / 3600; int m = (static_cast<int>(total_seconds) % 3600) / 60; int s = static_cast<int>(total_seconds) % 60;
    std::ostringstream oss; oss << std::setfill('0') << std::setw(2) << h << ":" << std::setfill('0') << std::setw(2) << m << ":" << std::setfill('0') << std::setw(2) << s; return oss.str();
}
void UI::update(uint64_t current_pos, uint32_t current_block, const char* label, uint64_t out_size) {
    if (verbose) return; auto now = std::chrono::steady_clock::now(); double elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - start_time).count() / 1000.0;
    double progress = total_size > 0 ? static_cast<double>(current_pos) / static_cast<double>(total_size) : 0.0; if (progress > 1.0) progress = 1.0;
    double eta = (progress > 0.0001) ? (elapsed / progress) - elapsed : 0; double mbps = (current_pos / 1024.0 / 1024.0) / (elapsed + 0.001);
    std::cout << "\r\033[K" << label << " [" << std::fixed << std::setprecision(2) << progress * 100.0 << "%] "
              << "Blk: " << current_block << (total_blocks > 0 ? "/" + std::to_string(total_blocks) : "")
              << " [E:" << matches << " F:" << fails << "]"
              << " | " << std::setprecision(2) << mbps << " MB/s"
              << " | Time: " << format_time(elapsed)
              << " | ETA: " << format_time(eta)
              << " | Size: " << std::fixed << std::setprecision(2) << (out_size / 1048576.0) << " MB" << std::flush;
}
double UI::get_elapsed() { return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start_time).count() / 1000.0; }

// ----- Helper functions --------------------------------------------------
std::vector<int32_t> ParseMethods(const std::string& input) {
    std::vector<int32_t> ids; std::stringstream ss(input); std::string token;
    while (std::getline(ss, token, '+')) {
        size_t start = token.find_first_not_of(" \t\r\n");
        size_t end = token.find_last_not_of(" \t\r\n");
        if (start != std::string::npos) token = token.substr(start, end - start + 1);
        std::transform(token.begin(), token.end(), token.begin(), ::tolower);
        // [FIX] Correctly assigned Hydra to 12, Leviathan to 13
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
    std::vector<uint8_t> key(32, 0);
    if (hex.length() == 64) { for (size_t i = 0; i < 64; i += 2) key[i / 2] = static_cast<uint8_t>(std::stoul(hex.substr(i, 2), nullptr, 16)); } return key;
}
void ResolveAESKey(std::vector<uint8_t>& aesKey, bool& useAES, const PreHeader& hdr) {
    if (hdr.use_aes && !useAES) { aesKey.assign(hdr.aes_key, hdr.aes_key + 32); useAES = true; }
}
uint32_t CalculateCRC32(const uint8_t* data, size_t len) {
    return static_cast<uint32_t>(crc32(crc32(0L, Z_NULL, 0), data, static_cast<uInt>(len)));
}
void write_gap(ThreadSafeReader& reader, FastStreamWriter& writer, ObjectPool<char>& pool, uint64_t offset, uint64_t length) {
    if (length == 0) return;
    auto buf_handle = pool.acquire(); auto& buf = buf_handle.get();
    uint64_t remaining = length; uint64_t current_offset = offset;
    while (remaining > 0) {
        size_t to_write = std::min(static_cast<uint64_t>(buf.size()), remaining);
        size_t read = reader.pread(buf.data(), to_write, current_offset);
        if (read == 0) break;
        writer.write(buf.data(), read); current_offset += read; remaining -= read;
    }
}

