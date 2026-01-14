# Advanced Pong (ECS Style)

This example demonstrates an Entity-Component-System (ECS) style architecture in Rae.

## Components
- **Transform**: Stores position (x, y, z).
- **Velocity**: Stores movement vector (dx, dy).
- **Paddle**: Marker component for paddles.
- **Ball**: Marker component for the ball.

## Systems
- **InputSystem**: Handles player input and updates paddle velocity.
- **MovementSystem**: Updates positions based on velocity.
- **CollisionSystem**: Handles ball bouncing and paddle collisions.
- **RenderSystem**: Draws all entities based on their Transform.

## Architecture
Entities are currently handled implicitly via parallel lists of components.
A more robust `Entity` manager can be built as the language matures.
