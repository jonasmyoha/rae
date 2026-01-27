# Rae Tetris 2D

A minimal Tetris implementation in Rae using the Raylib library for rendering and input.

## Features
- Game loop with gravity and basic scoring.
- Piece movement and pausing.
- Uses `enum` for game states and tetromino kinds.
- Uses `List(Int)` for board representation.

## How to Run

### Via VM (Live Mode)
```bash
./rae/compiler/bin/rae run rae/examples/94_tetris2d/main.rae
```

### Via C Backend (Compiled Mode)
Use the Rae DevTools dashboard or the `rae build` command.
