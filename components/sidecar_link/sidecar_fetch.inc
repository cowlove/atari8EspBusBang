// sidecarFetch(): a whole object from the peer (sidecar.h). Included at the
// end of sidecar.h: a template, so the sink can be any callable.
//
// A Nop probe, then ReadVersion, then ReadChunk from offset 0 to the end (a zero-length
// chunk), passing each chunk to sink(offset, data, length) (return false to
// stop), then ReadVersion again. consistent: all of it arrived and the
// token is the same after as before. crcMatches: the received bytes' CRC32
// equals the token, a full check for objects whose token is a CRC32
// (Firmware, Test; Core's is a SHA-256 prefix, checked by the caller). A Preempted call (the Sidecar only) serves the host's
// command (peer.serviceOnce()) and retries. Cold paths only: may take a
// while.

uint32_t sidecarCrc32(uint32_t crc, const uint8_t *data, size_t length);

// Nop until the peer answers (or timeoutUs): a side that restarted drops
// the first command it sees (sidecar.h, rule 7), so the first command after
// either side started may vanish. Serves the peer's command if preempted.
// Each try waits retryUs: longer than the host's poll interval (about 50 ms,
// core0_utilityTasks()), or every answer comes for a Nop already given up.
template <typename Peer>
bool sidecarProbe(Peer &peer, uint32_t timeoutUs, uint32_t retryUs = 200000)
{
    for (uint32_t waited = 0; waited < timeoutUs; waited += retryUs) {
        NopCommand command = {};
        NopResponse response;
        const SidecarResult r = peer.transact(SidecarCommand::Nop, &command, &response, retryUs);
        if (r == SidecarResult::Ok) return true;
        if (r == SidecarResult::Preempted) peer.serviceOnce();
    }
    return false;
}

template <typename Peer, typename Sink>
SidecarFetchResult sidecarFetch(Peer &peer, SidecarObject object, Sink &&sink,
                                uint32_t chunkTimeoutUs = 200000)
{
    SidecarFetchResult r = {};
    auto retry = [&](auto &&call) {
        for (int attempt = 0; attempt < 1000; ++attempt) {
            r.link = call();
            if (r.link != SidecarResult::Preempted) return;
            peer.serviceOnce();
        }
    };
    // A probe first: the peer may have restarted since our last command, and
    // would drop the next one.
    if (!sidecarProbe(peer, 2000000)) {
        r.link = SidecarResult::Timeout;
        return r;
    }
    uint32_t versionBefore = 0;
    retry([&] { return peer.readVersion(object, &r.size, &versionBefore, &r.status, chunkTimeoutUs); });
    r.version = versionBefore;
    if (r.link != SidecarResult::Ok || r.status != SidecarReadStatus::Ok) return r;

    uint8_t chunk[ReadChunkResponse::kDataBytes];
    for (;;) {
        uint16_t length = 0;
        retry([&] { return peer.readChunk(object, r.received, chunk, &length, &r.status, chunkTimeoutUs); });
        if (r.link != SidecarResult::Ok || r.status != SidecarReadStatus::Ok) return r;
        if (length == 0) break;   // the end
        r.crc = sidecarCrc32(r.crc, chunk, length);
        if (!sink(r.received, static_cast<const uint8_t *>(chunk), length)) return r;
        r.received += length;
    }
    uint32_t sizeAfter = 0, versionAfter = 0;
    retry([&] { return peer.readVersion(object, &sizeAfter, &versionAfter, &r.status, chunkTimeoutUs); });
    if (r.link != SidecarResult::Ok || r.status != SidecarReadStatus::Ok) return r;
    r.consistent = versionAfter == versionBefore && r.received == r.size;
    r.crcMatches = r.crc == versionBefore;
    return r;
}
