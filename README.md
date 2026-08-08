# slaptrack - OpenLDAP Log Viewer

An interactive terminal-based log viewer for OpenLDAP (slapd) logs with colorization, filtering, and real-time follow mode.

## Features

- **Colorized display** - Different log components are color-coded for easy reading
- **Compressed logs** - Transparent support for `.gz`, `.bz2`, `.xz` (auto-detected by magic bytes)
- **Interactive filtering** - Click on connection IDs, thread IDs, DNs, or other tokens to filter
- **Filter chaining** - Apply multiple filters in sequence (e.g., filter by conn, then by dn)
- **Token-aware navigation** - Cursor highlights entire token, press `/` to search for current token
- **Follow mode** - Real-time monitoring like `tail -f` with automatic scroll
- **Line numbers** - Toggle display with `#` key
- **Line wrapping** - Toggle with `W` for long lines
- **Mouse support** - Scroll wheel navigation, click to move cursor, double-click to filter
- **Search** - Full-text search with next/previous navigation
- **Vim-like navigation** - Supports hjkl, g/G, and other vim shortcuts
- **Read-only** - Safe for viewing production logs
- **Streaming architecture** - Handles huge files efficiently with on-demand parsing

## Building

```bash
make clean && make
```

## Usage

```bash
./build/slaptrack [-f] [--log-format <fmt>] <logfile>
```

### Options

- `-f` - Follow mode (like `tail -f`), automatically shows new lines as they're written
- `--log-format <fmt>` - Input log format: `auto`, `debug`, `syslog-utc`, `syslog-local`, `rfc3339` (default: `auto`)
  - `auto` - Per-line detection: decodes the debug hex epoch prefix **and** syslog timestamps (as UTC), leaves the rest untouched
  - `debug` - OpenLDAP debug/access log: `<sec>.<frac> <thread-id> ...` (hex epoch) decoded to RFC3339 UTC
  - `syslog-utc` - `%b %d %H:%M:%S` syslog timestamps decoded as UTC
  - `syslog-local` - same, but decoded as local time
  - `rfc3339` - no conversion (the default for existing logs)

### Examples

```bash
# View a log file
./build/slaptrack logs/slapd_2.6.log

# Follow a log file in real-time
./build/slaptrack -f logs/slapd_2.6.log

# Decode an epoch-prefixed debug log (olcLogFileFormat debug output)
./build/slaptrack --log-format debug logs/slapd_debug.log

# Decode syslog-format access log as UTC on stdin
tail -f /var/log/slapd/access.log | ./build/slaptrack - --log-format syslog-utc
```

## Controls

### Navigation

| Key | Action |
|-----|--------|
| `↑/↓` or `j/k` | Move cursor up/down |
| `←/→` or `h/l` | Move cursor left/right |
| `Page Up/Down` | Scroll by page |
| `Home/End` or `g/G` | Go to top/bottom |
| Mouse scroll | Scroll up/down (3 lines per tick) |
| Mouse click | Move cursor to position |

### Filtering

| Key | Action |
|-----|--------|
| `Enter` | Filter by token under cursor |
| `Esc` or `Backspace` | Remove last filter (undo) |
| `←` (at leftmost) | Remove last filter |
| `h` (at leftmost) | Remove last filter |
| Mouse double-click | Filter by token at position |

### Search

| Key | Action |
|-----|--------|
| `/` | Search (pre-filled with current token) |
| `n` | Next search result |
| `N` | Previous search result |

### Display

| Key | Action |
|-----|--------|
| `#` | Toggle line numbers |
| `q` | Quit |

## Color Legend

| Color | Component |
|-------|-----------|
| Cyan | Timestamp |
| Orange | Connection ID (conn=) |
| Purple | Operation ID (op=) |
| Magenta | Thread ID (0x...) |
| Green | Distinguished Name (dn=) |
| Yellow | Filter expression |
| Blue | IP Address |
| Red | Error code |
| Blue (bold) | Keywords (BIND, SRCH, RESULT, etc.) |

## Workflow Examples

### Basic Navigation
1. Open a log file: `./build/slaptrack logs/slapd_2.6.log`
2. Navigate with arrow keys or `hjkl`
3. Press `#` to show line numbers
4. Press `g` or `G` to jump to top/bottom

