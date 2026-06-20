#pragma once

#include <iostream>
#include <vector>
#include <string>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <queue>
#include <thread>
#include <future>
#include <functional>
#include <atomic>
#include <chrono>
#include <iomanip>
#include <sstream>
#include <algorithm>
#include <cstdint>
#include <fstream>
#include <set>
#include <map>
#include <cstring>
#include <cmath>
#include <stdexcept>
#include <filesystem>
#include <condition_variable>

#ifdef _WIN32
    #include <windows.h>
#else
    #include <unistd.h>
    #include <sys/stat.h>
    #include <fcntl.h>
    #include <dlfcn.h>
#endif

// ============================================================================
// Oodle Compression Options (Crucial for Frostbite & The Crew 2)
// ============================================================================
struct OodleLZ_CompressOptions {
    uint32_t verbosity;
    uint32_t spaceSpeedTradeoffBytes; // Required for Frostbite exact matches
    uint32_t unused;
    uint32_t sendQuantumCRCs;         // Required for The Crew 2
    uint32_t crcPolicy;
    uint32_t tryHarder;
    uint32_t reserved[10];            // Padding for DLL compatibility
};

// ============================================================================
// Global Configuration & Constants
// ============================================================================
namespace Config {
    constexpr size_t WIN_SIZE_LARGE = 128 * 1024 * 1024;
    constexpr size_t WIN_SIZE_SMALL = 64 * 1024 * 1024;
    constexpr size_t GAP_POOL_CHUNK = 16 * 1024 * 1024;

    constexpr uint32_t TEST_USIZES[] = {65536, 131072, 262144};
    constexpr int32_t ALL_METHODS[] = {8, 9, 11, 12, 13}; // Added Hydra (13)
    constexpr int32_t ALL_LEVELS[] = {1, 2, 3, 4, 5, 6, 7, 8, 9};
    constexpr size_t MAX_SAFE_COMPRESSED_SIZE = 100 * 1024 * 1024;

    // All 5 Oodle Magic Bytes
    constexpr uint8_t MAGIC_KRAKEN    = 0x8C;
    constexpr uint8_t MAGIC_LEVIATHAN = 0xCC;
    constexpr uint8_t MAGIC_MERMAID   = 0x4C;
    constexpr uint8_t MAGIC_SELKIE    = 0x2C;
    constexpr uint8_t MAGIC_HYDRA     = 0x6C;
}

// ============================================================================
// Oodle Definitions (Global Function Pointers)
// ============================================================================
// FIX: Added the '*' to make these actual pointer types, and fixed 'const void*'
typedef int64_t (*OodleLZ_Decompress_t)(
    const void* src, int64_t srcLen, void* dst, int64_t dstLen,
    int32_t fuzzSafe, int32_t checkCRC, int32_t verbosity,
    void* decBufBase, size_t decBufSize, void* fpCallback, void* callbackUserData,
    void* decoderMemory, size_t decoderMemorySize, int32_t threadPhase, int32_t unused
);

typedef int64_t (*OodleLZ_Compress_t)(
    int32_t codec, const void* src, int64_t srcLen, void* dst, int32_t level,
    void* opts, void* context, void* unused, size_t dictionarySize
);

extern OodleLZ_Decompress_t OodleLZ_Decompress;
extern OodleLZ_Compress_t   OodleLZ_Compress;
bool LoadOodle();

// Legacy aliases for backward compatibility with existing code
constexpr uint8_t MAGIC_8C = Config::MAGIC_KRAKEN;
constexpr uint8_t MAGIC_CC = Config::MAGIC_LEVIATHAN;

// ============================================================================
// Lightweight Debug Logger
// ============================================================================
namespace Logger {
    enum class Level { Info, Warn, Error, Debug };
    extern bool is_debug_enabled;
    extern std::ofstream debug_log;

    inline void Init(bool debug) {
        is_debug_enabled = debug;
        if (is_debug_enabled) debug_log.open("oodleforge_debug.log", std::ios::app);
    }

