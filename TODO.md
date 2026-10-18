# BOX model status — TODO

The CPU model already runs continuously in Docker and refreshes its 48-hour forecast every 10 minutes. Inference takes less than a millisecond on the deployment host; near-zero CPU between refreshes is expected, not evidence of failure. New observations affect inference; continuous retraining is not enabled.

## 1. Publish the deployed TLS compatibility fix

- [ ] Commit the server's existing `max_fragment_size = Some(2048)` change separately from this roadmap.
- [ ] Add a repeatable wire-level regression check: a complete 4096-byte dashboard response, including HTTP headers, must be delivered in TLS records that fit the firmware's 4096-byte plaintext input limit.

**Acceptance:** BOX keeps one connection and polls approximately every five seconds without returning to `WAIT`; peer and certificate validation remain enabled.

## 2. Expose model activity and freshness

- [ ] Extend the engine's local API with explicit model state, last successful forecast time, last attempt result and next scheduled refresh.
- [ ] Measure inference duration separately from provider/network latency; do not display an old benchmark as the current refresh duration.
- [ ] Preserve observation timestamps and distinguish forecast age from source-data age. Represent missing information explicitly.

**Acceptance:** clients can distinguish ready/idle, updating, stale and failed states without inferring them from CPU load or inventing freshness after a failed refresh.

## 3. Relay bounded status through mTLS

- [ ] Carry compact model-status metadata through the existing forecast/dashboard path, without opening another public endpoint.
- [ ] Keep the forecast response within the relay's 3072-byte input bound and the complete device dashboard within 4096 bytes; update boundary tests.
- [ ] Retain compatibility with older responses that omit the new fields. Recheck TLS record fragmentation as well as JSON size.

**Acceptance:** an authenticated dashboard read includes usable model state and 25 forecast points; absent/stale engine data cannot appear freshly generated.

## 4. Show useful model status on BOX

- [ ] Add a compact model indicator, forecast age and time until the next scheduled refresh, without replacing the existing Wi-Fi/dashboard `LIVE` / `WAIT` / `OFF` indicator.
- [ ] Keep host CPU/RAM metrics truthful: an idle but healthy model must not look failed, and CPU usage must not be artificially inflated.
- [ ] Clearly mark cached/stale estimates and unknown scheduling information. Keep graph and page controls responsive.

**Acceptance:** the screen explains that the model is working even at 0% CPU, and indicates a real update or source failure without a permanently spinning activity indicator.

## 5. Test the complete status path

- [ ] Cover ready/idle, in-progress refresh, provider failure, engine unavailability, stale cache and recovery with deterministic tests.
- [ ] Check that polling does not invoke training or provider fetches, and that failed updates do not reset forecast age.
- [ ] Validate native previews, then the real BOX over Wi-Fi: state changes, next-refresh countdown, touch navigation and autonomous operation from a charger.
- [ ] Repeat container crash/recreation checks and confirm that weights and genuine observation history survive.

**Acceptance:** API, relay and screen agree; visual behavior is confirmed on hardware rather than inferred solely from a successful server request.

## 6. Document operation and model limits

- [ ] Document status meanings, freshness thresholds, restart behavior and rollback alongside Docker deployment instructions.
- [ ] Define freshness monitoring separately from the container healthcheck, which currently checks API availability only.
- [ ] Keep adaptation a separate validated operation: require enough genuine history and holdout improvement before promoting weights; do not run state-mutating training jobs concurrently with the live service.

**Acceptance:** operators can distinguish a stopped container, a healthy idle model and an unavailable weather source without exposing private configuration or credentials.
