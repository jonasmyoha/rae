#!/usr/bin/env bash
# Install Rae syntax highlighting for VSCode and Sublime Text

set -euo pipefail

EDITOR_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SUBLIME_USER_DIR="$HOME/Library/Application Support/Sublime Text/Packages/User"
VSCODE_EXT_DIR="$HOME/.vscode/extensions/rae-lang"

echo "Installing Rae editor support..."

# 1. Sublime Text
if [ -d "$(dirname "$SUBLIME_USER_DIR")" ]; then
    mkdir -p "$SUBLIME_USER_DIR"
    cp "$EDITOR_DIR/rae.sublime-syntax" "$SUBLIME_USER_DIR/"
    echo "  - Sublime Text syntax installed to: $SUBLIME_USER_DIR"
else
    echo "  - Sublime Text not found, skipping."
fi

# 2. VSCode
mkdir -p "$VSCODE_EXT_DIR"
cp -R "$EDITOR_DIR/vscode/"* "$VSCODE_EXT_DIR/"
echo "  - VSCode extension installed to: $VSCODE_EXT_DIR"

echo
echo "Installation complete! Please restart your editor to see the changes."
