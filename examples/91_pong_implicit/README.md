# Pong (Data-Oriented GPU2D)

This example demonstrates a small data-oriented game using GPU2D rendering,
SDL3 input, and an ECS-backed `.raescene` overlay.

## Components
- **PaddleTransform**: Stores 2D positions.
- **Velocity**: Stores movement vector (dx, dy).

## Systems
- **paddleInputSystem** reads SDL3 keyboard state.
- **paddleAiSystem** predicts wall-bounce interception at human reaction
  intervals, then moves with acceleration, a dead zone, and imperfect aim.
- **movementSystem** updates positions using delta time.
- **bounceSystem** and **paddleCollisionSystem** implement gameplay.
- GPU2D draws the play field while `.raescene` owns score/help text.

## Architecture
The game stays intentionally small: ordinary component structs and systems are
enough for two paddles and one ball, while the UI uses Rae's real ECS.

Use W/S or the arrow keys to move and R to restart the match.
