# Rae Tetris 2D

A minimal Tetris implementation in Rae using GPU2D rendering, SDL3 input, and
an ECS-backed `.raescene` HUD.

## Features
- Game loop with gravity and basic scoring.
- Piece movement and pausing.
- Uses `enum` for game states and tetromino kinds.
- Uses `List(Int)` for board representation.

## How to Run

```bash
compiler/bin/rae run --target compiled examples/94_tetris2d/main.rae
```
