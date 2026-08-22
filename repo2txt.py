#!/usr/bin/env python
"""
repo2txt  —  flatten a source-code tree into one text file for AI analysis.
Public-domain / CC0.
"""
from __future__ import annotations

import argparse
import sys
import traceback  # For detailed error printing
from pathlib import Path

# ─────────────── Configuration ──────────────────────────────────────────
# Tailor these settings to your project to get the most relevant context.

# Max file size to include (in bytes)
MAX_FILE_SIZE = 5 * 1024 * 1024  # 5 MiB

# File extensions to include. Headers are crucial for C++ analysis.
TEXT_EXTS = {
    # C++ Source
    ".c", ".cpp", ".h", ".hpp",
    # Qt Files
    ".ui", ".qrc", ".pro", ".pri",
    # Build System & Scripts
    ".cmake", "CMakeLists.txt", ".sh", ".bat", ".py",
    # Documentation & Config
    ".md", ".txt", ".json", ".xml", ".yml", ".yaml", ".iss",
    # Shaders
    ".vert", ".frag", ".geom"
}

# --- Exclusion Rules ---

# 1. Directory NAMES to completely skip, wherever they appear.
# Most effective for build artifacts, IDE configs, etc.
SKIP_DIR_NAMES = {
    ".git", ".idea", ".vs", ".claude",  # VCS and IDE configs
    "build", "out", "Output",          # Common build output directory names
    "__pycache__", "node_modules",      # Python/JS cache and dependencies
    "images",                          # Often contains binary assets
}

# 2. Directory PATHS to completely skip, relative to the project root.
# Use this for specific project structures, like excluding all third-party code.
SKIP_DIR_PATHS = {
    "src/3rdparty",                    # <-- THIS IS THE KEY FIX
    "installer",
    "doc",
    "requirements",
    "tests",
}


# 3. Specific file or directory PATTERNS to skip using glob syntax.
# Useful for selectively excluding files within an otherwise included directory.
# Patterns are matched against paths relative to the root.
SKIP_PATTERNS = [
    "*.user",                          # Qt/VS user settings files
    "*.log",                           # Log files
    "*.sln", "slnx.sqlite", "*.suo",    # Visual Studio solution files
    "*.backup", "*.bak",               # Backup files
    "*.in",                            # CMake template files
    # Example of more specific pattern:
    # "src/some_lib/experimental/*"
]

# 4. Binary file extensions to always exclude.
BINARY_EXTS = {
    ".exe", ".dll", ".so", ".a", ".lib", ".o", ".obj",
    ".png", ".jpg", ".jpeg", ".gif", ".bmp", ".ico", ".svg", ".odg",
    ".ttf", ".otf", ".woff", ".woff2",
    ".zip", ".7z", ".rar", ".jar", ".pdf",
    ".db", ".sqlite", ".vsidx", ".ipch", ".bin",
    ".qm" # Qt translations binary
}

# ─────────────── Script Logic ───────────────────────────────────────────

def is_skipped(path: Path, root: Path) -> bool:
    """
    Check if a path should be skipped based on the configuration rules.
    """
    # Rule 1: Check against directory NAMES anywhere in the path
    # This is a fast and effective way to prune common unwanted dirs.
    if any(part in SKIP_DIR_NAMES for part in path.parts):
        return True

    try:
        # For path and pattern rules, we need the path relative to the root.
        relative_path = path.relative_to(root)
        # Normalize to forward slashes for consistent matching
        relative_path_str = str(relative_path).replace("\\", "/")
    except ValueError:
        # This can happen if `path` is not within `root`, though it's unlikely.
        # We can safely decide not to skip it.
        return False

    # Rule 2: Check against directory PATHS relative to the root.
    # This handles cases like "src/3rdparty".
    if any(relative_path_str == p or relative_path_str.startswith(p + '/')
           for p in SKIP_DIR_PATHS):
        return True

    # Rule 3: Check against glob PATTERNS relative to the root.
    if any(relative_path.match(pattern) for pattern in SKIP_PATTERNS):
        return True

    return False


