# TAVRN local-expiry full-table proof

Board A ran the FULL `FAST_TEST` hooks-only expiry benchmark for 327 seconds.
The machine-derived capture records 131 complete 16-slot sweeps, including 122
hard-expired/demand-deferred sweeps, with a maximum scheduler-return gap of
2 ms and zero faults, unavailable outcomes, targeted controls, verification
RREQs, or expiry-driven TC controls.

The capture JSON is derived from `board-A-330s.log`. Its provenance is bound to
the accompanying artifact manifest, generated build-config manifest, and
selected-source inventory. The test artifact itself is intentionally omitted;
the manifest records its ELF hash.
