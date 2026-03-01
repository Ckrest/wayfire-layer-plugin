# wayfire-layer-plugin

Wayfire compositor plugin that pins views to the topmost scene layer. Matched views stay above all other windows, including drag overlays and compositor effects.

## Features

- **Namespace auto-pinning** — automatically pin views by app ID via config
- **Priority ordering** — control stacking order between pinned views
- **IPC control** — pin/unpin views at runtime via `wf-msg`
- **Original parent tracking** — views return to their correct position when unpinned

## Build

Requires Wayfire development headers.

```bash
PKG_CONFIG_PATH=$HOME/.local/wayfire/lib/x86_64-linux-gnu/pkgconfig \
  meson setup build
ninja -C build
ninja -C build install
```

## Configuration

Add to `~/.config/wayfire.ini`:

```ini
[core]
plugins = ... topmost-scene ...

[topmost-scene]
enabled = true
namespaces = desktop-hud
priorities = desktop-hud:100
```

### Options

| Option | Type | Description |
|--------|------|-------------|
| `enabled` | bool | Enable root-scene topmost reordering (default: true) |
| `namespaces` | string | Semicolon-separated app IDs to auto-pin |
| `priorities` | string | Semicolon-separated `namespace:priority` pairs (higher = topmost) |

## IPC Methods

Control the plugin at runtime using `wf-msg`:

| Method | Description |
|--------|-------------|
| `topmost-scene/pin` | Pin a view by ID, with optional priority |
| `topmost-scene/unpin` | Unpin a view by ID |
| `topmost-scene/list` | List all tracked views with their pin state |
| `topmost-scene/reapply` | Force reapply stacking order |

### Examples

```bash
# List pinned views
wf-msg topmost-scene/list

# Pin a specific view with priority
wf-msg topmost-scene/pin -j '{"view_id": 42, "priority": 50}'

# Unpin a view
wf-msg topmost-scene/unpin -j '{"view_id": 42}'
```

## License

MIT
