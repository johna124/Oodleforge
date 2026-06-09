#include "aes.h"
#include "common.h"
#include <cstdlib>

namespace Logger {
    bool is_debug_enabled = false;
    std::ofstream debug_log;
}

void* AES_Context_Create(const uint8_t* key) {
    AES_ctx* ctx = (AES_ctx*)malloc(sizeof(AES_ctx));
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
    void* hModule = dlopen("./oo2core_9_win64.dll", RTLD_NOW);
    if (!hModule) hModule = dlopen("./liboo2core.so.9", RTLD_NOW);
    if (!hModule) hModule = dlopen("liboo2core.so.9", RTLD_NOW);
    if (!hModule) hModule = dlopen("liboo2core.so.8", RTLD_NOW);
    if (!hModule) return false;
    OodleLZ_Decompress = reinterpret_cast<OodleLZ_Decompress_t>(dlsym(hModule, "OodleLZ_Decompress"));
    OodleLZ_Compress   = reinterpret_cast<OodleLZ_Compress_t>(dlsym(hModule, "OodleLZ_Compress"));
#endif
    return (OodleLZ_Decompress != nullptr && OodleLZ_Compress != nullptr);
}

int64_t CompressAndVerify(int32_t method, int32_t level, const uint8_t* src, uint32_t usize,
                          const uint8_t* expected, uint32_t expected_size, std::vector<uint8_t>& temp_buf) {
    if (temp_buf.size() < usize * 2 + 4096) temp_buf.resize(usize * 2 + 4096);
    int64_t res = OodleLZ_Compress(method, src, usize, temp_buf.data(), level, nullptr, nullptr, nullptr, 0);
    if (res > 0 && res == static_cast<int64_t>(expected_size)) {
        if (std::memcmp(expected, temp_buf.data(), expected_size) == 0) return res;
    }
    return -1;
}

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

BlockScanner::BlockScanner(ThreadSafeReader& reader, uint64_t file_size, bool use_aes, const std::vector<uint8_t>& aes_key, size_t win_size)
    : reader_(reader), file_size_(file_size), win_size_(win_size), use_aes_(use_aes), aes_key_(aes_key) {
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
    while (pos < limit && pos < file_size_ - 16) {
        if (!ensure_window(pos, 16)) return false;
        size_t internal_idx = static_cast<size_t>(pos - win_start_);
        size_t max_check = std::min(win_len_ - internal_idx, static_cast<size_t>(file_size_ - 16 - pos));
        if (max_check == 0) { pos++; continue; }
        const uint8_t* search_start = use_aes_ ? &win_buf_dec_[internal_idx] : &win_buf_[internal_idx];
        const uint8_t* found_8c = static_cast<const uint8_t*>(std::memchr(search_start, MAGIC_8C, max_check));
        const uint8_t* found_cc = static_cast<const uint8_t*>(std::memchr(search_start, MAGIC_CC, max_check));
        size_t shift = std::min(found_8c ? static_cast<size_t>(found_8c - search_start) : max_check, found_cc ? static_cast<size_t>(found_cc - search_start) : max_check);
        if (shift < max_check) { pos += shift; return true; }
        pos += max_check;
    }
    return false;
}

std::shared_ptr<BlockTask> BlockScanner::extract_block(uint64_t pos, uint32_t usize, uint32_t csize, bool is_encrypted) {
    if (!ensure_window(pos, csize)) return nullptr;
    bool actual_encrypted = use_aes_ && is_encrypted && (win_buf_dec_[pos - win_start_] == MAGIC_8C || win_buf_dec_[pos - win_start_] == MAGIC_CC);
    uint8_t* active_buf = (use_aes_ && actual_encrypted) ? win_buf_dec_.get() : win_buf_.get();
    std::vector<uint8_t> dec_buf(usize);
    if (OodleLZ_Decompress(&active_buf[pos - win_start_], csize, dec_buf.data(), usize, 0, 0, 0, nullptr, 0, nullptr, nullptr, nullptr, 0, 0, 0) != static_cast<int64_t>(usize)) {
        return nullptr;
    }
    auto task = std::make_shared<BlockTask>();
    task->pos = pos; task->usize = usize; task->csize = csize; task->is_encrypted = actual_encrypted;
    task->raw_win_buf.assign(&active_buf[pos - win_start_], &active_buf[pos - win_start_] + csize);
    task->dec_data = std::move(dec_buf);
    return task;
}

