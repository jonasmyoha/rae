# Rae Language Goals & Philosophy

## 1. Core Philosophy
Rae is built for developers who want the performance and control of C with the safety and ergonomics of modern functional languages. It is designed for **Creative Systems**: applications that require high performance, predictable latency, and live iteration.

## 2. Key Principles

### Beginner-Friendly, Not a Toy
Rae should be easy to learn for anyone coming from Python, Swift, or C. However, it does not hide complexity where it matters. Explicit references (`view`/`mod`) teach users about ownership without the steep learning curve of a borrow checker.

### Games & Tools First
Every language feature is evaluated against two use cases:
1. **Can I build a high-performance 60 FPS game with this?**
2. **Can I build a reliable CLI tool or compiler with this?**

If a feature adds overhead that hurts games or complexity that slows down tool development, it is reconsidered.

### Deterministic & Strict
- **Strict Formatting:** Rae includes a built-in formatter (`rae format`). Projects should look identical across different authors.
- **Deterministic Builds:** No hidden state or ambient environment variables should affect the output of the C backend.

### First-Class C Interoperability
C is the world's "lingua franca" for systems programming. Rae aims to consume C libraries effortlessly. The C backend is not just an implementation detail; it is a primary distribution target that allows Rae code to run anywhere C does.

### Safety without a GC
Rae avoids Garbage Collection to ensure predictable "stop-the-world" free execution. It uses a combination of scoped lifetimes, explicit references, and (future) resource cleanup hooks to maintain memory safety without the runtime cost of a GC.

## 3. live Iteration
Hot-reloading is a core pillar. The ability to swap logic in a running VM while preserving state is essential for creative workflows. Rae is designed from the ground up to support this "live" experience.
