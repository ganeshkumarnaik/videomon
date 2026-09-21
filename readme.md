# videomon

`videomon` is a Linux utility that watches process file descriptors for `/dev/video0` and sends Telegram notifications when the webcam becomes active, when the stream ends, and periodically while the camera stays active.

## Current implementation details

This project currently behaves as follows:

- It scans `/proc` entries and inspects each process's `/proc/<pid>/fd` directory.
- If a file descriptor is a symlink to `/dev/video0`, that process is treated as active.
- It sends notifications through the Telegram Bot API using libcurl.
- It reloads configuration on `SIGHUP`.
- It watches only `/dev/video0`; there is no device selection configuration yet.

## Requirements

- Linux with a `/proc` filesystem
- CMake 3.16 or newer
- C++17 compiler
- libcurl development files
- Telegram bot token and chat ID

On Linux, install the build dependencies with:

```sh
sudo apt install build-essential cmake libcurl4-openssl-dev
```

## Build

Build from the repository root:

```sh
cmake -S . -B build
cmake --build build
```

The executable is created at:

```text
build/bin/videomon
```

## Configuration

The application reads `config.txt` at startup. In the current code, placeholder is mentioned in `src/main.cpp` as:

```cpp
/path/to/projects/videomon/src/config.txt
```


The file format is:

```ini
cooldown_seconds=60
check_interval_seconds=1
bot_token=<YOUR_BOT_TOKEN>
chat_id=<YOUR_CHAT_ID>
```

- `cooldown_seconds`: minimum interval between repeated "camera active" alerts.
- `check_interval_seconds`: sleep interval between `/proc` scans.
- `bot_token`: Telegram bot token used by the Bot API.
- `chat_id`: Telegram chat or user ID that receives alerts.

Keep the file private. Do not commit real credentials to version control. A typical setup is:

```sh
chmod 600 src/config.txt
```

## Running

Start it directly from the project root:

```sh
./build/bin/videomon
```

The process must be able to read `/proc/<pid>/fd` entries. Depending on the system configuration, this may require running with appropriate privileges.

After editing the configuration file, reload it with `SIGHUP`:

```sh
kill -HUP <videomon-pid>
```

## systemd service

A sample service file is provided at `systemd/videomon.service`. It currently expects placeholders such as:

- `WorkingDirectory=/path/to/projects/videomon`
- `ExecStart=/path/to/projects/videomon/build/bin/videomon`
- `User=root`

Copy it to `/etc/systemd/system/`, then reload and start it:

```sh
sudo cp systemd/videomon.service /etc/systemd/system/videomon.service
sudo systemctl daemon-reload
sudo systemctl enable --now videomon.service
```

Check status and logs with:

```sh
sudo systemctl status videomon.service
sudo journalctl -u videomon.service -f
```

If your repo path differs, edit the `WorkingDirectory` and `ExecStart` values before enabling the service.

## Detection logic

On each loop, the program does the following:

1. Iterates through `/proc` entries.
2. Checks each process's `/fd` directory.
3. Looks for symlinks whose target is exactly `/dev/video0`.
4. Treats any matching process as an active camera stream.

## Notes

- Telegram alerts are dispatched asynchronously.
- The native implementation uses libcurl rather than the shell helper shown in older code paths.
- The app is intentionally scoped to `/dev/video0` and does not yet support multiple camera devices or runtime device selection.