static uint32_t FindCompressedSize(const uint8_t* src, size_t max_len, uint32_t usize, uint8_t* temp_dec_buf) {
    uint32_t low = 8, high = static_cast<uint32_t>(max_len), csize = 0;
    while (low <= high) {
        uint32_t mid = low + (high - low) / 2;
        if (OodleLZ_Decompress(src, mid, temp_dec_buf, usize, 0, 0, 0, nullptr, 0, nullptr, nullptr, nullptr, 0, 0, 0) == static_cast<int64_t>(usize)) {
            csize = mid; high = mid - 1;
        } else { low = mid + 1; }
    }
    return csize;
}

std::shared_ptr<BlockTask> BlockScanner::extract_next_block(uint64_t& pos, uint64_t limit, const std::vector<uint32_t>& test_sizes) {
    while (pos < limit && pos < file_size_ - 16) {
        if (!find_next_magic(pos, limit)) break;
        if (!ensure_window(pos, 512 * 1024)) break;
        size_t available = (win_start_ + win_len_ > pos) ? static_cast<size_t>(win_start_ + win_len_ - pos) : 0;
        size_t pass_size = std::min(available, static_cast<size_t>(file_size_ - pos));
        bool is_curr_enc = use_aes_ && (win_buf_dec_[pos - win_start_] == MAGIC_8C || win_buf_dec_[pos - win_start_] == MAGIC_CC);
        uint8_t* active_buf = (use_aes_ && is_curr_enc) ? win_buf_dec_.get() : win_buf_.get();
        std::vector<uint8_t> dec_buf(256 * 1024);
        uint32_t found_usize = 0, found_csize = 0;
        for (uint32_t usize : test_sizes) {
            if (OodleLZ_Decompress(&active_buf[pos - win_start_], pass_size, dec_buf.data(), usize, 0, 0, 0, nullptr, 0, nullptr, nullptr, nullptr, 0, 0, 0) == static_cast<int64_t>(usize)) {
                uint32_t max_needed = static_cast<uint32_t>(std::min(static_cast<uint64_t>(usize * 2), file_size_ - pos));
                ensure_window(pos, max_needed);
                active_buf = (use_aes_ && is_curr_enc) ? win_buf_dec_.get() : win_buf_.get();
                found_csize = FindCompressedSize(&active_buf[pos - win_start_], max_needed, usize, dec_buf.data());
                if (found_csize > 0 && found_csize < usize && found_csize >= 16) {
                    found_usize = usize; break;
                }
                found_csize = 0;
            }
        }
        if (found_csize > 0) {
            auto task = extract_block(pos, found_usize, found_csize, is_curr_enc);
            if (task) { pos += found_csize; return task; }
        }
        pos++;
    }
    return nullptr;
}

ThreadPool::ThreadPool(size_t threads) : stop(false) {
    for (size_t i = 0; i < threads; ++i) {
        workers.emplace_back([this] {
            for (;;) {
                std::function<void()> task;
                {
                    std::unique_lock<std::mutex> lock(this->queue_mutex);
                    this->condition.wait(lock, [this] { return this->stop || !this->tasks.empty(); });
                    if (this->stop && this->tasks.empty()) return;
                    task = std::move(this->tasks.front()); this->tasks.pop();
                }
                try { task(); } catch (const std::exception& e) { std::cerr << "[ThreadPool] Exception: " << e.what() << "\n"; } catch (...) { std::cerr << "[ThreadPool] Unknown exception.\n"; }
            }
        });
    }
}
void ThreadPool::shutdown() { { std::unique_lock<std::mutex> lock(queue_mutex); if (stop) return; stop = true; } condition.notify_all(); for (std::thread& worker : workers) if (worker.joinable()) worker.join(); }
ThreadPool::~ThreadPool() { shutdown(); }