    inline void Log(Level lvl, const std::string& msg) {
        if (lvl == Level::Debug && !is_debug_enabled) return;
        std::string prefix = (lvl == Level::Debug) ? "[DEBUG] " : 
                             (lvl == Level::Warn)  ? "[WARN] " : 
                             (lvl == Level::Error) ? "[ERROR] " : "[INFO] ";
        if (is_debug_enabled && debug_log.is_open()) {
            debug_log << prefix << msg << "\n";
            debug_log.flush();
        } else if (lvl != Level::Debug) {
            std::cerr << prefix << msg << "\n";
        }
    }
}

// ============================================================================
// AES Wrapper & RAII
// ============================================================================
void* AES_Context_Create(const uint8_t* key);
void  AES_Context_Destroy(void* ctx);
void  AES_Context_Decrypt(void* ctx, uint8_t* buf, uint32_t len);
void  AES_Context_Encrypt(void* ctx, uint8_t* buf, uint32_t len);

struct AESContextDeleter {
    void operator()(void* ctx) const { if (ctx) AES_Context_Destroy(ctx); }
};
using AESContextPtr = std::unique_ptr<void, AESContextDeleter>;

// ============================================================================
// Enums & Error Handling
// ============================================================================
enum class ErrorCode {
    SUCCESS = 0, ERR_FILE_NOT_FOUND = 1, ERR_INVALID_MAGIC = 2, ERR_CRC_MISMATCH = 3,
    ERR_COMPRESSION_FAILED = 4, ERR_INVALID_ARGUMENT = 5, ERR_BUFFER_OVERFLOW = 6, ERR_UNKNOWN = 255
};

template <typename T>
class Result {
    T value;
    std::string error_msg;
    ErrorCode code;
    bool is_error;
public:
    Result(T val) : value(val), code(ErrorCode::SUCCESS), is_error(false) {}
    Result(ErrorCode c, const std::string& msg) : code(c), error_msg(msg), is_error(true) {}
    bool is_err() const { return is_error; }
    std::string get_error() const { return error_msg; }
    T get_value() const { return value; }
    ErrorCode get_code() const { return code; }
};

// ============================================================================
// Data Structures (Cross-platform alignment)
// ============================================================================
#pragma pack(push, 1)
struct PreHeader {
    uint32_t magic;
    uint32_t version;
    uint64_t original_size;
    uint32_t block_count;
    uint8_t use_aes;
    uint8_t aes_key[32];
};

struct BlockHeader {
    uint32_t stored_size;
};

struct PreBlock {
    uint64_t original_offset;
    uint32_t stored_size;
    uint32_t decompressed_size;
    uint32_t original_compressed_size;
    uint32_t compressor;
    uint8_t exact_match;
    uint8_t was_encrypted;
    uint8_t reserved;
    uint32_t crc32;
};
#pragma pack(pop)

struct BlockTask {
    uint64_t pos;
    uint32_t usize;
    uint32_t csize;
    std::vector<uint8_t> raw_win_buf;
    std::vector<uint8_t> dec_data;
    bool exact_match_found = false;
    int32_t matched_method = -1;
    int32_t matched_level = -1;
    bool is_encrypted = false;
};

struct DecTask {
    PreBlock b;
    uint64_t gap_len = 0;
    uint64_t gap_archive_offset = 0;
    uint64_t b_ptr_start = 0;
    uint64_t o_pos_start = 0;
    std::vector<uint8_t> block_data_in;
    std::vector<uint8_t> compressed_out;
    bool fatal_error = false;
    std::string error_msg;
};

struct ScanStats {
    uint32_t blocks_found = 0;
    uint32_t matches_identified = 0;
    uint32_t fails_unidentified = 0;
    std::map<int32_t, uint32_t> method_counts;
    std::map<int32_t, uint32_t> level_counts;
    
