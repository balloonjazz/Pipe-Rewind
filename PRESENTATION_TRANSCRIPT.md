# PipeRewind — Presentation Transcript
### Luiz Takahashi, Ehzoc Chavez, & Jesse Perez
### Total Time: ~12–14 minutes

---

## PART 1: ELEVATOR PITCH (Slides 1–6) — ~2 minutes

---

### Slide 1 — Title Slide

> **Luiz:** Good morning everyone. We're Luiz Takahashi, Ehzoc Chavez, and Jesse Perez. Today we are presenting **PipeRewind** — a time-travel debugger for UNIX shell pipelines.

*(~10 seconds)*

---

### Slide 2 — Section Header

> **Ehzoc:** We'll start with a quick pitch of what this project is, then cover our design decisions and trade-offs, and finally walk you through every feature with real examples.

*(~10 seconds)*

---

### Slide 3 — The Problem

> **Jesse:** Here's the problem we set out to solve. When you chain together commands with pipes — like `cat` into `grep` into `sort` into `uniq` — each one connects through the kernel. The data between them is completely invisible to you. If the output is wrong or empty, you have no idea which stage caused it.
> 
> The standard workarounds are poor. `tee` requires you to rewrite the entire command. `strace` gives you thousands of unrelated syscall lines. Neither gives you a clean picture of what data flowed where.

*(~35 seconds)*

---

### Slide 4 — The Solution

> **Luiz:** PipeRewind solves this by sitting between every pair of commands. It creates its own pipes, intercepts every byte, timestamps it with nanosecond precision using `CLOCK_MONOTONIC`, and saves everything into a compact binary trace file. The pipeline itself runs exactly the same — same output, same behavior. It's completely transparent.
> 
> After recording, you can open the trace in a full-screen terminal viewer and literally scrub backwards and forwards through time, seeing what every stage sent and received.

*(~30 seconds)*

---

### Slide 5 — Systems Programming Themes

> **Ehzoc:** This project hits six major systems programming themes. Process control with `fork` and `exec`. Event-driven concurrency with `epoll`. System-level I/O with raw `read`, `write`, and `dup2`. Process scheduling observation via the `/proc` filesystem. POSIX threading with `pthreads` for our live mode. And custom binary file formats with our `.prt` trace specification.

*(~25 seconds)*

---

### Slide 6 — System Architecture

> **Jesse:** Here's the architecture. We wrote about 2,900 lines of C99 across seven source files. Every file has a single responsibility. `pipeline.c` parses commands, `capture.c` runs the epoll engine, `trace.c` handles binary I/O, `tui.c` is the interactive viewer, `diff.c` does side-by-side comparisons, and `procstate.c` monitors process states via `/proc`. `main.c` ties them all together as a CLI dispatcher.

*(~25 seconds)*

---

## PART 2: DESIGN CHOICES & TRADE-OFFS (Slides 7–9) — ~2 minutes

---

### Slide 7 — Section Header

> **Luiz:** Now let's talk about the key decisions we made and why.

*(~5 seconds)*

---

### Slide 8 — Key Design Decisions

> **Ehzoc:** We made four big design choices. First, we use `epoll` instead of `select` or `poll` because it's O(1) per event notification — it scales to any pipeline depth without scanning all file descriptors every time.
> 
> Second, we designed a custom binary format instead of JSON or plain text. Because events are variable-length, we need byte-level control. And the index table at the end gives O(1) random access via `fseek` — you can jump to any event instantly.
> 
> Third, we chose parent-process interposition instead of `ptrace` or `LD_PRELOAD`. No privilege escalation needed, and child processes run completely unmodified.
> 
> Fourth — the sliding window in our TUI. A 100K-event trace could easily consume hundreds of megabytes. We keep a sliding window of 20,000 events in memory, which keeps our RAM wrapper under 16 MB.

*(~55 seconds)*

---

### Slide 9 — Trade-offs & Limitations

> **Jesse:** Every design has trade-offs. Our capture engine is single-threaded — no locking, no race conditions, but a heavily CPU-bound stage could theoretically starve the epoll loop. For our live mode, we use `fflush` for synchronization instead of mutexes — simple but causes a bit of I/O overhead. And our shell parser handles quotes and escapes natively but doesn't support redirections, subshells, or environment variables.
> 
> On the limitations side: we're Linux-only because of `epoll` and `/proc`, we only capture stdout — not stderr — and the TUI requires a real terminal. These are clear targets for future work.

*(~40 seconds)*

---

## PART 3: FEATURE WALKTHROUGH (Slides 10–21) — ~9 minutes

---

### Slide 10 — Section Header