UI::UI(uint64_t sz, uint32_t blks, bool v) : total_size(sz), total_blocks(blks), verbose(v) { start_time = std::chrono::steady_clock::now(); }
void UI::log(const std::string& message) { if (!verbose) return; std::lock_guard<std::mutex> lock(log_mutex); std::cout << "[VERBOSE] " << message << "\n"; }
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
              << " [E:" << matches << " F:" << fails << "]" << " | " << std::setprecision(2) << mbps << " MB/s"
              << " | Time: " << format_time(elapsed) << " | ETA: " << format_time(eta)
              << " | Size: " << std::fixed << std::setprecision(2) << (out_size / 1048576.0) << " MB" << std::flush;
}
double UI::get_elapsed() { return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start_time).count() / 1000.0; }

std::vector<int32_t> ParseMethods(const std::string& input) {
    std::vector<int32_t> ids; std::stringstream ss(input); std::string token;
    while (std::getline(ss, token, '+')) {
        std::transform(token.begin(), token.end(), token.begin(), ::tolower);
        if (token == "kraken" || token == "8") ids.push_back(8);
        else if (token == "leviathan" || token == "9") ids.push_back(9);
        else if (token == "mermaid" || token == "11") ids.push_back(11);
        else if (token == "selkie" || token == "12") ids.push_back(12);
    }
    if (ids.empty()) ids.push_back(8); return ids;
}
std::vector<int32_t> ParseLevels(const std::string& input) {
    std::vector<int32_t> levels; std::stringstream ss(input); std::string token;
    while (std::getline(ss, token, '+')) { try { levels.push_back(std::stoi(token)); } catch (...) {} } return levels;
}
std::vector<uint8_t> ParseKey(const std::string& hex) {
    std::vector<uint8_t> key(32, 0);
    if (hex.length() == 64) { for (size_t i = 0; i < 64; i += 2) key[i / 2] = static_cast<uint8_t>(std::stoul(hex.substr(i, 2), nullptr, 16)); } return key;
}
void ResolveAESKey(std::vector<uint8_t>& aesKey, bool& useAES, const PreHeader& hdr) {
    if (hdr.use_aes && !useAES) { aesKey.assign(hdr.aes_key, hdr.aes_key + 32); useAES = true; }
}