    void add_match(int32_t m, int32_t l) {
        matches_identified++;
        method_counts[m]++;
        level_counts[l]++;
    }
};

// ============================================================================
// Utility Classes
// ============================================================================
template <typename T>
class ObjectPool {
    size_t item_size;
    std::queue<std::shared_ptr<std::vector<T>>> available;
    std::mutex mtx;
public:
    ObjectPool(size_t count, size_t sz) : item_size(sz) {
        for(size_t i = 0; i < count; ++i) available.push(std::make_shared<std::vector<T>>(sz));
    }

    struct Handle {
        std::shared_ptr<std::vector<T>> ptr;
        ObjectPool* pool;

        Handle(std::shared_ptr<std::vector<T>> p, ObjectPool* pl) : ptr(p), pool(pl) {}
        ~Handle() { if(ptr && pool) pool->release(ptr); }
        
        Handle(Handle&& o) noexcept : ptr(o.ptr), pool(o.pool) { o.ptr = nullptr; o.pool = nullptr; }
        Handle& operator=(Handle&& o) noexcept { 
            ptr = o.ptr; pool = o.pool; o.ptr = nullptr; o.pool = nullptr; return *this; 
        }
        
        std::vector<T>& get() { return *ptr; }
    };

    Handle acquire() {
        std::unique_lock<std::mutex> lock(mtx);
        if(available.empty()) available.push(std::make_shared<std::vector<T>>(item_size));
        auto p = available.front(); available.pop();
        return Handle(p, this);
    }

    void release(std::shared_ptr<std::vector<T>> p) {
        std::unique_lock<std::mutex> lock(mtx);
        available.push(p);
    }
};

class ThreadSafeReader {
    struct Impl;
    std::unique_ptr<Impl> pImpl_;
public:
    ThreadSafeReader(const std::string& path);
    ~ThreadSafeReader();
    bool is_open() const;
    uint64_t get_size() const;
    size_t pread(char* dest, size_t count, uint64_t offset) const;
};

class FastStreamWriter {
    struct Impl {
        std::vector<uint8_t> active_buf;
        std::vector<uint8_t> flush_buf;
        std::vector<uint8_t> disk_buf;
        size_t buffer_size = 64 * 1024 * 1024;
        uint64_t total_flushed = 0;
        uint64_t pending_disk_bytes = 0; 
        
        std::ofstream file;
        std::thread flush_thread;
        
        mutable std::mutex mtx; 
        std::mutex file_io_mtx; 
        std::condition_variable cv;
        
        bool flush_ready = false;
        bool stop_flush = false;

        Impl() {
            active_buf.reserve(buffer_size);
            flush_buf.reserve(buffer_size);
            disk_buf.reserve(buffer_size);
        }

        ~Impl() { close(); }

        bool open(const std::string& path, size_t /*size*/ = 0) {
            file.open(path, std::ios::binary | std::ios::trunc);
            if (!file) return false;

            stop_flush = false;
            flush_ready = false;
            active_buf.clear();
            flush_buf.clear();
            disk_buf.clear();
            total_flushed = 0;
            pending_disk_bytes = 0;

            flush_thread = std::thread([this]() {
                while (true) {
                    std::unique_lock<std::mutex> lock(mtx);
                    cv.wait(lock, [this]() { return flush_ready || stop_flush; });

                    if (!flush_buf.empty()) {
                        std::swap(disk_buf, flush_buf);
                        pending_disk_bytes = disk_buf.size();
                        flush_ready = false;
                        cv.notify_all();
                        lock.unlock();

                        size_t chunk_size = disk_buf.size();
                        
                        {
                            std::lock_guard<std::mutex> io_lock(file_io_mtx);
                            file.write(reinterpret_cast<const char*>(disk_buf.data()), chunk_size);
                        }

                        disk_buf.clear();
                        
                        lock.lock();
                        total_flushed += chunk_size;
                        pending_disk_bytes = 0;
                        cv.notify_all(); 
                    } else if (stop_flush) {
                        break;
                    }
                }
            });

            return true;
        }

