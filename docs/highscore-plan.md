# Highscore System Plan for Rae Tetris 2D

## Goal
To implement a persistent highscore system for Tetris 2D, starting with local file storage, progressing to a web-based client-server model, and potentially implementing the server itself in Rae.

---

## 1. Local File-Based Highscores
**Goal:** Persist a list of highscores (Name, Score, Level) to a local file (e.g., `highscores.rae` or `scores.json`).

*   **Feasibility:** **100% (High)**. We already have basic file I/O hooks. We just need to expose them properly in the `sys` or `io` module.
*   **Time Estimate:** ~1-2 hours.
*   **Robustness:** High. Simple disk operations are predictable.
*   **API Complexity:** Low. 
    *   `sys.readFile(path: String) -> String`
    *   `sys.writeFile(path: String, content: String) -> Bool`
*   **Language Impact:** Minimal. Requires standardizing the `sys` module across VM and C backend.

---

## 2. Web-Based Highscores (Client)
**Goal:** Send highscores to a remote server and fetch the Top 10 leaderboard via HTTP.

*   **Feasibility:** **85% (Medium-High)**. Requires adding a networking dependency to the Rae runtime (e.g., `libcurl` or a lightweight C header like `mongoose` or `http.h`).
*   **Time Estimate:** ~3-5 hours.
*   **Robustness:** Medium. Depends on network stability and server availability.
*   **API Complexity:** Medium.
    *   `http.get(url: String) -> String`
    *   `http.post(url: String, body: String) -> String`
*   **Language Impact:** Significant. Requires linking external libraries during the `rae build` process and ensuring the VM can also load these symbols.

---

## 3. Web Server in Rae (Stretch Goal)
**Goal:** Write a backend in Rae that listens on a port (e.g., localhost:3000), accepts TCP connections, parses HTTP, and manages a score database.

*   **Feasibility:** **40% (Low-Medium)**. Rae is currently optimized for games/apps. A server requires robust socket listening and multi-threading/concurrency which are currently experimental or missing in the VM.
*   **Time Estimate:** ~1-2 days.
*   **Robustness:** Low (initially). Writing a reliable HTTP parser and socket handler from scratch in a young language is error-prone.
*   **API Complexity:** High.
    *   `net.listen(port: Int, callback: (connection: Socket))`
    *   Requires parsing raw strings into HTTP headers/bodies.
*   **Language Impact:** Massive. Would drive the development of concurrency primitives, better binary buffer handling, and advanced string manipulation.

---

## Missing or "Nice-to-Have" Language Features

Before starting, the following gaps should be addressed:

### A. Crucial for Local/Web Highscores
1.  **JSON Support:** Highscores are almost always stored as JSON. We need a `json.parse(str)` and `json.stringify(obj)` or a way to easily map structs to strings.
2.  **String Utilities:** `String.split(delimiter)`, `String.find(sub)`, and `String.trim()` are essential for parsing custom file formats or HTTP responses.
3.  **Path Handling:** `sys.getAppConfigDir()` to ensure highscores are saved in the correct user folder across macOS/Windows/Linux.

### B. Crucial for the Rae Server (Stretch Goal)
1.  **Binary Buffers:** Better support for `Buffer(U8)` to handle raw network packets without constant string conversions.
2.  **Concurrency:** A more robust `spawn` or `async` system so the server doesn't freeze the terminal while waiting for a request.
3.  **Error Handling:** Improved `Result(T, E)` or `try/catch` logic for network timeouts and file locks.
