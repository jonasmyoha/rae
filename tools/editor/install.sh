#!/usr/bin/env bash
# Install Rae syntax highlighting for VSCode and Sublime Text

set -euo pipefail

EDITOR_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
VSCODE_EXT_DIR="$HOME/.vscode/extensions/rae-lang"

# Determine Sublime Text User Packages directory based on OS
if [[ "$OSTYPE" == "darwin"* ]]; then
    SUBLIME_USER_DIR="$HOME/Library/Application Support/Sublime Text/Packages/User"
    if [ ! -d "$(dirname "$SUBLIME_USER_DIR")" ]; then
        SUBLIME_USER_DIR="$HOME/Library/Application Support/Sublime Text 3/Packages/User"
    fi
else
    # Linux paths
    SUBLIME_USER_DIR="$HOME/.config/sublime-text/Packages/User"
    if [ ! -d "$(dirname "$SUBLIME_USER_DIR")" ]; then
        SUBLIME_USER_DIR="$HOME/.config/sublime-text-3/Packages/User"
    fi
fi

echo "Installing Rae editor support..."

# 1. Sublime Text
if [ -d "$(dirname "$SUBLIME_USER_DIR")" ]; then
    mkdir -p "$SUBLIME_USER_DIR"
    cp "$EDITOR_DIR/rae.sublime-syntax" "$SUBLIME_USER_DIR/"
    echo "  - Sublime Text syntax installed to: $SUBLIME_USER_DIR"
else
    echo "  - Sublime Text not found or path different, skipping."
fi

# 2. VSCode
# Check if VSCode is installed (either 'code' command or the extensions dir exists)
if command -v code >/dev/null 2>&1 || [ -d "$HOME/.vscode" ]; then
    mkdir -p "$VSCODE_EXT_DIR"
    cp -R "$EDITOR_DIR/vscode/"* "$VSCODE_EXT_DIR/"
    echo "  - VSCode extension installed to: $VSCODE_EXT_DIR"
else
    echo "  - VSCode not found, skipping."
fi

echo
echo "Installation complete! Please restart your editor to see the changes."