        void write(const char* data, size_t len) {
            std::unique_lock<std::mutex> lock(mtx);
            if (len > buffer_size) {
                cv.wait(lock, [this]() { return !flush_ready; });
                if (!active_buf.empty()) {
                    std::swap(flush_buf, active_buf);
                    flush_ready = true;
                    cv.notify_all();
                    cv.wait(lock, [this]() { return !flush_ready; });
                }
                lock.unlock();
                
                {
                    std::lock_guard<std::mutex> io_lock(file_io_mtx);
                    file.write(data, len);
                }
                
                lock.lock();
                total_flushed += len;
                return;
            }

            while (active_buf.size() + len > buffer_size) {
                cv.wait(lock, [this]() { return !flush_ready; });
                if (!active_buf.empty()) {
                    std::swap(flush_buf, active_buf);
                    flush_ready = true;
                    cv.notify_all();
                }
            }

            active_buf.insert(active_buf.end(), data, data + len);
        }

        void flush() {
            std::unique_lock<std::mutex> lock(mtx);
            cv.wait(lock, [this]() { return !flush_ready; });

            if (!active_buf.empty()) {
                std::swap(flush_buf, active_buf);
                flush_ready = true;
                cv.notify_all();
            }

            cv.wait(lock, [this]() { return !flush_ready; });
            
            {
                std::lock_guard<std::mutex> io_lock(file_io_mtx);
                file.flush();
            }
        }

        void close() {
            flush();
            {
                std::unique_lock<std::mutex> lock(mtx);
                if (!stop_flush) {
                    stop_flush = true;
                    cv.notify_all();
                }
            }
            if (flush_thread.joinable()) flush_thread.join();
            if (file.is_open()) file.close();
        }

        uint64_t tellp() const {
            std::unique_lock<std::mutex> lock(mtx);
            return total_flushed + active_buf.size() + flush_buf.size() + pending_disk_bytes;
        }

        void seekp(uint64_t pos) {
            std::unique_lock<std::mutex> lock(mtx);
            cv.wait(lock, [this]() { return !flush_ready; });
            if (!active_buf.empty()) {
                std::swap(flush_buf, active_buf);
                flush_ready = true;
                cv.notify_all();
                cv.wait(lock, [this]() { return !flush_ready; });
            }
            
            cv.wait(lock, [this]() { return pending_disk_bytes == 0; });
            
            {
                std::lock_guard<std::mutex> io_lock(file_io_mtx);
                file.clear();
                file.seekp(static_cast<std::streamoff>(pos));
            }
        }
    };

    std::unique_ptr<Impl> pImpl_;
public:
    FastStreamWriter() : pImpl_(std::make_unique<Impl>()) {}
    ~FastStreamWriter() { pImpl_->close(); }
    bool open(const std::string& path, size_t size = 0) { return pImpl_->open(path, size); }
    void write(const char* data, size_t len) { pImpl_->write(data, len); }
    void flush() { pImpl_->flush(); }
    uint64_t tellp() const { return pImpl_->tellp(); }
    void seekp(uint64_t pos) { pImpl_->seekp(pos); }

    size_t get_buffer_size() const { return pImpl_->buffer_size; }
    size_t get_write_pos() const { return pImpl_->active_buf.size(); }
};

class BlockScanner {
    ThreadSafeReader& reader_;
    uint64_t file_size_;
    size_t win_size_;
    bool use_aes_;
    std::vector<uint8_t> aes_key_;
    std::unique_ptr<uint8_t[]> win_buf_;
    std::unique_ptr<uint8_t[]> win_buf_dec_;
    AESContextPtr aes_ctx_;
    uint64_t win_start_ = 0;
    size_t win_len_ = 0;
    
