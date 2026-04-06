# collab-editor

A collaborative terminal text editor. Multiple clients connect to a shared server and edit a single document in real time. Every keystroke is propagated to all connected clients within one round trip.

## Architecture

```
 client A          client B
    │                  │
    │    TCP (epoll)    │
    └──────┬────────────┘
           │
        server
           │
     SharedDoc (mmap)
     pthread_rwlock_t
           │
      document.txt  ← auto-save thread (every 30 s)
```

**Server** (`server.cpp`)
- `epoll`-based event loop — handles accept and per-client I/O without a thread per connection
- Thread pool (8 workers) processes client events off the epoll thread
- Document stored in a POSIX shared memory segment (`/collab_editor_doc`) so a local UI process can map it directly via `shm_open_doc()`
- `pthread_rwlock_t` with `PTHREAD_PROCESS_SHARED`: many readers (auto-save, new-client sync) acquire simultaneously; writers hold the exclusive lock only for the duration of `apply_command` + `doc->set()`
- Auto-save thread wakes every 30 s via `condition_variable::wait_for`; writes atomically through a `.tmp` file then `rename()`
- Broadcast uses fine-grained locking: collect fd list under one brief lock, then re-lock per send so a slow client does not block new connections

**Client** (`client.cpp`)
- Raw terminal mode via `termios`; restored on exit through `atexit`
- `poll()` on stdin with 50 ms timeout — screen refreshes when the server sends new content even without a keypress
- Receiver thread uses `poll()` with 200 ms timeout instead of busy-spinning on `WouldBlock`
- Cursor tracked locally as `(row, col)`; each edit sends a positional command to the server and the cursor advances optimistically

## Wire Protocol

Commands are newline-terminated ASCII strings sent from client to server:

| Command | Meaning |
|---------|---------|
| `I:<offset>:<char>\n` | Insert `char` at byte `offset` |
| `D:<offset>\n` | Delete byte at `offset` |

After each command the server broadcasts the full document to all connected clients.

## Build

```bash
cd Network
make          # builds server and client
make test     # builds and runs the unit test suite
make clean    # removes binaries, object files, and autosave artifacts
```

Requires a C++17 compiler and Linux (epoll, POSIX shared memory).

## Run

```bash
# Terminal 1 — start the server
./server

# Terminal 2+ — connect a client
./client
```

`Ctrl-Q` exits the client. The server runs until killed; `document.txt` is written every 30 seconds.

## Shared Memory

The document buffer is a `SharedDoc` struct mapped into every process that calls `shm_open_doc()`. A separate UI process can read the live document without going through the TCP socket:

```cpp
SharedDoc* doc = shm_open_doc();   // map existing segment
{
    RdLock rd(doc->rwlock);        // shared read lock
    std::string text = doc->get();
}
shm_close(doc);                    // unmap (does not destroy)
```

The server owns the segment and calls `shm_destroy()` on exit.

## Testing

Unit tests live in `test_main.cpp` and cover all pure logic — no network or shared memory required:

| Area | What is tested |
|------|----------------|
| `apply_command` | insert/delete, bounds clamping, malformed commands |
| `split_lines` | empty input, single/multi-line, trailing newline, blank lines |
| `cursor_to_offset` | origin, mid-line, cross-line positions |
| `clamp_col` | column/row overflow and underflow |
| `SharedDoc::set/get` | basic round-trip, empty string, overwrite |

The pure functions are declared in `apply_cmd.hpp` and `editor_logic.hpp` so they can be included by both the application and the test binary without duplicating code.

## Key Files

| File | Role |
|------|------|
| `server.cpp` | epoll server, thread pool, auto-save, broadcast |
| `client.cpp` | terminal UI, cursor, key handling, send/recv |
| `shared_doc.hpp` | `SharedDoc`, `RdLock`/`WrLock`, `shm_create`/`shm_open_doc` |
| `socket.hpp/cpp` | `SimpleNet::Socket` wrapper, `set_nonblocking()` |
| `apply_cmd.hpp` | `apply_command` — OT command parser (server + tests) |
| `editor_logic.hpp` | `Cursor`, `split_lines`, `cursor_to_offset`, `clamp_col` (client + tests) |
| `test_main.cpp` | unit test suite (70 tests, no external framework) |
| `Makefile` | incremental build with correct per-target header deps |
