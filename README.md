# Oodleforge — The Kraken Slayer
**Advanced OodleLZ Archive Engine | v33.2 Threading Stability, Cryptographic Integrity, and Resource Efficiency.**

*Exact Reconstruction • AES-256 • Multi-Method Oodle • Async Memory Beast I/O*

---

## About

This tool lets you:
- Perfectly detect and extract Oodle blocks
- Build byte-for-byte exact archives
- Reconstruct original files with 100% fidelity
- Use AES-256 encryption

---

## Features

- Full Oodle support (Kraken, Leviathan, Mermaid, Selkie)
- AES-256-CBC encryption (per-block)
- Async Double-Buffered I/O (64 MB Memory Beast)
- Threaded pipeline with backpressure
- Exact-match verification (no false positives)
- Automatic level detection (-auto) + brute-force mode
- Professional UI with ETA and real-time stats
- Scan mode for format analysis
- Perfect round-trip reconstruction

---

## Getting Started

### Requirements
- oo2core_9_win64.dll or oo2core_8_win64.dll (or Linux equivalent)

### Build
Windows (MinGW):
make

---

## Usage

### Basic Syntax

oodleforge.exe <operation> <input_file> [output_file] [options]

Operations:
e  — Encode (create .oodle archive)
r  — Reconstruct (restore original file)
scan — Quick analysis mode

### Examples

oodleforge.exe e game_data.bin game_data.oodle -m kraken -auto

oodleforge.exe e assets.pack assets.oodle -m kraken+leviathan -level 5+7

oodleforge.exe e secure.bin secure.oodle -k 2b7e151628aed2a6abf7158809cf4f3c...

oodleforge.exe r archive.oodle restored.bin

---

## Pro Tips

- Always run a -scan 1.0 or -scan 5.0 first on unknown files
- -auto usually finds the perfect level
- Keep oo2core_*.dll in the same folder as the executable

---

## Version History

- v33.2 (June 2026) Threading Stability, Cryptographic Integrity, and Resource Efficiency.

- v33.1 (June 2026) — The Memory Beast  
  Async Double-Buffered I/O + major performance improvements + pacing removed

- v33.0 — Multi-Method Edition  
  Full support for Kraken + Leviathan + Mermaid + Selkie + -auto / -force

- v32.x — AES + basic encode/reconstruct

- Original OodleForge (34 days on 2010 PC) — The foundation

---

## Community

Oodleforge thrives on collaboration.  
The Memory Beast async I/O system was significantly improved thanks to idea of wrathma.

https://www.fileforums.com/showpost.php?p=510277&postcount=181


We are grateful to everyone who has helped make this tool better.

---

## Happy Kraken Hunting!

May your matches be many, your fails be few, and your MB/s forever high.

— The Oodleforger
OST: https://www.youtube.com/watch?v=NCfNOwJ8BNs
