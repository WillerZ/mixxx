// AI-GENERATED CONTENT DISCLAIMER: the code and comments in this file were
// generated autonomously by an AI assistant on the developer's machine, as
// allowed for assistance with code generation (see AGENTS.md). They are not an
// upstream contribution: the human developer must review them, test them on a
// real DJ setup, and submit them personally. No autonomous PRs or issues.
//
#include "engine/deckpairtransport.h"

void EnginePairTransport::publish(const PairTransportSnapshot& snapshot) {
    if (!isActive()) {
        // A disarmed pair never shares a transport, not even a stale one.
        return;
    }
    // Seqlock writer: enter the write window with an odd sequence number,
    // fill the cell, then leave the window with the next even number. The
    // release store orders all field writes before the window is closed.
    const quint32 seq = m_seq.load(std::memory_order_relaxed);
    // Sequentially consistent store publishes the odd marker before any field
    // of the new snapshot becomes visible to the reader.
    m_seq.store(seq + 1, std::memory_order_seq_cst);

    m_cell.posStart.store(snapshot.posStart, std::memory_order_relaxed);
    m_cell.posEnd.store(snapshot.posEnd, std::memory_order_relaxed);
    m_cell.rate.store(snapshot.rate, std::memory_order_relaxed);
    m_cell.speed.store(snapshot.speed, std::memory_order_relaxed);
    m_cell.playing.store(snapshot.playing, std::memory_order_relaxed);
    m_cell.scratching.store(snapshot.scratching, std::memory_order_relaxed);
    m_cell.reverse.store(snapshot.reverse, std::memory_order_relaxed);

    m_seq.store(seq + 2, std::memory_order_release);
}

bool EnginePairTransport::consume(PairTransportSnapshot* pSnapshotOut) const {
    if (pSnapshotOut == nullptr || !isActive()) {
        return false;
    }

    // Seqlock reader: the acquire loads pin the field reads between two
    // sequence reads. A differing pair means the writer was in the middle of
    // an update, so the snapshot is retried.
    for (int attempt = 0; attempt < kMaxConsumeAttempts; ++attempt) {
        const quint32 seqBefore = m_seq.load(std::memory_order_acquire);
        if (seqBefore == kInitialSeq) {
            // Nothing published yet.
            return false;
        }
        if (seqBefore % 2 != 0) {
            // Publisher inside its write window.
            continue;
        }

        PairTransportSnapshot snapshot;
        snapshot.posStart = m_cell.posStart.load(std::memory_order_relaxed);
        snapshot.posEnd = m_cell.posEnd.load(std::memory_order_relaxed);
        snapshot.rate = m_cell.rate.load(std::memory_order_relaxed);
        snapshot.speed = m_cell.speed.load(std::memory_order_relaxed);
        snapshot.playing = m_cell.playing.load(std::memory_order_relaxed);
        snapshot.scratching = m_cell.scratching.load(std::memory_order_relaxed);
        snapshot.reverse = m_cell.reverse.load(std::memory_order_relaxed);

        if (m_seq.load(std::memory_order_acquire) == seqBefore) {
            *pSnapshotOut = snapshot;
            return true;
        }
    }
    return false;
}

void EnginePairTransport::setActive(bool active) {
    m_active.store(active ? 1 : 0, std::memory_order_seq_cst);
}

bool EnginePairTransport::isActive() const {
    return m_active.load(std::memory_order_acquire) != 0;
}

void EnginePairTransport::setRoles(PairRole leaderRole, PairRole followerRole) {
    m_roleLeader.store(static_cast<quint8>(leaderRole), std::memory_order_seq_cst);
    m_roleFollower.store(static_cast<quint8>(followerRole), std::memory_order_seq_cst);
}

PairRole EnginePairTransport::leaderRole() const {
    return static_cast<PairRole>(m_roleLeader.load(std::memory_order_acquire));
}

PairRole EnginePairTransport::followerRole() const {
    return static_cast<PairRole>(m_roleFollower.load(std::memory_order_acquire));
}

void EnginePairTransport::reset() {
    setRoles(PairRole::Unpaired, PairRole::Unpaired);
    setActive(false);
    m_cell.posStart.store(0.0, std::memory_order_relaxed);
    m_cell.posEnd.store(0.0, std::memory_order_relaxed);
    m_cell.rate.store(0.0, std::memory_order_relaxed);
    m_cell.speed.store(0.0, std::memory_order_relaxed);
    m_cell.playing.store(false, std::memory_order_relaxed);
    m_cell.scratching.store(false, std::memory_order_relaxed);
    m_cell.reverse.store(false, std::memory_order_relaxed);
    m_seq.store(kInitialSeq, std::memory_order_release);
}

bool EnginePairTransport::hasSnapshot() const {
    return m_seq.load(std::memory_order_acquire) != kInitialSeq;
}

// End of autonomously AI-generated content; see the disclaimer at the top of
// this file.
