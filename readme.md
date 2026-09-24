# videomon

`videomon` is a Linux utility designed to detect when a configured camera device is accessed and notify the user through Telegram. It is intended for monitoring camera activity on a home server or unattended Linux environment where access to a video stream needs to be observed without manual checking.

## Current implementation details

The current implementation behaves as follows:

- It reads `/proc` entries and examines each process's `/proc/<pid>/fd` directory.
- It compares symlinks against the device paths defined in the TOML configuration.
- It sends alerts through the Telegram Bot API using libcurl.
- It reloads the configuration when it receives `SIGHUP`.
- The configuration includes a `devices.monitor` list, but the active detection loop currently evaluates only the first item in that list.

## Requirements

- Linux with a `/proc` filesystem
- CMake 3.16 or newer
- C++17 compiler
- libcurl development files
- Telegram bot token and chat ID
- TOML++ header file

On Linux, install the build dependencies with:

```sh
sudo apt install build-essential cmake libcurl4-openssl-dev
```

The project also depends on the TOML++ header library. Download the upstream TOML++ release or single-header package and place it in the project include path, or adjust the include path in your build environment accordingly.

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

The application reads a TOML configuration file at startup. In the current build, the path is set with `VIDEOMON_CONFIG_PATH` and points to:

```text
src/config.toml
```

A sample configuration file, config_example.toml, is included for reference.

Example configuration:

```ini
cooldown_seconds = 60
check_interval_seconds = 1

[devices]
monitor = ["/dev/video0"]

[telegram]
bot_token = "<YOUR_BOT_TOKEN>"
chat_id = "<YOUR_CHAT_ID>"
```

- `cooldown_seconds`: minimum interval between repeated "camera active" alerts.
- `check_interval_seconds`: interval between `/proc` scans.
- `bot_token`: Telegram bot token used by the Bot API.
- `chat_id`: Telegram chat or user ID that receives notifications.
- `monitor`: device paths that the application watches.

Keep the file private. Do not commit real credentials to version control.

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

On each loop iteration, the program performs the following actions:

1. Iterates through `/proc` entries.
2. Inspects each process's `/fd` directory.
3. Checks for symlinks that point to the first configured monitor path.
4. Treats a matching process as an active camera stream.

## Notes

- Telegram alerts are dispatched asynchronously.
- The application uses libcurl for Bot API requests.
- At present, the detection loop checks only the first item in `devices.monitor`; additional entries are parsed but not used in the active scan.
