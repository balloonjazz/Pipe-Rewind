# PipeRewind — Presentation Transcript
### Luiz Takahashi & Ehzoc Chavez
### Total Time: ~12–14 minutes

---

## PART 1: ELEVATOR PITCH (Slides 1–6) — ~2 minutes

---

### Slide 1 — Title Slide

> **Luiz:** Good morning everyone. We're Luiz Takahashi and Ehzoc Chavez and today we're presenting **PipeRewind** — a time-travel debugger for UNIX shell pipelines.

*(~10 seconds)*

---

### Slide 2 — Section Header

> **Luiz:** We'll start with a quick pitch of what this project is, then cover our design decisions and trade-offs, and finally walk you through every feature with real examples.

*(~10 seconds)*

---

### Slide 3 — The Problem

> **Luiz:** So here's the problem we set out to solve. When you chain together commands with pipes — like `cat` into `grep` into `sort` into `uniq` — each one connects through the kernel. The data between them is completely invisible to you. If the output is wrong or empty, you have no idea which stage caused it.

> The standard workarounds are poor. `tee` requires you to rewrite the entire command. `strace` gives you thousands of unrelated syscall lines. Neither gives you a clean picture of what data flowed where.

*(~35 seconds)*

---

### Slide 4 — The Solution

> **Ehzoc:** PipeRewind solves this by sitting between every pair of commands. It creates its own pipes, intercepts every byte, timestamps it with nanosecond precision using `CLOCK_MONOTONIC`, and saves everything into a compact binary trace file. The pipeline itself runs exactly the same — same output, same behavior. It's completely transparent.

> After recording, you can open the trace in a full-screen terminal viewer and literally scrub backwards and forwards through time, seeing what every stage sent and received.

*(~30 seconds)*

---

### Slide 5 — Systems Programming Themes

> **Ehzoc:** This project hits six major systems programming themes. Process control with `fork` and `exec`. Event-driven concurrency with `epoll`. System-level I/O with raw `read`, `write`, and `dup2`. Process scheduling observation via the `/proc` filesystem. POSIX threading with `pthreads` for our live mode. And custom binary file formats with our `.prt` trace specification.

*(~25 seconds)*

---

### Slide 6 — System Architecture

> **Luiz:** Here's the architecture. About 2,900 lines of C99 across seven source files. Every file has a single responsibility. `pipeline.c` parses commands, `capture.c` runs the epoll engine, `trace.c` handles binary I/O, `tui.c` is the viewer, `diff.c` does side-by-side comparisons, and `procstate.c` monitors process states via `/proc`. `main.c` ties them all together as a CLI dispatcher.

*(~25 seconds)*

---

## PART 2: DESIGN CHOICES & TRADE-OFFS (Slides 7–9) — ~2 minutes

---

### Slide 7 — Section Header

> **Ehzoc:** Now let's talk about the key decisions we made and why.

*(~5 seconds)*

---

### Slide 8 — Key Design Decisions

> **Ehzoc:** Four big design choices. First, we use `epoll` instead of `select` or `poll` because it's O(1) per event notification — it scales to any pipeline depth without scanning all file descriptors every time.

> Second, we designed a custom binary format instead of JSON or plain text. Because events are variable-length, we need byte-level control. And the index table at the end gives O(1) random access via `fseek` — you can jump to any event instantly.

> Third, we chose parent-process interposition instead of `ptrace` or `LD_PRELOAD`. No privilege escalation needed, and child processes run completely unmodified.

> Fourth — the sliding window in our TUI. A 100K-event trace could be hundreds of megabytes. We keep a window of 20,000 events in memory, roughly under 16 MB, and load more dynamically.

*(~55 seconds)*

---

### Slide 9 — Trade-offs & Limitations

> **Luiz:** Every design has trade-offs. Our capture engine is single-threaded — no locking, no race conditions, but a CPU-bound stage could theoretically starve the epoll loop. For our live mode, we use `fflush` for synchronization instead of mutexes — simple but causes a bit of I/O overhead. And our shell parser handles quotes and escapes but doesn't support redirections, subshells, or environment variables.