> **Luiz:** Now the main portion — a walkthrough of every feature. We'll show you exactly how PipeRewind works from build to live debugging.

*(~8 seconds)*

---

### Slide 11 — Building & Running

> **Ehzoc:** Building is one command: `make clean && make`. That compiles the seven source files and links against ncurses and pthreads. `make test` runs five automated smoke tests covering two-, three-, and four-stage pipelines, plus single and double quoted arguments. All tests verify both the capture and the dump output.

*(~25 seconds)*

---

### Slide 12 — Recording a Pipeline

> **Jesse:** The record command is where everything starts. You wrap your pipeline in quotes and PipeRewind handles the rest. Under the hood, three things happen: First, `pipeline.c` tokenizes the command string with a state machine that correctly handles quotes and escapes. Second, `capture.c` forks a child for each stage, redirects file descriptors with `dup2`, and runs `execvp`. Third, the epoll loop sits in the parent, waiting for data on any pipe, timestamping each chunk, writing events to the trace file, and forwarding the bytes downstream.

*(~35 seconds)*

---

### Slide 13 — Dump Command

> **Luiz:** The dump command is the simplest way to read a trace. It opens the `.prt` file, reads the header and stage information, then prints every event chronologically. You can see here — stage 0 produced `banana, apple, cherry`, stage 1 received it, sorted it to `apple, banana, cherry`, and stage 2 kept only the first two lines. Every byte is accounted for, every timestamp is recorded. You can literally read this top to bottom and reconstruct the entire pipeline execution.

*(~30 seconds)*

---

### Slide 14 — Replay TUI

> **Ehzoc:** For a richer experience, there's the interactive TUI. This is a full-screen ncurses interface with four regions: a header bar, a timeline with progress, a list of stages with color-coded status, and a data panel showing the actual bytes at the current point in time.
> 
> You navigate with `j` and `k` to step one event at a time, `J` and `K` to jump by ten, arrow keys to select different stages, and `Home`/`End` to jump to the beginning or end. It's designed to feel exactly like scrubbing through a video.

*(~35 seconds)*

---

### Slide 15 — Side-by-Side Diff

> **Jesse:** Press `d` inside the TUI and you get a side-by-side diff. This compares what one stage sent with what the next stage received. We use the Longest Common Subsequence algorithm — the exact same approach used by `git diff` — implemented in `diff.c` with O(n times m) dynamic programming. Lines are color-coded: green for additions, red for removals.
> 
> In this example, `sort` takes the unsorted input and reorders it. You can instantly verify that it worked correctly, or if something was corrupted, you'd see it highlighted immediately. No more manually saving intermediate outputs to temp files.

*(~35 seconds)*

---

### Slide 16 — epoll Deep Dive

> **Luiz:** Let's go deeper into the capture engine. The epoll loop is seven steps: `epoll_wait` blocks until any pipe has data. We look up which stage the file descriptor belongs to. We `read` into a 64KB buffer — which matches the kernel's pipe capacity. We record a `DATA_OUT` event for the sending stage, a `DATA_IN` event for the receiving stage, then `write` the data forward to the next stage's stdin. If `read` returns zero, we record a `PIPE_EOF` and close the descriptor.
> 
> This is O(1) per event — no scanning, no polling. Highly efficient even with massive multi-stage pipelines.

*(~30 seconds)*

---

### Slide 17 — Process State Monitoring

> **Ehzoc:** PipeRewind also monitors whether each process is running, sleeping, or blocked. It does this by reading `/proc/[pid]/status` — the kernel's real-time status file. The state character tells us: R for running, S for sleeping, D for disk blocked, Z for zombie.
> 
> In the TUI, this translates to color coding — green means the process is actively running, yellow means it's waiting for something, and red means it exited. If you see a stage stuck on yellow while the others have finished, you immediately know where the bottleneck is happening.

*(~30 seconds)*

---

### Slide 18 — Sliding Window Cache

> **Jesse:** One of the harder engineering challenges: what happens when you have a massive trace? A pipeline processing a large log file could generate hundreds of thousands of events. Loading all of them with their payloads into memory would use hundreds of megabytes.
> 
> Our solution is a sliding window. The TUI keeps only about 20,000 events in memory at a time — roughly 16 MB. When you scroll near the edge, `tui_load_window` frees the old chunk and loads the new one. The index table makes any fseek jump O(1), so sliding is completely instant. You can scrub through a million events and never hit an out-of-memory crash.

*(~35 seconds)*

---

### Slide 19 — Live-Attach Mode

