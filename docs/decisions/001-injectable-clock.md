# ADR 001: Injectable Clock for Testing

## Status
Accepted

## Context
Original Clock implementation directly called steady_clock::now(), making time-dependent tests difficult.

## Decision
Introduce ClockInterface abstraction:
- RealClock: production implementation
- MockClock: test implementation with controllable time
- Clock::set_clock(): inject custom clock

## Consequences
- Tests can control time progression
- Slight runtime overhead from virtual dispatch
- Better testability for timeout and timer logic
