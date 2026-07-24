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
#include <deque>
#include <array>
#include <type_traits>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <unistd.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <dlfcn.h>
#endif

struct OodleLZ_CompressOptions {
    uint32_t version;
    uint32_t verbosity;
    uint32_t spaceSpeedTradeoffBytes;
    uint32_t unused;
    uint32_t sendQuantumCRCs;
    uint32_t crcPolicy;
    uint32_t tryHarder;
    uint32_t tryDictionary;
    float farMatchMinEfficacy;
    uint32_t reserved[4];
};

namespace Config {
    constexpr size_t WIN_SIZE_LARGE = 64 * 1024 * 1024;
    constexpr size_t WIN_SIZE_SMALL = 32 * 1024 * 1024;
    constexpr size_t GAP_POOL_CHUNK = 8 * 1024 * 1024;
    constexpr uint32_t TEST_USIZES[] = {
        1048576, 655360, 524288, 393216, 327680, 262144, 196608, 131072,
        98304, 65536, 49152, 32768, 24576, 16384, 12288, 8192,
        4096, 2048
    };
    constexpr int32_t ALL_METHODS[] = {8, 9, 11, 12, 13};
    constexpr int32_t ALL_LEVELS[] = {1, 2, 3, 4, 5, 6, 7, 8, 9};
    constexpr size_t MAX_SAFE_COMPRESSED_SIZE = 100 * 1024 * 1024;
    constexpr size_t MAX_DECOMP_BUF_SIZE = 256 * 1024 * 1024;
    constexpr uint8_t MAGIC_COMPRESSED_FIRST   = 0x8C;
    constexpr uint8_t MAGIC_UNCOMPRESSED_FIRST = 0xCC;
    constexpr uint8_t MAGIC_COMPRESSED_CHAIN   = 0x0C;
    constexpr uint8_t MAGIC_UNCOMPRESSED_CHAIN = 0x4C;
    constexpr uint32_t MIN_VALID_FIRST_SEGMENT = 8;
    constexpr uint32_t MIN_OODLE_BLOCK_SIZE = 16;
    constexpr uint32_t MAX_BLOCKS = 5000000;
    constexpr uint32_t MAX_WALK_ITERATIONS = 100000;
}

inline size_t SafeCompressBound(size_t srcSize) {
    return srcSize * 2 + 65536;
}

typedef int64_t (OodleLZ_Decompress_t)(
    const void* src, int64_t srcLen, void* dst, int64_t dstLen,
    int32_t fuzzSafe, int32_t checkCRC, int32_t verbosity,
    void* decBufBase, size_t decBufSize, void* fpCallback, void* callbackUserData,
    void* decoderMemory, size_t decoderMemorySize, int32_t threadPhase, int32_t unused);

typedef int64_t (OodleLZ_Compress_t)(
    int32_t codec, const void* src, int64_t srcLen, void* dst, int32_t level,
    void* opts, const void* dictionaryBase, const void* lrm,
    void* scratchMem, int64_t scratchSize);

extern OodleLZ_Decompress_t* OodleLZ_Decompress;
extern OodleLZ_Compress_t* OodleLZ_Compress;
bool LoadOodle();

namespace Logger {
    enum class Level { Info, Warn, Error, Debug };
    extern bool is_debug_enabled;
    extern std::ofstream debug_log;
    extern std::mutex log_mutex;

    inline void Init(bool debug) {
         is_debug_enabled = debug;
         if (is_debug_enabled) debug_log.open("oodleforge_debug.log", std::ios::app);
     }

