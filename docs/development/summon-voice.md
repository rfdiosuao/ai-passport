[简体中文](summon-voice.zh_CN.md)

# SUMMON USB voice application

## Wi-Fi transport increment

Long DOWN enables the cloud connection. Long UP opens local phone provisioning; double OK closes its hotspot while preserving the station connection. Saved Wi-Fi reconnects at startup. The network status is shown above the conversation. Wi-Fi uses the same bounded media messages over WSS, validates the server certificate with the ESP-IDF CA bundle, and synchronizes time before TLS. No raw audio is retained by the cloud media bridge.

A deployment-specific device token is provisioned once over USB with `network.configure`; it is stored in NVS, never built into a public image. The cloud stores STT/TTS credentials, allowed Agent nameplates and the authorized execution computer. Device and remote-sender credentials are distinct. `remote.begin` requires an idle device and a `remote.ready` response before playback. A sender must check the cloud message receipt; HTTP acceptance is not playback completion. This increment requires separate Wi-Fi, power-only, remote playback and reconnect tests; earlier USB results do not validate it.

This branch replaces the hardware-test menu with a dedicated voice screen. It reuses the board BSP and Chinese font. `main/summon_voice.c` is the application entry point. The host companion is [gateway.passport](https://github.com/rfdiosuao/summon-protocol/blob/main/gateway/passport.py).

Connect USB to the Windows gateway. OK starts recording; silence after speech ends the utterance, or OK submits immediately. The maximum recording is eight seconds. Double OK cancels or returns. UP/DOWN changes volume by ten points; long UP opens phone provisioning. Visit the address displayed on the device. USB operation permits saving only an Agent nameplate, without Wi-Fi credentials. The page also offers a volume slider. Settings persist in NVS; do not overwrite NVS with a merged image when upgrading.

Media uses bounded `SUMMON1 ` JSONL frames, separate from Hub control messages. PCM is 16 kHz, 16-bit, mono, Base64 chunks at most 1024 bytes. Recording frames contain turn and sequence IDs; missing/out-of-order chunks are rejected by the host. Playback queues sixteen chunks and prebuffers twelve; `play.ack` acknowledges enqueueing, while `play.done` reports bytes, elapsed time and underruns after the audio worker finishes. Cancellation discards queued playback. USB reads are batched to avoid one-byte host overhead.

Keys only enqueue events. Audio runs in separate tasks. LVGL writes hold the BSP lock. API keys never enter firmware. The Windows bridge uploads audio to the configured speech service; the remote Agent returns authorized desktop actions and results. Current tests use a server-hosted test Agent, not a user's existing local Codex instance. Two-PC routing and Codex integration are separate follow-up work.

Validation: CI firmware and host gates; application-only flash with identical partition table; live microphone transcription and actual desktop command with cloud receipt; buffered speaker test. A 3735 ms test clip reported 3653 ms driver time and zero underruns. Driver timing does not prove acoustic quality; user listening and phone provisioning checks remain separate.
