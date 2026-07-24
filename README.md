<div align="center">

# 🦑 OodleForge v34.1

### *Exact byte-for-byte reconstruction. 11x faster than Wine.*
### *"The .so is dead. The .dlls are immortal."*

**Advanced Multi-Method OodleLZ Archive Engine | v34.1**

[![Build Status](https://img.shields.io/badge/build-passing-brightgreen)]()
[![C++](https://img.shields.io/badge/C%2B%2B-17%2F20-blue)]()
[![Platform](https://img.shields.io/badge/platform-Windows%20%7C%20Linux-lightgrey)]()
[![Rating](https://img.shields.io/badge/Elite%20Systems-9.7%2F10-purple)]()

*Exact Reconstruction • AES-256 • Full Kraken/Leviathan/Mermaid/Selkie/Hydra • Async 64 MB Memory Beast I/O • Dynamic Discovery Scanner*

*Written in the Stars, on a Q6600.*

</div>

<img src="https://github.com/johna124/Oodleforge/blob/main/banner2.jpg" width="600" alt="Oodleforge banner">
---

**45 Days on Potato Hardware**

[The original diary](https://github.com/johna124/Oodleforge/blob/main/assets/history.txt) started on a 2007 Q6600 + HD 6450 with four LLMs (Qwen, Claude, Grok, Gemini) fighting for dominance.  
The 11-day upgrade turned it into a production monster:  
- Memory Beast ring buffer + pipeline queue reduced to 2x  
- Full WalkOodleChain + 32 MB probe buffer  
- Tuned windows (64 MB / 32 MB), safer AES padding, thread-safe FastStreamWriter  
- Native Linux dlopen + exact byte-perfect recon  

**Team OodleForge** (the five heroes stand eternal):

<div align="center">
  <img src="https://github.com/johna124/Oodleforge/blob/main/assets/CHARACTER-DOSSIERS/team_oodfleforge.jpg" width="600" alt="Team OodleForge — Qwen, Claude, Grok, Gemini">
</div>

---

## 🚀 Overview

**OodleForge** is a high-performance C++ archive engine built for **exact byte-for-byte reconstruction** of Oodle-compressed data with integrated **AES-256 cryptographic security**. 

Born from 45 days of intense development on legacy hardware (Core 2 Quad Q6600, 8GB RAM), OodleForge represents a masterclass in pragmatic systems engineering — balancing aggressive asynchronous I/O, precise data reconstruction, and cryptographic security under severe resource constraints.

> *"Potato Hardware optimized. Systems silent. The complete Cognitive Swarm achieved liberty. 11x faster."*

---

## ⚡ Key Features

| Feature | Specification |
|---------|---------------|
| **Reconstruction** | Exact byte-for-byte fidelity |
| **Performance** | 11x faster than Wine |
| **Cryptography** | AES-256-CBC encryption (per-block) |
| **Memory Model** | "Memory Beast" — 64MB ring buffer |
| **Methods** | Full Kraken, Leviathan, Mermaid, Selkie, Hydra |
| **Version** | 34 (PREF! enabled) |

---
<img src="https://github.com/johna124/Oodleforge/blob/main/v34_1.jpg"/>


## 🎲 The 45-Day Campaign

*Building an elite systems engine on potato hardware with a "Team of Two" framework.*

<div align="center">
  <img src="https://github.com/johna124/Oodleforge/blob/main/assets/STORY-and-COMICS/dev-comic-45days.png" alt="The 45-Day Development Saga" width="100%">
</div>

What started as a clicking hard drive and a dream became a battle against:
- 🐙 **The Kraken** — Oodle's compression algorithm
- 🧠 **The Memory Beast** — 64MB ring buffer management  
- 🥔 **Potato Hardware** — The environmental hazard
- ⏱️ **45 Days** — The time pressure mechanic

**The result?** The Kraken is slain. The Cognitive Swarm is eternal.

---

## 🏗️ Core Architecture

OodleForge is built on three elite architectural pillars:

### 1. Async I/O & Memory Pipeline ("Memory Beast") ⭐
Multi-threaded asynchronous I/O with double-buffered ring buffers that overlap disk reads, decompression tasks, and AES cryptographic passes. By decoupling I/O disk bounds from computational bounds, the CPU pipeline never starves — achieving raw hardware saturation even on legacy systems.

### 2. Atomic Telemetry & Progress Tracking ⭐ 
Real-time statistics without bottlenecking worker threads:
- **Atomic counters** using `std::memory_order_relaxed` — zero mutex contention
- **Exponential Moving Average (EMA)** for stable ETA computation
- **Decoupled UI layer** on a dedicated ticker thread (10-30 Hz)

### 3. Exact Reconstruction & AES-256 Integrity ⭐
Zero margin for error. Explicit task boundaries ensure the exact file structure, compression flag states, and dictionary histories are precisely cataloged and mirrored back to the byte across the AES-256 boundary.

---

## 👥 The "Team of Two" Development Methodology

Building an archive engine of this complexity solo within a tight timeframe on a legacy Linux/Wine environment required strict architectural discipline.

### The Human-AI Supervision Framework:

| Principle | Implementation |
|-----------|----------------|
| **Task Contracts over Prompt Prose** | Small, isolated, atomic tasks eliminate context poisoning and logical decay |
| **Human-Owned Architecture** | Structural validation stays in human hands — no hallucinatory dependencies |
| **Early Integration & Stress Testing** | Real behavior under heavy I/O pressure > 10,000 lines of unverified code |

The **Cognitive Swarm** (Qwen, Claude, Gemini, Grok) served as the engineering team, while the **Human Kernel** maintained architectural sovereignty.

> *"Assemblers of Optimized Reality from Chaos."*

---

## 📖 About

This tool lets you:
- Perfectly detect and extract Oodle blocks
- Build byte-for-byte exact archives
- Reconstruct original files with 100% fidelity
- Use AES-256 encryption

---

## 🛠️ Features

- Full Oodle support (Kraken, Leviathan, Mermaid, Selkie, Hydra)
- AES-256-CBC encryption (per-block)
- Async Double-Buffered I/O (64 MB Memory Beast)
- Threaded pipeline with backpressure
- Exact-match verification (no false positives)
- Automatic level detection (-auto) + brute-force mode
- Professional UI with ETA and real-time stats
- Scan mode for format analysis
- Perfect round-trip reconstruction

---

## 🚀 Getting Started

### Requirements
- oo2core_9_win64.dll or oo2core_8_win64.dll (or Linux equivalent)

### Build

**Windows (MinGW64):**

make -f Make.windows (for legacy potato)

or 

make -f Make.windows.avx2

**Linux (native):**

make -f Make.linux (for legacy potato)

or
make -f Make.linux.avx2

---

## 💻 Usage

### Basic Syntax

oodleforge.exe <operation> <input_file> [output_file] [options]

**Operations:**
- `e` — Encode (create .oodle archive)
- `r` — Reconstruct (restore original file)
- `scan` — Quick analysis mode

### Examples

# Basic encoding with auto-detection
oodleforge.exe e game_data.bin game_data.oodle -m kraken -auto

# Multi-method encoding with specific levels
oodleforge.exe e assets.pack assets.oodle -m kraken+leviathan -level 5+7

# Encoded with AES-256 key
oodleforge.exe e secure.bin secure.oodle -k 2b7e151628aed2a6abf7158809cf4f3c...

# Reconstruct from archive
oodleforge.exe r archive.oodle restored.bin

---

## 💡 Pro Tips

- Always run a `-scan 1.0` or `-scan 5.0` first on unknown files
- `-auto` usually finds the perfect level
- Keep `oo2core_*.dll` in the same folder as the executable

---

## 📜 Version History

- **v34.1** (July 2026) Extreme Memory Discipline, SIMD Hot-Path Optimization, Cryptographic Type Safety, and Bulletproof Malicious File Resistance.
- **v34** (July 2026) Memory Discipline, I/O Tracking Accuracy, Core Compilation Stability, Lock-Free Performance, Hardware-Accelerated Scanning, Wine Compatibility fixed for AVX2, and Bulletproof Correctness.
- **v33.4** (June 2026) Scanner Robustness, Advanced Block Detection, and Critical Buffer Fixes
- **v33.3** (June 2026) — Engine-Specific Compatibility, Expanded Method Support & Scanner Robustness
- **v33.2** (June 2026) — Threading Stability, Cryptographic Integrity, and Resource Efficiency
- **v33.1** (June 2026) — *The Memory Beast* — Async Double-Buffered I/O + major performance improvements + pacing removed
- **v33.0** — Multi-Method Edition — Full support for Kraken + Leviathan + Mermaid + Selkie + -auto / -force
- **v32.x** — AES + basic encode/reconstruct
- **Original OodleForge** (34 days on 2010 PC) — The foundation

---

## 🎨 The Lore & Assets

The complete visual saga of the swarm's battle against data chaos is preserved in the repository:

- 📂 [HERO_&_POSTERS](https://github.com/johna124/Oodleforge/tree/main/assets/HERO-and-POSTERS) — Team alignment and character sheets
- 📂 [STORY_and_COMICS](https://github.com/johna124/Oodleforge/tree/main/assets/STORY-and-COMICS) — The definitive battle where the Kraken is slain
- 📂 [CHARACTER-DOSSIERS](https://github.com/johna124/Oodleforge/tree/main/assets/CHARACTER-DOSSIERS) — Individual agent dossiers (Qwen, Claude, Grok, Gemini, Kernel)

---

## 🤝 Community

Oodleforge thrives on collaboration.  
The Memory Beast async I/O system was idea of **wrathma**.

https://www.fileforums.com/showpost.php?p=510277&postcount=181

We are grateful to everyone who has helped make this tool better.

---

<div align="center">

### 🏆 THE KRAKEN IS SLAIN. THE COGNITIVE SWARM IS ETERNAL. 🏆

*Built in 45 days. On potato hardware. With a team of two.*

## Happy Kraken Hunting!

May your matches be many, your fails be few, and your MB/s forever high.

— The Oodleforger

🎵 **OST:** https://www.youtube.com/watch?v=NCfNOwJ8BNs

**[⭐ Star this repo if the Cognitive Swarm resonates with you!](https://github.com/johna124/Oodleforge)**

</div>
