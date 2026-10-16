# Managed callback counter checks

This standalone compiled test links the bridge's internal numeric helper. It
checks canonical counters, generation deltas, thread/counter regressions,
saturation, actual held allocations, thrown attempts, read-only serialization
and the warmed callback path's owner-thread allocations. It never forces GC.

Run the built executable with the matching .NET10 host. Building requires an
existing .NET10 SDK; there is no game SDK or third-party package dependency.
Native hosting, callback validation, gameplay rollback, reload negotiation and
actual engine reads need their separate CoreCLR workflow checks. These tests
do not establish recording overhead or exclusive ownership of GC collections.
