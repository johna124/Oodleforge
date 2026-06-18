OodleForge Technical Reference
Version: 33.0
Date: June 2026
Developed in 34 days on a 17-year-old PC
This document contains complete low-level technical details of the OodleForge archive format, data structures, algorithms, and implementation notes.

Archive File Format (.oodle)

1.1 PreHeader (64 bytes at offset 0)
struct PreHeader {
uint32_t magic;          // 0x50524546 ("PREF")
uint32_t version;        // 33
uint64_t original_size;  // Original file size
uint32_t block_count;    // Number of blocks
uint8_t  use_aes;        // 0 = disabled, 1 = enabled
uint8_t  aes_key[32];    // AES-256 key (if use_aes == 1)
uint8_t  reserved[23];   // Padding to 64 bytes
} attribute((packed));
1.2 File Layout Overview
[ PreHeader (64 bytes) ]
[ Block Data Region (variable) ]

Gap data (raw bytes from original file)
Block 0 data
Gap data
Block 1 data
...
[ PreBlock Metadata Array (at END of file) ]
PreBlock[0]
PreBlock[1]
...
PreBlock[block_count-1]

The metadata is stored at the end to allow streaming writes during encoding.

PreBlock Structure (40 bytes per entry)

struct PreBlock {
uint64_t original_offset;         // Offset in original file
uint32_t stored_size;             // Size of data stored in .oodle
uint32_t decompressed_size;       // Uncompressed size (usize)
uint32_t original_compressed_size;// Original compressed size
uint32_t compressor;              // method | (level << 8)
uint8_t  exact_match;             // 1 = exact match
uint8_t  was_encrypted;           // 1 = was AES encrypted
uint8_t  reserved;                // 1 byte padding
uint32_t crc32;                   // CRC32 of data as written
} attribute((packed));

Block Types


exact_match = 1 : Exact Oodle match
Stored as: BlockHeader (4 bytes) + decompressed data
Reconstruction: Re-compress using saved method + level
exact_match = 0 : Raw / Failed match
Stored as: Original compressed Oodle data (verbatim)
Reconstruction: Copied as-is

BlockHeader (only for exact_match blocks):
struct BlockHeader {
uint32_t stored_size;   // == decompressed_size
} attribute((packed));

Example Hex Dumps

4.1 PreHeader Example (AES enabled)
00000000  46 52 45 50 21 00 00 00 00 00 00 00 00 00 00 00  PREF!...........
00000010  00 00 00 00 00 00 00 00 01 2b 7e 15 16 28 ae d2 a6  .........+~..(..
00000020  ab f7 15 88 09 cf 4f 3c 00 00 00 00 00 00 00 00 00  ......O<........
00000030  00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00  ................

magic = 50 52 45 46 ("PREF")
version = 33
use_aes = 01
AES key starts at byte 0x11

4.2 PreBlock Example (Exact Match, Kraken Level 5)
00000000  00 00 00 00 00 00 00 00 00 10 00 00 00 00 10 00  ................
00000010  00 00 10 00 05 08 00 00 01 01 00 00 ab cd ef 12  ................

original_offset = 0
stored_size = 4096
decompressed_size = 4096
compressor = 0x00000805 (Method 8 / Level 5)
exact_match = 1
was_encrypted = 1


Core Algorithms

5.1 Block Scanning

Sliding window (64-128 MiB)
Searches for magic bytes 0x8C and 0xCC
Uses binary search + OodleLZ_Decompress to find exact compressed size
AES decryption done transparently inside the window

5.2 Exact Match Detection

Tries multiple method + level combinations
Uses OodleLZ_Compress + memcmp for perfect byte-for-byte match
Successful pairs are cached across threads

5.3 Reconstruction

Reads metadata from end of file
exact_match blocks: re-compress using stored method+level
Raw blocks: copy verbatim
Gaps filled with original data using write_gap()

5.4 AES-256-CBC

Uses tiny-AES-C implementation
Per-block encryption/decryption
Standard CBC mode (IV = previous ciphertext)
Padded to 16-byte boundaries

5.5 I/O Optimizations (v33.1 Architecture)
ThreadSafeReader: Platform-specific fast random reads.
FastStreamWriter (Async Double-Buffered):
64 MB in-memory ring buffer.
Dedicated background physical disk worker thread.
True parallelization: Main encoder threads write to RAM at ~50 GB/s while the mechanical drive flushes 64MB chunks in the background.
Zero-allocation buffer swapping using std::swap to prevent heap thrashing.
Reduces HDD write operations from 4K+ to only ~16-17 per GB.


Threading & Performance


ThreadPool with bounded queue
Default max 8 threads
Pipeline pattern with backpressure
Thread-local buffers to reduce memory pressure


Changelog

OodleForge v33.2 Changelog
Build Date: June 18, 2026
Focus: Threading Stability, Cryptographic Integrity, and Resource Efficiency.

🛠 Critical Fixes (Backend)
FastStreamWriter Thread-Safety:
Implemented file_io_mtx to strictly serialize physical disk I/O, preventing file handle contention during multi-threaded heavy writes.
Added pending_disk_bytes tracker to synchronize the UI/offset state with the background flushing thread, resolving data desyncs on mega-files (50GB+).

AES Cryptographic Correction:

IV Initialization: Swapped malloc for calloc in AES_Context_Create. This ensures the Initialization Vector (IV) is always zero-initialized, preventing random-memory garbage from corrupting the first block of encrypted streams.

Key Length: Updated aes.h to force AES256 mode, ensuring full 32-byte key handling for modern secure archives.

Scanner Cache (Performance Optimization):

Added a "double-check" lock pattern in scan.cpp's TryMatchBlock. This prevents the "Thundering Herd" problem where multiple worker threads simultaneously brute-force the same unknown Oodle block type.

🚀 Portability & Build
POSIX Compatibility: Sanitized LoadOodle() to remove Windows-specific .dll fallback logic on Linux/Unix systems, allowing clean liboo2core.so loading.

Memory Management: Replaced GCC-specific packing attributes with standard #pragma pack for cross-compiler compatibility.

📊 UI & Monitoring
State Tracking: Fixed seekp implementation to ensure the background thread fully flushes before the file pointer moves, preventing data corruption during reconstruction.

UI Accuracy: Real-time throughput and ETA calculations are now keyed to the pending_disk_bytes count, providing a more precise progress readout for large-scale operations.


Known Limitations


Max recommended threads: 8
AES assumes standard per-block CBC
Scanning can have false positives on random data
Requires matching oo2core DLL version for perfect reconstruction
No support for Oodle dictionaries
Best performance on SSD with -ssd flag


Wine Compatibility
This tool runs very well under Wine without problems. The "it actually works" factor is high. Tested on Linux via Wine.
