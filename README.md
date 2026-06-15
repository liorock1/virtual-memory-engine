# Virtual Memory – Hierarchical Page Tables

A C++ implementation of a virtual memory subsystem using hierarchical page tables of arbitrary depth, built on top of a simulated physical memory layer.

## Overview

This project implements the core mechanisms that an operating system uses to translate virtual addresses to physical addresses. It supports page faults, demand paging, and page eviction — all without any dynamic memory allocation or global variables.

## How It Works

Virtual addresses are broken into multi-level indices that are walked through a tree of page tables stored in simulated physical memory frames. The root table always lives in frame 0. When a translation misses (page fault), the system allocates a frame using the following priority:

1. **Reuse an empty table frame** — detach it from its parent and repurpose it
2. **Use a fresh unused frame** — if `max_referenced_frame + 1 < NUM_FRAMES`
3. **Evict a page** — choose the data page with the maximum cyclic distance from the target page, swap it out, and reclaim its frame

## Key Components

| File | Purpose |
|------|---------|
| `VirtualMemory.cpp` | Core implementation: `VMinitialize`, `VMread`, `VMwrite` |
| `VirtualMemory.h` | Public API |
| `PhysicalMemory.h/cpp` | Simulated RAM (provided, not submitted) |
| `MemoryConstants.h` | Configurable constants: address widths, page size, tree depth |

## Implementation Details

- **Address translation**: each virtual address is split into a page offset and `TABLES_DEPTH` indices, one per tree level
- **Frame allocator**: a DFS scan (`scan()`) over the live page-table tree collects candidates for all three priority levels in a single pass
- **Eviction policy**: cyclic distance — `min(|a − b|, NUM_PAGES − |a − b|)` — maximised over all resident data pages
- **No heap allocations**: all state lives inside the simulated physical memory frames; STL containers are not used

## Building

```bash
make          # produces libVirtualMemory.a
```

## Configuration

Edit `MemoryConstants.h` to change memory geometry (virtual/physical address width, page size). The implementation adapts automatically — no hardcoded constants are used in the logic.

## Concepts Demonstrated

- Hierarchical (multi-level) page tables
- Demand paging and page fault handling
- Page eviction with a distance-based replacement policy
- Bit manipulation for address decomposition
- Recursive tree traversal without dynamic allocation
