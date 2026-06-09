# Requirements — parallel image processing engine

## Functional requirements

| ID   | Requirement |
|------|-------------|
| F-01 | Accept configurable number of worker processes (--procs N, N ≥ 1) |
| F-02 | Accept configurable number of threads per process (--threads T, T ≥ 1) |
| F-03 | Process all .ppm images in the input directory |
| F-04 | Apply grayscale conversion followed by Sobel edge detection |
| F-05 | Write output images to the specified output directory |
| F-06 | Produce bit-identical output regardless of N and T |

## IPC requirements

| ID   | Mechanism      | Usage |
|------|---------------|-------|
| I-01 | fork()         | Spawn N worker processes |
| I-02 | Pipe           | Workers report per-image completion to master |
| I-03 | Shared memory  | Shared task queue (ring buffer) distributed across processes |
| I-04 | POSIX semaphore | Mutual exclusion on shared task queue (pshared=1) |
| I-05 | pthreads        | T threads per worker pop and process tasks concurrently |

## Synchronisation

- Semaphore protects shared queue head/tail/count — maps to classic producer-consumer / sleeping-barber problem.
- Threads write to separate output files: no mutex needed on file I/O.
- Master waits on all pipes (select) then waitpid() all children before reporting.

## Non-functional requirements

| ID   | Requirement |
|------|-------------|
| N-01 | Compile with gcc -O2 -lpthread -lm, no external libraries |
| N-02 | Benchmark mode (--benchmark) runs 5 configs and prints speedup table |
| N-03 | Tested on Linux (POSIX); Arch Linux / glibc ≥ 2.17 |

## Deliverables

1. Source code + this spec in a git repository