uint32_t CalculateCRC32(const uint8_t* data, size_t len) {
    static const uint32_t crc_table[256] = {
        0x00000000, 0x77073096, 0xee0e612c, 0x990951ba, 0x076dc419, 0x706af48f, 0xe963a535, 0x9e6495a3, 0x0edb8832, 0x79dcb8a4, 0xe0d5e91e, 0x97d2d988, 0x09b64c2b, 0x7eb17cbd, 0xe7b82d07, 0x90bf1d91,
        0x1db71064, 0x6ab020f2, 0xf3b97148, 0x84be41de, 0x1adad47d, 0x6ddde4eb, 0xf4d4b551, 0x83d385c7, 0x136c9856, 0x646ba8c0, 0xfd62f97a, 0x8a65c9ec, 0x14015c4f, 0x63066cd9, 0xfa0f3d63, 0x8d080df5,
        0x3b6e20c8, 0x4c69105e, 0xd56041e4, 0xa2677172, 0x3c03e4d1, 0x4b04d447, 0xd20d85fd, 0xa50ab56b, 0x35b5a8fa, 0x42b2986c, 0xdbbbc9d6, 0xacbcf940, 0x32d86ce3, 0x45df5c75, 0xdcd60dcf, 0xabd13d59,
        0x26d930ac, 0x51de003a, 0xc8d75180, 0xbfd06116, 0x21b4f4b5, 0x56b3c423, 0xcfba9599, 0xb8bda50f, 0x2802b89e, 0x5f058808, 0xc60cd9b2, 0xb10be924, 0x2f6f7c87, 0x58684c11, 0xc1611dab, 0xb6662d3d,
        0x76dc4190, 0x01db7106, 0x98d220bc, 0xefd5102a, 0x71b18589, 0x06b6b51f, 0x9fbfe4a5, 0xe8b8d433, 0x7807c9a2, 0x0f00f934, 0x9609a88e, 0xe10e9818, 0x7f6a0dbb, 0x086d3d2d, 0x91646c97, 0xe6635c01,
        0x6b6b51f4, 0x1c6c6162, 0x856530d8, 0xf262004e, 0x6c0695ed, 0x1b01a57b, 0x8208f4c1, 0xf50fc457, 0x65b0d9c6, 0x12b7e950, 0x8bbeb8ea, 0xfcb9887c, 0x62dd1ddf, 0x15da2d49, 0x8cd37cf3, 0xfbd44c65,
        0x4db26158, 0x3ab551ce, 0xa3bc0074, 0xd4bb30e2, 0x4adfa541, 0x3dd895d7, 0xa4d1c46d, 0xd3d6f4fb, 0x4369e96a, 0x346ed9fc, 0xad678846, 0xda60b8d0, 0x44042d73, 0x33031de5, 0xaa0a4c5f, 0xdd0d7cc9,
        0x5005713c, 0x270241aa, 0xbe0b1010, 0xc90c2086, 0x5768b525, 0x206f85b3, 0xb966d409, 0xce61e49f, 0x5edef90e, 0x29d9c998, 0xb0d09822, 0xc7d7a8b4, 0x59b33d17, 0x2eb40d81, 0xb7bd5c3b, 0xc0ba6cad,
        0xedb88320, 0x9abfb3b6, 0x03b6e20c, 0x74b1d29a, 0xead54739, 0x9dd277af, 0x04db2615, 0x73dc1683, 0xe3630b12, 0x94643b84, 0x0d6d6a3e, 0x7a6a5aa8, 0xe40ecf0b, 0x9309ff9d, 0x0a00ae27, 0x7d079eb1,
        0xf00f9344, 0x8708a3d2, 0x1e01f268, 0x6906c2fe, 0xf762575d, 0x806567cb, 0x196c3671, 0x6e6b06e7, 0xfed41b76, 0x89d32be0, 0x10da7a5a, 0x67dd4acc, 0xf9b9df6f, 0x8ebeeff9, 0x17b7be43, 0x60b08ed5,
        0xd6d6a3e8, 0xa1d1937e, 0x38d8c2c4, 0x4fdff252, 0xd1bb67f1, 0xa6bc5767, 0x3fb506dd, 0x48b2364b, 0xd80d2bda, 0xaf0a1b4c, 0x36034af6, 0x41047a60, 0xdf60efc3, 0xa867df55, 0x316e8eef, 0x4669be79,
        0xcb61b38c, 0xbc66831a, 0x256fd2a0, 0x5268e236, 0xcc0c7795, 0xbb0b4703, 0x220216b9, 0x5505262f, 0xc5ba3bbe, 0xb2bd0b28, 0x2bb45a92, 0x5cb36a04, 0xc2d7ffa7, 0xb5d0cf31, 0x2cd99e8b, 0x5bdeae1d,
        0x9b64c2b0, 0xec63f226, 0x756aa39c, 0x026d930a, 0x9c0906a9, 0xeb0e363f, 0x72076785, 0x05005713, 0x95bf4a82, 0xe2b87a14, 0x7bb12bae, 0x0cb61b38, 0x92d28e9b, 0xe5d5be0d, 0x7cdcefb7, 0x0bdbdf21,
        0x86d3d2d4, 0xf1d4e242, 0x68ddb3f8, 0x1fda836e, 0x81be16cd, 0xf6b9265b, 0x6fb077e1, 0x18b74777, 0x88085ae6, 0xff0f6a70, 0x66063bca, 0x11010b5c, 0x8f659eff, 0xf862ae69, 0x616bffd3, 0x166ccf45,
        0xa00ae278, 0xd70dd2ee, 0x4e048354, 0x3903b3c2, 0xa7672661, 0xd06016f7, 0x4969474d, 0x3e6e77db, 0xaed16a4a, 0xd9d65adc, 0x40df0b66, 0x37d83bf0, 0xa9bcae53, 0xdebb9ec5, 0x47b2cf7f, 0x30b5ffe9,
        0xbdbdf21c, 0xcabac28a, 0x53b39330, 0x24b4a3a6, 0xbad03605, 0xcdd70693, 0x54de5729, 0x23d967bf, 0xb3667a2e, 0xc4614ab8, 0x5d681b02, 0x2a6f2b94, 0xb40bbe37, 0xc30c8ea1, 0x5a05df1b, 0x2d02ef8d
    };
    uint32_t crc = 0xFFFFFFFF;
    for (size_t i = 0; i < len; i++) crc = crc_table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFF;
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