    inline void Log(Level lvl, const std::string& msg) {
         if (lvl == Level::Debug && !is_debug_enabled) return;
         std::lock_guard<std::mutex> lock(log_mutex);
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

struct AES_ctx; 

struct AESContextDeleter {
    void operator()(AES_ctx* ctx) const;
};
using AESContextPtr = std::unique_ptr<AES_ctx, AESContextDeleter>;

AES_ctx* AES_Context_Create(const uint8_t* key);
void     AES_Context_Destroy(AES_ctx* ctx);
void     AES_Context_Decrypt(AES_ctx* ctx, uint8_t* buf, uint32_t len);
void     AES_Context_Encrypt(AES_ctx* ctx, uint8_t* buf, uint32_t len);

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
    Result(T val) : value(std::move(val)), code(ErrorCode::SUCCESS), is_error(false) {}
    Result(ErrorCode c, std::string msg) : value(), error_msg(std::move(msg)), code(c), is_error(true) {}
    
    bool is_err() const { return is_error; }
    const std::string& get_error() const { return error_msg; }
    
    // [FIX] Ref-qualified overloads to resolve GCC ambiguity and enable safe moves
    T& get_value() & { return value; }
    const T& get_value() const & { return value; }
    T get_value() && { return std::move(value); }
    
    ErrorCode get_code() const { return code; }
};

#pragma pack(push, 1)
struct PreHeader {
    uint32_t magic;
    uint32_t version;
    uint64_t original_size;
    uint32_t block_count;
    uint8_t use_aes;
    uint8_t aes_key[32];
    uint32_t space_speed_tradeoff_bytes;
    uint8_t quantum_crc;
    uint8_t reserved[6];
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
    std::atomic<uint32_t> blocks_found{0};
    std::atomic<uint32_t> matches_identified{0};
    std::atomic<uint32_t> fails_unidentified{0};
    void add_match() { 
        matches_identified.fetch_add(1, std::memory_order_relaxed); 
    }
};

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
            if (this != &o) {
                if (ptr && pool) pool->release(ptr);
                ptr = o.ptr;
                pool = o.pool;
                o.ptr = nullptr;
                o.pool = nullptr;
            }
            return *this;
        }
        Handle(const Handle&) = delete;
        Handle& operator=(const Handle&) = delete;
        std::vector<T>& get() { return *ptr; }
    };

    Handle acquire() {
        std::unique_lock<std::mutex> lock(mtx);
        if(available.empty()) available.push(std::make_shared<std::vector<T>>(item_size));
        auto p = available.front(); available.pop(); return Handle(p, this);
    }

    void release(std::shared_ptr<std::vector<T>> p) {
        std::unique_lock<std::mutex> lock(mtx); available.push(p);
    }
};

template <typename T>
class ReusableBufferPool {
    std::mutex mtx;
    std::vector<std::vector<T>> pool;
public:
    std::vector<T> acquire(size_t min_size) {
        std::lock_guard<std::mutex> lock(mtx);
        if (pool.empty()) {
            std::vector<T> buf;
            if (min_size > 0) buf.resize(min_size);
            return buf;
        }
        auto buf = std::move(pool.back());
        pool.pop_back();
        if (buf.size() < min_size) buf.resize(min_size);
        return buf;
    }
    void release(std::vector<T> buf) {
        std::lock_guard<std::mutex> lock(mtx);
        pool.push_back(std::move(buf));
    }
};

template <typename T>
class PooledBuffer {
    ReusableBufferPool<T>* pool;
    std::vector<T> buf;
public:
    PooledBuffer(ReusableBufferPool<T>& p, size_t min_size) : pool(&p), buf(p.acquire(min_size)) {}
    ~PooledBuffer() { if (pool) pool->release(std::move(buf)); }
    
    PooledBuffer(const PooledBuffer&) = delete;
    PooledBuffer& operator=(const PooledBuffer&) = delete;
    
    PooledBuffer(PooledBuffer&& other) noexcept : pool(other.pool), buf(std::move(other.buf)) { other.pool = nullptr; }
    PooledBuffer& operator=(PooledBuffer&& other) noexcept {
        if (this != &other) {
            if (pool) pool->release(std::move(buf));
            pool = other.pool;
            buf = std::move(other.buf);
            other.pool = nullptr;
        }
        return *this;
    }