> On the limitations side: we're Linux-only because of `epoll` and `/proc`, we only capture stdout — not stderr — and the TUI requires a real terminal. These are clear targets for future work.

*(~40 seconds)*

---

## PART 3: FEATURE WALKTHROUGH (Slides 10–21) — ~9 minutes

---

### Slide 10 — Section Header

> **Ehzoc:** Now the main portion — a walkthrough of every feature. We'll show you exactly how PipeRewind works from build to live debugging.

*(~8 seconds)*

---

### Slide 11 — Building & Running

> **Luiz:** Building is one command: `make clean && make`. That compiles seven source files and links against ncurses and pthreads. `make test` runs five automated smoke tests covering two-, three-, and four-stage pipelines, plus single and double quoted arguments. All tests verify both the capture and the dump output.

*(~25 seconds)*

---

### Slide 12 — Recording a Pipeline

> **Luiz:** The record command is where everything starts. You wrap your pipeline in quotes and PipeRewind handles the rest. Under the hood, three things happen: First, `pipeline.c` tokenizes the command string with a state machine that correctly handles quotes and escapes. Second, `capture.c` forks a child for each stage, redirects file descriptors with `dup2`, and runs `execvp`. Third, the epoll loop sits in the parent, waiting for data on any pipe, timestamping each chunk, writing events to the trace file, and forwarding the bytes downstream.

*(~35 seconds)*

---

### Slide 13 — Dump Command

> **Ehzoc:** The dump command is the simplest way to read a trace. It opens the `.prt` file, reads the header and stage information, then prints every event chronologically. You can see here — stage 0 produced `banana, apple, cherry`, stage 1 received it, sorted it to `apple, banana, cherry`, and stage 2 kept only the first two lines. Every byte is accounted for, every timestamp is recorded. You can read this top to bottom and reconstruct the entire pipeline execution.

*(~30 seconds)*

---

### Slide 14 — Replay TUI

> **Luiz:** For a richer experience, there's the interactive TUI. This is a full-screen ncurses interface with four regions: a header bar, a timeline with progress, a list of stages with color-coded status, and a data panel showing the actual bytes at the current point in time.

> You navigate with `j` and `k` to step one event at a time, `J` and `K` to jump by ten, arrow keys to select different stages, and `Home`/`End` to jump to the beginning or end. It's designed to feel like scrubbing through a video. *(gesture to screenshot)*

*(~35 seconds)*

---

### Slide 15 — Side-by-Side Diff

> **Ehzoc:** Press `d` inside the TUI and you get a side-by-side diff. This compares what one stage sent with what the next stage received. We use the Longest Common Subsequence algorithm — the same approach used by `git diff` — implemented in `diff.c` with O(n times m) dynamic programming. Lines are color-coded: green for additions, red for removals.

> In this example, `sort` takes the unsorted input and reorders it. You can instantly verify that it worked correctly, or if something was corrupted, you'd see it highlighted immediately. No more manually saving intermediate outputs to temp files.

*(~35 seconds)*

---

### Slide 16 — epoll Deep Dive

> **Luiz:** Let's go deeper into the capture engine. The epoll loop is seven steps: `epoll_wait` blocks until any pipe has data. We look up which stage the file descriptor belongs to. We `read` into a 64KB buffer — which matches the kernel's pipe capacity. We record a `DATA_OUT` event for the sending stage, a `DATA_IN` event for the receiving stage, then `write` the data forward to the next stage's stdin. If `read` returns zero, we record a `PIPE_EOF` and close the descriptor.

> This is O(1) per event — no scanning, no polling. Highly efficient even with many stages.

*(~30 seconds)*

---

### Slide 17 — Process State Monitoring

> **Ehzoc:** PipeRewind also monitors whether each process is running, sleeping, or blocked. It does this by reading `/proc/[pid]/status` — the kernel's real-time status file. The state character tells us: R for running, S for sleeping, D for disk blocked, Z for zombie.

