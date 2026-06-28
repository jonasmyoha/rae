# User brief

- Task type: Feature design
- Max rounds: 3
- Quiet mode: off
- Provider retry budget: 1
- Provider timeout: 1200 ms
- Code apply approval: not approved
- Participants: Chattie, Clo, Gem

## Prompt
What would be the ideal threading or concurrent coding solution for Rae programming language. I assume there are different kinds of concurrency. Like a long running rendering thread, or a single task doing multiple operations in a loop with multiple threads to a single or multiple component tables etc. and then joining once the task is done. - Also there those async await calls that this language doesn’t yet have. I have opinions about async/await syntax (in short: await should not be marked, but the opposite of it, the ”spawn” keyword will spawn an async call. awaited calls are just waited by default, so opposite of what other languages do, because that is more sane.) What would the ideal design document for concurrency have for Rae programming language given the languages C-compiled and bytecode VM version code, and the language style etc.
