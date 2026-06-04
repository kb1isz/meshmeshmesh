# AGENTS.md — T-Deck LoRa Messenger

> Guidelines for AI agents working on this firmware project.

---

## Compilation

- **Always run `pio run` to compile after making any code changes.**
- Do not mark a task complete until `pio run` exits with code 0.
- If compilation fails, fix the errors and re-run `pio run` before proceeding.

## General rules
- All code/calls should be documented.
- write code non blocking where at all possible.
- Secure code is not optional.