### Filtering by Connection
1. Navigate to a line with `conn=12345`
2. Move cursor onto the connection ID token
3. Press `Enter` to filter - now only showing that connection
4. Press `Esc` or `Backspace` to remove the filter

### Filtering by Thread
1. Navigate to a line with a thread ID token (`0x...`, magenta, at the start of debug-format lines)
2. Move cursor onto the thread ID token
3. Press `Enter` to filter - now only showing lines from that slapd thread
4. Press `Esc` or `Backspace` to remove the filter

### Chained Filtering
1. Filter by connection ID (conn=12345)
2. Navigate to a line with `dn="uid=user,dc=example"`
3. Press `Enter` to add another filter
4. Now showing only that connection AND that DN
5. Press `Esc` twice to remove both filters

### Real-time Monitoring
1. Open with follow mode: `./build/slaptrack -f logs/slapd_2.6.log`
2. Automatically scrolls to bottom and shows new lines
3. Navigate up to review history (disables auto-scroll)
4. Press `G` to jump to bottom and re-enable auto-scroll

### Token-based Search
1. Navigate to any token (conn, dn, op, etc.)
2. Press `/` - search is pre-filled with that token's value
3. Press `Enter` to search
4. Use `n`/`N` to navigate results

## Architecture

### Streaming Design
- **LogIndex** - Builds byte-offset index for fast random access (~11MB for 1.37M lines)
- **LogBuffer** - Keeps only ~2000 parsed lines in memory around current view
- **On-demand parsing** - Lines are parsed only when needed
- **Prefetching** - Loads lines ahead as you scroll

### Performance
- **Startup**: ~0.15s for 214MB file (index build)
- **Memory**: ~25MB vs loading entire file into memory
- **Scrolling**: Instant, lines parsed on-demand
- **Follow mode**: Uses inotify for efficient file monitoring

## Requirements

- C++17 compiler (GCC 8+, Clang 5+)
- Linux (uses inotify for follow mode)
- ncurses development headers
- zlib, bzip2, and xz/lzma development libraries (for compressed log support)

### Install dependencies per distribution

| Distribution | Command |
|--------------|---------|
| Fedora / RHEL 9+ | `sudo dnf install gcc-c++ ncurses-devel zlib-devel bzip2-devel xz-devel` |
| RHEL 8 (EPEL) | `sudo dnf install gcc-toolset-12-gcc-c++ ncurses-devel zlib-devel bzip2-devel xz-devel` |
| Debian / Ubuntu | `sudo apt install g++ libncurses-dev zlib1g-dev libbz2-dev liblzma-dev` |
| Arch / Manjaro | `sudo pacman -S gcc ncurses zlib bzip2 xz` |
| openSUSE Leap/Tumbleweed | `sudo zypper install gcc-c++ ncurses-devel zlib-devel libbz2-devel xz-devel` |
| Alpine | `sudo apk add g++ ncurses-dev zlib-dev bzip2-dev xz-dev` |

## Project Structure

```
slaptrack/
├── Makefile
├── src/
│   ├── main.cpp            # Entry point, argument parsing, compression dispatch
│   ├── compressed_io.h/cpp # Magic-byte detection + gzip/bzip2/xz decompression
│   ├── log_parser.h/cpp    # Log parsing and tokenization
│   ├── log_index.h/cpp     # File indexing for random access
│   ├── log_buffer.h/cpp    # Rolling window buffer
│   ├── viewer.h/cpp        # TUI rendering and interaction
│   └── filter.h/cpp        # Filter management
└── logs/                   # Sample log files
```

## Compressed Log Handling

When given a file, `slaptrack` reads the first bytes to detect the compression
format rather than trusting the filename extension.  The supported formats are:

| Magic bytes | Format |
|-------------|--------|
| `1F 8B` | gzip (`.gz`) |
| `42 5A 68` (`BZh[1-9]`) | bzip2 (`.bz2`) |
| `FD 37 7A 58 5A 00` | xz (`.xz`) |

On detection, the file is fully decompressed into a uniquely-named temp file
under `/tmp` (via `mkstemp(3)`), the TUI is built on that file, and the temp
file is removed on exit (including on `SIGINT` / `SIGTERM` via `atexit(3)`).

## License

AGPL-3.0