> **Luiz:** Here is our most advanced feature: live mode. Instead of recording first and replaying later, you do both simultaneously. The architecture uses two threads: a background pthread runs `capture_run` — the full epoll engine writing to `trace.prt`. The main thread runs the ncurses TUI.
> 
> They share the trace file on disk. The writer calls `fflush` after every event to ensure bytes are visible. The reader calls `trace_reader_refresh` inside the getch loop, which detects and indexes newly written events. We use `halfdelay(1)` so getch times out every 100 milliseconds, giving us regular refresh opportunities without blocking on input.
> 
> Press `f` for follow mode, and the TUI automatically scrolls to the latest event as it arrives. It's like watching a live feed of your pipeline's internal data flow.

*(~45 seconds)*

---

### Slide 20 — Binary Trace Format

> **Ehzoc:** And here's the .prt format in detail. Fixed 32-byte file header with a magic number for validation. Then N stage headers at 280 bytes each. Then a variable-length stream of events — each with a 24-byte header plus payload. And finally, the index table: one 16-byte entry per event containing the timestamp and the file offset.
> 
> That index table is the core design element that makes this all possible. It turns any random access query into a single `fseek` plus `fread` — no sequential scanning. This is what makes the sliding window and the TUI scrubbing feel instant.

*(~30 seconds)*

---

### Slide 21 — Live Demo

> **Jesse:** And now let's show it running live. We'll start from a clean build.

*(Switch display to a full-screen terminal)*

**Step 1: Build the Project**
```bash
make clean && make
```
> **Jesse:** As you can see, the build is fast. Seven source files compile into a single `piperewind` executable.

**Step 2: Record a simple pipeline**
```bash
./piperewind record -v "echo -e 'banana\napple\ncherry' | sort | head -2"
```
> **Jesse:** We pass the pipeline as a single string. The `-v` flag means verbose, so we see PipeRewind parsing it into 3 distinct stages. The pipeline executes perfectly, and the final output appears exactly as it would normally.

**Step 3: Dump the chronological trace**
```bash
./piperewind dump trace.prt
```
> **Jesse:** Now we dump the binary trace. Here you can see the deterministic timeline. `echo` started, `sort` started... then `echo` wrote 20 bytes of fruit names, which `sort` read. `sort` then wrote the alphabetized list to `head`. It perfectly reconstructs the data flow.

**Step 4: Interactive TUI Replay**
```bash
./piperewind replay trace.prt
```
> **Jesse:** But a text dump isn't interactive. Let's launch the TUI.
> *(Inside TUI)*: As I press `j` and `k`, we scrub back and forth through time. Notice the timeline progress bar moving.
> Notice the color codes: when `sort` is running, it's green. When it finishes, it turns red indicating it exited cleanly.
> *(Inside TUI)*: Now, I'll press `d` to open the Side-by-Side Diff. Look at the data panel: you can visibly see `apple` and `banana` being rearranged from the unsorted output into the sorted input.
> *(Press 'q' to quit)*

**Step 5: Live-Attach Mode**
> **Jesse:** Finally, let's trace a pipeline while it's actively running. We'll use a bash script to generate slow, streaming data — outputting one item every half second into `sort`.
```bash
./piperewind live "bash -c 'for i in 1 2 3 4 5; do echo item_\$i; sleep 0.5; done' | sort -r"
```
> **Jesse:** *(Inside TUI)*: I'll press `f` to enable Follow Mode. As the bash script generates data in real-time, our background thread captures it, and the interface automatically ticks forward.
> Here's the best part: look closely at the `sort` process. It is sitting in the SLEEPING state (yellow). `piperewind` knows it's blocked, waiting for the EOF from bash before it can reverse-sort the data!

*(~3–4 minutes for demo, pace carefully to let the audience absorb the visuals)*

---

### Slide 22 — Thank You

> **Luiz:** That's PipeRewind — a systems-level time-travel debugger built entirely in C, hitting all the major themes from this class. Thank you. We're happy to take questions.

*(~10 seconds)*

---

## SPEAKING TIPS

1. **Practice the demo.** Run through the terminal commands at least 3 times before presenting. Know exactly what the output will look like.
2. **Smooth Handoffs.** The transcript is dynamically divided between Jesse, Luiz, and Ehzoc, giving everyone around 7 slides. Make sure to nod reading cues to one another. 
3. **Speak to the audience, not the screen.** Glance at the slide briefly, then face forward.
4. **Use your hands to point** at diagrams and code on the slide when referencing specific parts.
5. **Pace yourself.** If the demo goes fast, fill time by explaining what you see in the dump output line by line.
6. **If something breaks in the demo**, don't panic. You have the dump output on slide 13 as a backup.
7. **Time check:** After Part 2 (slide 9) you should be at roughly 4 minutes. If you're ahead, spend more time on the demo. If behind, skip the `/proc` slide or the binary format slide.