> In the TUI, this translates to color coding — green means the process is actively running, yellow means it's waiting for something, and red means it exited. If you see a stage stuck on yellow while the others have finished, you immediately know where the bottleneck is.

*(~30 seconds)*

---

### Slide 18 — Sliding Window Cache

> **Luiz:** One of the harder engineering challenges: what happens when you have a huge trace? A pipeline processing a large log file could generate 100,000 or more events. Loading all of them with their payloads into memory would use hundreds of megabytes.

> Our solution is a sliding window. The TUI keeps only about 20,000 events in memory at a time — roughly 16 MB. When you scroll near the edge, `tui_load_window` frees the old chunk and loads the new one. The index table makes any fseek jump O(1), so sliding is instant. You can scrub through a million events and never hit an out-of-memory crash.

*(~35 seconds)*

---

### Slide 19 — Live-Attach Mode

> **Ehzoc:** The most advanced feature: live mode. Instead of recording first and replaying later, you do both simultaneously. The architecture uses two threads: a background pthread runs `capture_run` — the full epoll engine writing to `trace.prt`. The main thread runs the ncurses TUI.

> They share the trace file on disk. The writer calls `fflush` after every event to ensure bytes are visible. The reader calls `trace_reader_refresh` inside the getch loop, which detects and indexes newly written events. We use `halfdelay(1)` so getch times out every 100 milliseconds, giving us regular refresh opportunities without blocking on input.

> Press `f` for follow mode, and the TUI automatically scrolls to the latest event as it arrives. It's like watching a live feed of your pipeline's internal data flow.

*(~45 seconds)*

---

### Slide 20 — Binary Trace Format

> **Luiz:** Here's the .prt format in detail. Fixed 32-byte file header with a magic number for validation. Then N stage headers at 280 bytes each. Then a variable-length stream of events — each with a 24-byte header plus payload. And finally, the index table: one 16-byte entry per event containing the timestamp and the file offset.

> That index table is the key design element. It turns any random access query into a single `fseek` plus `fread` — no sequential scanning. This is what makes the sliding window and the TUI scrubbing feel instant.

*(~30 seconds)*

---

### Slide 21 — Live Demo

> **Ehzoc:** And now let's show it running live.

*(Open terminal. Run the following)*

```bash
# 1. Build
make clean && make

# 2. Record
./piperewind record -v "echo -e 'banana\napple\ncherry' | sort | head -2"

# 3. Dump
./piperewind dump trace.prt

# 4. Replay (navigate with j/k, press d for diff, q to quit)
./piperewind replay trace.prt

# 5. Live mode (if time permits)
./piperewind live "for i in $(seq 1 20); do echo line_$i; sleep 0.2; done | sort"
```

*(~2–4 minutes for demo, adjust based on remaining time)*

---

### Slide 22 — Thank You

> **Luiz:** That's PipeRewind — a systems-level time-travel debugger built entirely in C, hitting all the major themes from this class. Thank you. We're happy to take questions.

*(~10 seconds)*

---

## SPEAKING TIPS

1. **Practice the demo.** Run through the terminal commands at least 3 times before presenting. Know exactly what the output will look like.
2. **Each person should own their slides.** Luiz takes slides 1, 3, 6, 8 second-half, 11, 12, 14, 16, 18, 20, 22. Ehzoc takes 4, 5, 7, 9, 13, 15, 17, 19, 21-demo.
3. **Speak to the audience, not the screen.** Glance at the slide briefly, then face forward.
4. **Use your hands to point** at diagrams and code on the slide when referencing specific parts.
5. **Pace yourself.** If the demo goes fast, fill time by explaining what you see in the dump output line by line.
6. **If something breaks in the demo**, don't panic. You have the dump output on slide 13 as a backup.
7. **Time check:** After Part 2 (slide 9) you should be at roughly 4 minutes. If you're ahead, spend more time on the demo. If behind, skip the `/proc` slide or the binary format slide.