def is_text_file(path: Path) -> bool:
    """Check if a file should be treated as text and included."""
    if path.stat().st_size > MAX_FILE_SIZE:
        return False

    suf = path.suffix.lower()
    if suf in BINARY_EXTS:
        return False
    # Special case for files without an extension like 'CMakeLists.txt'
    if suf not in TEXT_EXTS and path.name not in TEXT_EXTS:
        return False

    try:
        # A simple heuristic to detect binary files is to check for null bytes.
        with path.open("rb") as f:
            return b"\0" not in f.read(8192)
    except OSError:
        return False # File cannot be read

def read_file(path: Path) -> str:
    """Read a file with fallback encoding."""
    try:
        return path.read_text("utf-8")
    except UnicodeDecodeError:
        try:
            return path.read_text("latin-1", errors="replace")
        except Exception as e:
            return f"[repo2txt: Could not decode file {path.name}: {e}]\n"
    except OSError as e:
        return f"[repo2txt: OSError reading file {path.name}: {e}]\n"

def build_tree(root: Path) -> list[str]:
    """Build a visual tree of the relevant directory structure."""
    lines = ["└── ./"]

    def walk(dir_path: Path, prefix=""):
        try:
            entries = sorted(dir_path.iterdir(), key=lambda p: (p.is_file(), p.name.lower()))
        except OSError as e:
            lines.append(f"{prefix}└── [Error reading directory: {e}]")
            return

        visible_entries = [e for e in entries if not is_skipped(e, root)]

        for i, entry in enumerate(visible_entries):
            is_last = (i == len(visible_entries) - 1)
            connector = "└── " if is_last else "├── "
            lines.append(f"{prefix}{connector}{entry.name}")

            if entry.is_dir():
                new_prefix = prefix + ("    " if is_last else "│   ")
                walk(entry, new_prefix)

    walk(root)
    return lines

def dump_repo(root: Path, out_stream):
    """Dump the repository structure and relevant file contents to a stream."""
    out_stream.write("Directory Structure:\n\n")
    out_stream.write("\n".join(build_tree(root)) + "\n\n")

    def walk_for_files(current_path: Path):
        try:
            for entry in sorted(current_path.iterdir(), key=lambda p: (p.is_file(), p.name.lower())):
                if is_skipped(entry, root):
                    continue
                if entry.is_dir():
                    yield from walk_for_files(entry)
                elif entry.is_file() and is_text_file(entry):
                    yield entry
        except OSError as e:
            print(f"[Warning: Could not read directory {current_path}: {e}]", file=sys.stderr)

    for path in walk_for_files(root):
        try:
            rel_path_str = "/" + str(path.relative_to(root)).replace("\\", "/")
            out_stream.write(f"---\nFile: {rel_path_str}\n---\n\n")
            text = read_file(path)
            out_stream.write(text if text.endswith("\n") else text + "\n")
            out_stream.write("\n")
        except Exception as e:
            out_stream.write(f"[repo2txt: Unexpected error processing file {path}: {e}]\n\n")
            traceback.print_exc(file=sys.stderr)

# ─────────────── Main Execution ───────────────────────────────────────────

def main(argv: list[str] | None = None):
    """Main function to parse arguments and run the script."""
    ap = argparse.ArgumentParser(
        description="Flatten a source-code tree into one text file, configured for AI analysis.",
        formatter_class=argparse.ArgumentDefaultsHelpFormatter
    )
    ap.add_argument(
        "folder",
        nargs='?',
        default='.',
        help="Root folder to process. Defaults to the current working directory."
    )
    ap.add_argument(
        "-o", "--output",
        metavar="FILE",
        default='-',
        help="Output .txt file (use '-' for stdout, which is the default)."
    )
    args = ap.parse_args(argv)

    root = Path(args.folder).expanduser().resolve()
    if not root.is_dir():
        print(f"Error: Folder '{root}' is not a valid directory.", file=sys.stderr)
        sys.exit(1)

    if args.output == "-":
        dump_repo(root, sys.stdout)
    else:
        out_path = Path(args.output).expanduser().resolve()
        try:
            out_path.parent.mkdir(parents=True, exist_ok=True)
            with out_path.open("w", encoding="utf-8", newline="\n") as f:
                dump_repo(root, f)
            print(f"Wrote repository contents to {out_path}")
        except OSError as e:
            print(f"Error writing to output file '{out_path}': {e}", file=sys.stderr)
            sys.exit(1)

if __name__ == "__main__":
    main()