    T* data() { return buf.data(); }
    const T* data() const { return buf.data(); }
    size_t size() const { return buf.size(); }
    std::vector<T>& vec() { return buf; }
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
    struct Impl;
    std::unique_ptr<Impl> pImpl_;
public:
    FastStreamWriter();
    ~FastStreamWriter();
    bool open(const std::string& path, size_t size = 0);
    void write(const char* data, size_t len);
    void flush();
    void close();
    uint64_t tellp() const;
    void seekp(uint64_t pos);
    bool has_error() const;
    std::string get_last_error() const;
    void clear_error();
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
    std::vector<uint8_t> chain_probe_buf_;
    bool ensure_window(uint64_t pos, size_t needed);
    bool find_next_magic(uint64_t& pos, uint64_t limit);
    uint64_t WalkOodleChain(uint64_t start_pos, uint8_t& codec_out, bool& is_valid);
public:
    BlockScanner(ThreadSafeReader& reader, uint64_t file_size, bool use_aes, const std::vector<uint8_t>& aes_key, size_t win_size);
    ~BlockScanner();
    BlockScanner(const BlockScanner&) = delete;
    BlockScanner& operator=(const BlockScanner&) = delete;
    BlockScanner(BlockScanner&&) = delete;
    BlockScanner& operator=(BlockScanner&&) = delete;
    
    std::shared_ptr<BlockTask> extract_next_block(uint64_t& pos, uint64_t limit, const std::vector<uint32_t>& test_sizes);

    struct ScanDiagnostics {
        uint64_t magic_candidates_found = 0;
        uint64_t passed_fast_rejection = 0;
        uint64_t rejected_by_fast_rejection = 0;
        uint64_t blocks_validated = 0;
        std::map<uint8_t, uint64_t> rejected_b1_histogram;
        uint64_t walk_called = 0;
        uint64_t walk_not_a_chain_start = 0;
        uint64_t walk_first_consistency_fail = 0;
        uint64_t walk_first_too_small = 0;  
        uint64_t walk_bounds_fail = 0;
        uint64_t walk_succeeded = 0;
    };
    ScanDiagnostics diagnostics_;
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

    template <typename F>
    std::future<std::invoke_result_t<F>> enqueue(F&& f) {
        using return_type = std::invoke_result_t<F>;
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

uint32_t CalculateCRC32(const uint8_t* data, size_t len);
bool write_gap(ThreadSafeReader& reader, FastStreamWriter& writer, ObjectPool<char>& pool, uint64_t offset, uint64_t length);
std::vector<int32_t> ParseMethods(const std::string& input);
std::vector<int32_t> ParseLevels(const std::string& input);
std::vector<uint8_t> ParseKey(const std::string& hex);
void ResolveAESKey(std::vector<uint8_t>& aesKey, bool& useAES, const PreHeader& hdr);
uint32_t GetOodleBlockSize(const uint8_t* hdr, size_t available_len, uint8_t& codec_out);

int64_t CompressAndVerify(int32_t method, int32_t level, const uint8_t* src, uint32_t usize,
                          const uint8_t* expected, uint32_t expected_size, std::vector<uint8_t>& temp_buf,
                          const OodleLZ_CompressOptions* opts, void* scratchMem, int64_t scratchSize);

bool TryMatchBlock(const std::shared_ptr<BlockTask>& task,
                   const std::vector<int32_t>& all_methods,
                   const std::vector<int32_t>& all_levels,
                   std::set<std::pair<int32_t, int32_t>>& cache,
                   std::shared_mutex& cache_mutex,
                   int32_t& out_method, int32_t& out_level,
                   const OodleLZ_CompressOptions* opts);

Result<int> RunScan(const std::string& input_path, bool verbose, int num_threads,
                    const std::vector<int32_t>& m_ids, const std::vector<int32_t>& opt_levels,
                    bool opt_auto, bool opt_force, const std::vector<uint8_t>& aesKey,
                    bool useAES, double scan_percent, bool debug_mode,
                    uint32_t tradeoff_bytes, bool quantum_crc);

Result<int> RunEncode(const std::string& input_path, const std::string& output_path, bool verbose, int num_threads,
                      const std::vector<int32_t>& m_ids, const std::vector<int32_t>& opt_levels, bool opt_auto, bool opt_force,
                      const std::vector<uint8_t>& aesKey, bool useAES, uint32_t tradeoff_bytes, bool quantum_crc);

Result<int> RunReconstruct(const std::string& input_path, const std::string& output_path, bool verbose, int num_threads,
                           std::vector<uint8_t>& aesKey, bool& useAES, uint32_t tradeoff_bytes, bool quantum_crc);