    bool ensure_window(uint64_t pos, size_t needed);
    bool find_next_magic(uint64_t& pos, uint64_t limit);
    std::shared_ptr<BlockTask> extract_block(uint64_t pos, uint32_t usize, uint32_t csize, bool is_encrypted);

public:
    BlockScanner(ThreadSafeReader& reader, uint64_t file_size, bool use_aes, const std::vector<uint8_t>& aes_key, size_t win_size);
    ~BlockScanner();
    std::shared_ptr<BlockTask> extract_next_block(uint64_t& pos, uint64_t limit, const std::vector<uint32_t>& test_sizes);
};

class ThreadPool {
    std::vector<std::thread> workers;
    std::queue<std::function<void()>> tasks;
    std::mutex queue_mutex;
    std::condition_variable condition;
    bool stop;
    void shutdown();

public:
    ThreadPool(size_t threads);
    ~ThreadPool();

    template <class F>
    std::future<typename std::invoke_result<F>::type> enqueue(F&& f) {
        using return_type = typename std::invoke_result<F>::type;
        auto task_ptr = std::make_shared<std::packaged_task<return_type()>>(std::forward<F>(f));
        std::future<return_type> res = task_ptr->get_future();
        {
            std::unique_lock<std::mutex> lock(queue_mutex);
            if(stop) throw std::runtime_error("enqueue on stopped ThreadPool");
            tasks.emplace([task_ptr]() { (*task_ptr)(); });
        }
        condition.notify_one();
        return res;
    }
};

class UI {
    uint64_t total_size;
    uint32_t total_blocks;
    bool verbose;
    std::chrono::steady_clock::time_point start_time;
    std::mutex log_mutex;
    std::atomic<uint32_t> matches{0};
    std::atomic<uint32_t> fails{0};

public:
    UI(uint64_t sz, uint32_t blks, bool v);
    void log(const std::string& message);
    void set_stats(uint32_t m, uint32_t f);
    std::string format_time(double total_seconds);
    void update(uint64_t current_pos, uint32_t current_block, const char* label, uint64_t out_size);
    double get_elapsed();
};

// ============================================================================
// Global Function Declarations
// ============================================================================
uint32_t CalculateCRC32(const uint8_t* data, size_t len);
void write_gap(ThreadSafeReader& reader, FastStreamWriter& writer, ObjectPool<char>& pool, uint64_t offset, uint64_t length);
std::vector<int32_t> ParseMethods(const std::string& input);
std::vector<int32_t> ParseLevels(const std::string& input);
std::vector<uint8_t> ParseKey(const std::string& hex);
void ResolveAESKey(std::vector<uint8_t>& aesKey, bool& useAES, const PreHeader& hdr);

// FIX: Added 'opts' parameter to CompressAndVerify
int64_t CompressAndVerify(int32_t method, int32_t level, const uint8_t* src, uint32_t usize, 
                          const uint8_t* expected, uint32_t expected_size, std::vector<uint8_t>& temp_buf, 
                          const OodleLZ_CompressOptions* opts = nullptr);

// FIX: Added 'tradeoff_bytes' and 'quantum_crc' to match main.cpp
Result<int> RunScan(const std::string& input_path, bool verbose, int num_threads, const std::vector<int32_t>& m_ids, 
                    const std::vector<uint8_t>& aesKey, bool useAES, double scan_percent, bool debug_mode,
                    uint32_t tradeoff_bytes, bool quantum_crc);

Result<int> RunEncode(const std::string& input_path, const std::string& output_path, bool verbose, int num_threads, 
                      const std::vector<int32_t>& m_ids, const std::vector<int32_t>& opt_levels, bool opt_auto, bool opt_force, 
                      const std::vector<uint8_t>& aesKey, bool useAES, uint32_t tradeoff_bytes, bool quantum_crc);

Result<int> RunReconstruct(const std::string& input_path, const std::string& output_path, bool verbose, int num_threads, 
                           std::vector<uint8_t>& aesKey, bool& useAES, uint32_t tradeoff_bytes, bool quantum_crc);
