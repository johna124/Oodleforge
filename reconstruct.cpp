#include "common.h"
#include <iomanip>
#include <future>

Result<int> RunReconstruct(const std::string& input_path, const std::string& output_path, bool verbose, int num_threads, std::vector<uint8_t>& aesKey, bool& useAES) {
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
    if (hdr.magic != 0x50524546 || hdr.version != 33) {
        return Result<int>(ErrorCode::ERR_INVALID_MAGIC, "Invalid archive magic or version mismatch.");
    }
    ResolveAESKey(aesKey, useAES, hdr);

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

    AESContextPtr aesCtxE = useAES ? AESContextPtr(AES_Context_Create(aesKey.data())) : nullptr;

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

    auto process_and_write = [&](std::shared_ptr<DecTask> task) -> bool {
        if (task->fatal_error) {
            std::cerr << "\n[FATAL] " << task->error_msg << std::endl;
            return false;
        }
        if (task->gap_len > 0) {
            write_gap(reader, fast_out, gap_pool, task->gap_archive_offset, task->gap_len);
        }

        uint8_t* data_ptr = task->b.exact_match ? task->compressed_out.data() : task->block_data_in.data();
        size_t data_len = task->b.exact_match ? task->b.original_compressed_size : task->b.stored_size;

        if (useAES && task->b.was_encrypted) {
            size_t aes_len = (data_len + 15) & ~15;
            if (task->b.exact_match && task->compressed_out.size() < aes_len) task->compressed_out.resize(aes_len, 0);
            if (!task->b.exact_match && task->block_data_in.size() < aes_len) task->block_data_in.resize(aes_len, 0);
            AES_Context_Encrypt(aesCtxE.get(), data_ptr, static_cast<uint32_t>(aes_len));
        }

        fast_out.write(reinterpret_cast<const char*>(data_ptr), data_len);
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
        task->block_data_in.resize(b.stored_size);

        if (reader.pread(reinterpret_cast<char*>(task->block_data_in.data()), b.stored_size, archive_offset) != b.stored_size) {
            task->fatal_error = true;
            task->error_msg = "Failed to read block data at offset " + std::to_string(archive_offset);
            fatal_abort = true;
            break;
        }

        if (b.exact_match) {
            dec_queue.push(pool.enqueue([task, &dec_matches]() {
                if (task->b.original_compressed_size > Config::MAX_SAFE_COMPRESSED_SIZE) {
                    task->fatal_error = true;
                    task->error_msg = "Unreasonable compressed size detected.";
                    return task;
                }
                uint32_t crc_stored = CalculateCRC32(task->block_data_in.data(), task->b.stored_size);
                if (crc_stored != task->b.crc32) {
                    std::cerr << "\n[WARN] CRC32 mismatch on stored exact-match block (offset 0x"
                              << std::hex << task->b.original_offset << std::dec
                              << "). Data was probably stored with a different Oodle version. Proceeding..." << std::endl;
                }
                std::vector<uint8_t> safe_temp(task->b.original_compressed_size + 1024 * 1024);
                int32_t rec_method = task->b.compressor & 0xFF;
                int32_t rec_level = task->b.compressor >> 8;
                int64_t comp_res = OodleLZ_Compress(
                    rec_method, task->block_data_in.data() + sizeof(BlockHeader), task->b.decompressed_size,
                    safe_temp.data(), rec_level, nullptr, nullptr, nullptr, 0
                );
                if (comp_res <= 0) {
                    task->fatal_error = true;
                    task->error_msg = "Oodle Re-compression failed.";
                    return task;
                }
                task->compressed_out.assign(safe_temp.begin(), safe_temp.begin() + std::min(comp_res, (int64_t)task->b.original_compressed_size));
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

        while (dec_queue.size() >= static_cast<size_t>(num_threads * 3)) {
            auto completed = dec_queue.front().get();
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
    }

    while (!dec_queue.empty()) {
        auto completed = dec_queue.front().get();
        dec_queue.pop();
        if (!fatal_abort && !process_and_write(completed)) {
            fatal_abort = true;
        }
    }

    if (fatal_abort) {
        return Result<int>(ErrorCode::ERR_UNKNOWN, "Reconstruction aborted due to fatal error.");
    }

    if (hdr.original_size > original_offset) {
        write_gap(reader, fast_out, gap_pool, archive_offset, hdr.original_size - original_offset);
    }
    fast_out.flush();

    std::cout << std::endl << "--- Reconstruction Performance ---" << std::endl
              << "Restored Architecture Size: " << hdr.original_size / 1024.0 / 1024.0 << " MB" << std::endl
              << "Exact Matches: " << dec_matches.load() << " | Full Copies: " << dec_fails.load() << std::endl
              << "Total Duration: " << ui.format_time(ui.get_elapsed()) << std::endl;

    return Result<int>(0);
}
