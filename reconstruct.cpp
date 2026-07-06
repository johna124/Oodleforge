#include "common.h"
#include <iomanip>
#include <future>

Result<int> RunReconstruct(const std::string& input_path, const std::string& output_path, bool verbose, int num_threads,
                           std::vector<uint8_t>& aesKey, bool& useAES, uint32_t tradeoff_bytes, bool quantum_crc) {
    ThreadSafeReader reader(input_path);
    if (!reader.is_open()) {
        return Result<int>(ErrorCode::ERR_FILE_NOT_FOUND, "Failed to open target archive file: " + input_path);
    }

    uint64_t fsz = reader.get_size();
    if (fsz < sizeof(PreHeader)) {
        return Result<int>(ErrorCode::ERR_INVALID_MAGIC, "File is too small to be a valid archive.");
    }

    PreHeader hdr;
    reader.pread(reinterpret_cast<char*>(&hdr), sizeof(hdr), 0);

    if (hdr.magic != 0x50524546 || hdr.version != 34) { // UPDATED TO V34
        return Result<int>(ErrorCode::ERR_INVALID_MAGIC, "Invalid archive magic or version mismatch.");
    }

    ResolveAESKey(aesKey, useAES, hdr);
    tradeoff_bytes = hdr.space_speed_tradeoff_bytes;
    quantum_crc = hdr.quantum_crc != 0;

    if (hdr.block_count > Config::MAX_BLOCKS) {
        return Result<int>(ErrorCode::ERR_INVALID_MAGIC,
            "Archive claims too many blocks (" + std::to_string(hdr.block_count) + " > " + std::to_string(Config::MAX_BLOCKS) + ")");
    }

    std::vector<PreBlock> blocks(hdr.block_count);
    uint64_t blocks_meta_size = hdr.block_count * sizeof(PreBlock);
    if (fsz < sizeof(PreHeader) + blocks_meta_size) {
        return Result<int>(ErrorCode::ERR_INVALID_MAGIC, "Archive metadata exceeds file size.");
    }

    reader.pread(reinterpret_cast<char*>(blocks.data()), blocks_meta_size, fsz - blocks_meta_size);

    std::cout << "[REC] Reconstructing " << hdr.original_size / 1024 / 1024 << " MB (" << hdr.block_count << " blocks) using " << num_threads << " threads." << std::endl;

    FastStreamWriter fast_out;
    if (!fast_out.open(output_path)) {
        return Result<int>(ErrorCode::ERR_FILE_NOT_FOUND, "Failed to open rebuild file.");
    }

    AESContextPtr aesCtxE;
    if (useAES) {
        if (aesKey.size() != 32) {
            return Result<int>(ErrorCode::ERR_INVALID_ARGUMENT, "AES key must be 32 bytes.");
        }
        aesCtxE = AESContextPtr(AES_Context_Create(aesKey.data()));
        if (!aesCtxE) {
            return Result<int>(ErrorCode::ERR_UNKNOWN, "Failed to create AES encryption context.");
        }
    }

    ThreadPool pool(num_threads);
    ObjectPool<char> gap_pool(num_threads * 2, Config::GAP_POOL_CHUNK);
    UI ui(hdr.original_size, hdr.block_count, verbose);

    std::atomic<uint32_t> dec_matches{0}, dec_fails{0};
    std::queue<std::future<std::shared_ptr<DecTask>>> dec_queue;

    uint64_t archive_offset = sizeof(PreHeader);
    uint64_t original_offset = 0;
    std::atomic<bool> fatal_abort{false};
    auto last_ui_time = std::chrono::steady_clock::now();
    uint32_t processed_blocks = 0;

    OodleLZ_CompressOptions opts = {};
    opts.version = 232;
    opts.spaceSpeedTradeoffBytes = tradeoff_bytes;
    opts.sendQuantumCRCs = quantum_crc ? 1 : 0;

    auto process_and_write = [&](std::shared_ptr<DecTask> task) -> bool {
        if (task->fatal_error) {
            std::cerr << "\n[FATAL] " << task->error_msg << std::endl;
            return false;
        }

        try {
            if (task->gap_len > 0) {
                if (!write_gap(reader, fast_out, gap_pool, task->gap_archive_offset, task->gap_len)) {
                    std::cerr << "[FATAL] Failed to write gap data.\n";
                    return false;
                }
                if (fast_out.has_error()) {
                    std::cerr << "[FATAL] Gap write failed: " << fast_out.get_last_error() << "\n";
                    return false;
                }
            }

            uint8_t* data_ptr = task->b.exact_match ? task->compressed_out.data() : task->block_data_in.data();
            size_t data_len = task->b.exact_match ? task->compressed_out.size() : task->b.stored_size;

            if (useAES && task->b.was_encrypted && task->b.exact_match) {
                size_t aes_len = (data_len + 15) & ~15;
                if (task->compressed_out.size() < aes_len) {
                    task->compressed_out.resize(aes_len, 0);
                    data_ptr = task->compressed_out.data();
                }
                AES_Context_Encrypt(aesCtxE.get(), data_ptr, static_cast<uint32_t>(aes_len));
                data_len = aes_len;
            }

            fast_out.write(reinterpret_cast<const char*>(data_ptr), data_len);
            if (fast_out.has_error()) {
                std::cerr << "[FATAL] Write failed: " << fast_out.get_last_error() << "\n";
                return false;
            }
        } catch (const std::exception& e) {
            std::cerr << "\n[FATAL] File I/O Error: " << e.what() << std::endl;
            return false;
        }
        return true;
    };

    for (uint32_t i = 0; i < hdr.block_count; ++i) {
        if (fatal_abort) break;
        const auto& b = blocks[i];
        uint64_t gap = b.original_offset - original_offset;

        auto task = std::make_shared<DecTask>();
        task->b = b;
        task->gap_len = gap;
        task->gap_archive_offset = archive_offset;
        archive_offset += gap;
        task->o_pos_start = original_offset;

        if (b.exact_match) {
            if (b.stored_size < sizeof(BlockHeader) + b.decompressed_size) {
                task->fatal_error = true;
                task->error_msg = "Invalid stored_size for exact-match block: " + std::to_string(b.stored_size);
                fatal_abort = true;
                break;
            }
        }

        task->block_data_in.resize(b.stored_size);
        if (reader.pread(reinterpret_cast<char*>(task->block_data_in.data()), b.stored_size, archive_offset) != b.stored_size) {
            task->fatal_error = true;
            task->error_msg = "Failed to read block data at offset " + std::to_string(archive_offset);
            fatal_abort = true;
            break;
        }

        if (b.exact_match) {
            if (b.decompressed_size > Config::MAX_SAFE_COMPRESSED_SIZE) {
                task->fatal_error = true;
                task->error_msg = "Decompressed size too large: " + std::to_string(b.decompressed_size);
                fatal_abort = true;
                break;
            }

            dec_queue.push(pool.enqueue([task, &dec_matches, &opts]() {
                if (task->b.original_compressed_size > Config::MAX_SAFE_COMPRESSED_SIZE) {
                    task->fatal_error = true;
                    task->error_msg = "Unreasonable compressed size detected.";
                    return task;
                }

                // FIX 2 & 3: Thread-local buffers to prevent OOM and internal Oodle mallocs
                thread_local std::vector<uint8_t> safe_temp;
                thread_local std::vector<uint8_t> scratch_mem;
                const size_t SCRATCH_SIZE = 8 * 1024 * 1024;
                if (scratch_mem.size() < SCRATCH_SIZE) scratch_mem.resize(SCRATCH_SIZE);

                size_t safe_bound = SafeCompressBound(task->b.decompressed_size);
                if (safe_temp.size() < safe_bound) safe_temp.resize(safe_bound);

                int32_t rec_method = task->b.compressor & 0xFF;
                int32_t rec_level = task->b.compressor >> 8;

                // Pass scratch memory to Oodle
                int64_t comp_res = OodleLZ_Compress(
                    rec_method, task->block_data_in.data() + sizeof(BlockHeader),
                    task->b.decompressed_size, safe_temp.data(), rec_level,
                    &opts, nullptr, nullptr, scratch_mem.data(), scratch_mem.size()
                );

                if (comp_res <= 0 || static_cast<size_t>(comp_res) > safe_temp.size()) {
                    task->fatal_error = true;
                    task->error_msg = "Oodle Re-compression failed or output overflow.";
                    return task;
                }

                task->compressed_out.assign(safe_temp.begin(), safe_temp.begin() + comp_res);
                dec_matches++;
                return task;
            }));
        } else {
            dec_queue.push(pool.enqueue([task, &dec_fails]() {
                uint32_t crc_stored = CalculateCRC32(task->block_data_in.data(), task->b.stored_size);
                if (crc_stored != task->b.crc32) {
                    task->fatal_error = true;
                    task->error_msg = "CRC32 Integrity Check Failed for raw block.";
                    return task;
                }
                dec_fails++;
                return task;
            }));
        }

        archive_offset += b.stored_size;
        original_offset = b.original_offset + b.original_compressed_size;

        while (dec_queue.size() >= static_cast<size_t>(num_threads * 2)) {
            std::shared_ptr<DecTask> completed;
            try {
                completed = dec_queue.front().get();
            } catch (const std::exception& e) {
                std::cerr << "\n[FATAL] Task exception: " << e.what() << std::endl;
                fatal_abort = true;
                break;
            }
            dec_queue.pop();

            if (!process_and_write(completed)) {
                fatal_abort = true;
                break;
            }

            processed_blocks++;
            auto now = std::chrono::steady_clock::now();
            if (std::chrono::duration_cast<std::chrono::milliseconds>(now - last_ui_time).count() >= 500) {
                ui.set_stats(dec_matches.load(), dec_fails.load());
                ui.update(completed->o_pos_start, processed_blocks, "REC", fast_out.tellp());
                last_ui_time = now;
            }
        }
        if (fatal_abort) break;
    }

    while (!dec_queue.empty() && !fatal_abort) {
        std::shared_ptr<DecTask> completed;
        try {
            completed = dec_queue.front().get();
        } catch (const std::exception& e) {
            std::cerr << "\n[FATAL] Task exception: " << e.what() << std::endl;
            fatal_abort = true;
            break;
        }
        dec_queue.pop();
        if (!process_and_write(completed)) {
            fatal_abort = true;
        }
    }

    if (fatal_abort) {
        return Result<int>(ErrorCode::ERR_UNKNOWN, "Reconstruction aborted due to fatal error.");
    }

    try {
        if (hdr.original_size > original_offset) {
            if (!write_gap(reader, fast_out, gap_pool, archive_offset, hdr.original_size - original_offset)) {
                return Result<int>(ErrorCode::ERR_UNKNOWN, "Failed to write final gap.");
            }
            if (fast_out.has_error()) {
                return Result<int>(ErrorCode::ERR_UNKNOWN, "Final gap write failed: " + fast_out.get_last_error());
            }
        }
        fast_out.flush();
        if (fast_out.has_error()) {
            return Result<int>(ErrorCode::ERR_UNKNOWN, "Final flush failed: " + fast_out.get_last_error());
        }
    } catch (const std::exception& e) {
        return Result<int>(ErrorCode::ERR_UNKNOWN, std::string("Final file write/flush failed: ") + e.what());
    }

    std::cout << std::endl << "--- Reconstruction Performance ---" << std::endl
              << "Restored Architecture Size: " << hdr.original_size / 1024.0 / 1024.0 << " MB" << std::endl
              << "Exact Matches: " << dec_matches.load() << " | Full Copies: " << dec_fails.load() << std::endl
              << "Total Duration: " << ui.format_time(ui.get_elapsed()) << std::endl;

    return Result<int>(0);
}
