# msxiv

`msxiv` is a lightweight, Zathura-like image viewer inspired by `sxiv`, using **ImageMagick** as a backend.
It should support all formats that imagemagick `display` supports.

*I generated most of the codebase with o1. Use with caution. Hillarity may ensue.*

## Building

### Requirements

- **X11** development libraries (`libX11`, `libXext`, `libXfixes`)
- **ImageMagick** development headers
- **CMake** and **make**

### NixOS

```sh
nix develop
cmake -B build
make -C build
```

### Generic Linux

```sh
cmake -B build
make -C build
sudo make -C build install
```

This installs `msxiv` to `/usr/local/bin/msxiv`.

---

## Usage

```sh
msxiv image1.jpg image2.png ...
```

- **Next Image**: `Space`
- **Previous Image**: `Backspace`
- **Gallery Mode**: `Enter`
- **Zoom In/Out**: `+` / `-` or `Ctrl + Mouse Wheel`
- **Pan**: `WASD` or Arrow Keys
- **Fit-to-Window**: `=`
- **Command Mode**: `:` (e.g., `:save ~/output.png`)
- **Quit**: `q`

---

## Configuration

Configuration is loaded from:

```
~/.config/msxiv/config.toml
```

### Example

```toml
bg_color = "#000000"   # Background color
keybindings = {
    quit = "q",
    next = "space",
    prev = "backspace",
    zoom_in = "+",
    zoom_out = "-",
    fit_window = "=",
    gallery = "enter",
    command = ":",
}
bookmarks = {
    "wallpapers" = "~/Pictures/Wallpapers",
    "saved" = "~/Pictures/Saved"
}
```

### Keybindings

- You can remap keys to commands or external shell commands.
- Use `exec <command>` to run a shell command, with `%s` as a placeholder for the current image filename.

Example:
```toml
keybindings = {
    delete = "d",
    custom = "exec mv %s ~/Trash"
}
```

## Commands

### Command Mode (`:`)

- `:save <path>`  
  Save the image to the specified path. If `<path>` is a directory, save it there with the original filename.

- `:convert <target>`  
  Convert and save the image in the specified format. E.g., `:convert output.jpg`

- `:delete`  
  Delete the current image.

- `:bookmark <label>`  
  Save the image to a directory defined in `config.toml` under `[bookmarks]`.

## Gallery Mode

Press **Enter** to open the gallery when viewing multiple images. 

- **Navigate**: Arrow Keys
- **Open Selected**: `Enter`
- **Exit Gallery**: `Escape`
