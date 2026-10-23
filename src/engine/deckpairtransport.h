// AI-GENERATED CONTENT DISCLAIMER: the code and comments in this file were
// generated autonomously by an AI assistant on the developer's machine, as
// allowed for assistance with code generation (see AGENTS.md). They are not an
// upstream contribution: the human developer must review them, test them on a
// real DJ setup, and submit them personally. No autonomous PRs or issues.
//
#pragma once

#include <QtGlobal>
#include <atomic>

/// Role of a single deck inside a deck pair.
///
/// A pair is structurally asymmetric: the *leader* computes the transport once
/// per engine callback and publishes it, the *follower* replays it. This has
/// nothing to do with the BPM sync leader/follower roles (`SyncMode`): a pair
/// leader may well be a sync follower and vice versa.
enum class PairRole : quint8 {
    Unpaired = 0,
    PairLeader = 1,
    PairFollower = 2,
};

/// One transport state of a deck pair, as produced by the pair leader and
/// consumed by the pair follower.
///
/// `posStart`/`posEnd` are the pair plane frame positions at the beginning and
/// at the end of the leader's buffer. They are deliberately published as a
/// position *pair* and not as a rate: loop/trigger wraps, seeks and cache-miss
/// padding relocate the read position in the middle of a buffer, so the buffer
/// end position cannot be integrated from the rate alone.
struct PairTransportSnapshot {
    /// The seqlock sequence value this snapshot was read under. Two consumes
    /// reporting the same value saw the same publication, which is how a
    /// consumer tells "the publisher has stopped publishing" apart from "the
    /// pair plane is not moving": the contents cannot tell those apart,
    /// because a leader wrapping inside a short loop legitimately publishes
    /// byte-identical positions on every callback while playing at full speed
    /// (DECK_PAIR_PLAN.md 9.18).
    quint32 sequence = 0;
    double posStart = 0.0;
    double posEnd = 0.0;
    /// Sample consumption rate applied by the leader.
    double rate = 0.0;
    /// Ratio between track time and real time (1.0 == normal speed).
    double speed = 0.0;
    bool playing = false;
    bool scratching = false;
    bool reverse = false;

    /// Payload comparison only: `sequence` is transport metadata, not part of
    /// what was published, so it stays out of ==. Consumers that care about
    /// freshness compare `sequence` explicitly.
    bool operator==(const PairTransportSnapshot& other) const {
        return posStart == other.posStart && posEnd == other.posEnd &&
                rate == other.rate && speed == other.speed &&
                playing == other.playing && scratching == other.scratching &&
                reverse == other.reverse;
    }
    bool operator!=(const PairTransportSnapshot& other) const {
        return !(*this == other);
    }
};

/// The transport control plane shared by the two members of one deck pair.
///
/// There is exactly one instance per pair slot (two per session). Instances are
/// owned by `DeckPairManager` and are never destroyed during a session, which
/// makes the pointer handoff into `EngineBuffer` race-free by construction:
/// pair formation and teardown only change the role/active flags and the
/// contents of the snapshot, never the address of the object.
///
/// Threading model:
///   - publish() runs on the engine thread in the pair leader's buffer.
///   - consume() runs on the engine thread in the pair follower's buffer, which
///     EngineMixer orders right after the leader so that the follower always
///     replays a same-callback snapshot.
///   - The role and active flags are written by the main thread when a pair is
///     formed or disbanded.
///
/// publish()/consume() implement a seqlock (single producer, single consumer):
/// no locks, no allocations, and the reader never blocks the realtime path.
class EnginePairTransport {
  public:
    EnginePairTransport() = default;
    EnginePairTransport(const EnginePairTransport&) = delete;
    EnginePairTransport& operator=(const EnginePairTransport&) = delete;

    /// Publishes a new snapshot (pair leader, engine thread).
    void publish(const PairTransportSnapshot& snapshot);

    /// Copies the most recent complete snapshot into `*pSnapshotOut`.
    /// Returns false when the pair is inactive or when the leader has not
    /// published anything yet, in which case `*pSnapshotOut` is untouched.
    bool consume(PairTransportSnapshot* pSnapshotOut) const;

    /// True while the pair shares its transport.
    void setActive(bool active);
    bool isActive() const;

    void setRoles(PairRole leaderRole, PairRole followerRole);
    PairRole leaderRole() const;
    PairRole followerRole() const;

    /// Clears the flags and invalidates the published snapshot. Must only be
    /// called while the pair is disarmed, i.e. when no buffer consumes it.
    void reset();

    /// True once the leader has published at least one snapshot.
    bool hasSnapshot() const;

    /// \name Accumulator lane (follower -> leader)
    ///
    /// A transport input which its engine consumer reads-and-zeroes cannot be
    /// relayed as a control value: on a pair the follower's consumer is gated,
    /// so the follower's accumulator keeps whatever the latest write carried --
    /// absolute-velocity writers (the usual controller jog contract) overwrite,
    /// relative writers accumulate -- and every copy of that value to the leader
    /// re-feeds deltas the leader has already applied (DECK_PAIR_PLAN.md
    /// 9.12 i / 9.13). The lane moves the accumulation at the one point where
    /// its meaning is defined: the follower's gated consumer performs its
    /// regular read-and-zero once per callback and publishes exactly what it
    /// discarded; the leader's consumer adds the lane to its own read at its
    /// regular consumption step. Both calls run on the engine thread in their
    /// owner's buffer processing; EngineMixer orders the leader's buffer before
    /// the follower's, so a value published in callback N takes effect in the
    /// leader's callback N+1 -- the same one-buffer delay a write landing just
    /// after the leader's consumption point has on a single deck.
    ///
    /// Latest-wins by design: there is exactly one publish per follower
    /// callback, so publishing re-baselines, and a leader that is loading
    /// (nothing takes) stalls the lane at one window's value instead of
    /// accumulating a stale lump. @{
    /// No-ops on NaN (a real controller may send unprocessed absolute wheel
    /// positions before it has a reference; a NaN in the lane would otherwise
    /// poison every later window).
    void publishAccumulated(double value);
    /// Drains the lane, leaving zero behind.
    double takeAccumulated();
    /// Test hook: the pending accumulation without removing it.
    double accumulated() const;
    /// @}

  private:
    static constexpr quint32 kInitialSeq = 0;
    /// Bounded retries keep consume() wait-free: giving up is harmless because
    /// the follower simply replays the previous snapshot in that (practically
    /// unreachable) case.
    static constexpr int kMaxConsumeAttempts = 8;

    // Each field is stored atomically so that a concurrent reader can never
    // observe a torn value; the sequence counter makes sure that the fields of
    // one snapshot are never mixed with those of the next one.
    struct SnapshotCell {
        std::atomic<double> posStart{0.0};
        std::atomic<double> posEnd{0.0};
        std::atomic<double> rate{0.0};
        std::atomic<double> speed{0.0};
        std::atomic<bool> playing{false};
        std::atomic<bool> scratching{false};
        std::atomic<bool> reverse{false};
    };

    SnapshotCell m_cell;
    std::atomic<quint32> m_seq{kInitialSeq};
    std::atomic<quint8> m_active{0};
    std::atomic<quint8> m_roleLeader{static_cast<quint8>(PairRole::Unpaired)};
    std::atomic<quint8> m_roleFollower{static_cast<quint8>(PairRole::Unpaired)};
    /// Accumulator lane, latest-wins; see `publishAccumulated()`. Written by
    /// the follower's engine pass, exchanged by the leader's; atomically so a
    /// concurrent take can never observe a torn value or lose a publish.
    std::atomic<double> m_accumulator{0.0};
};

// End of autonomously AI-generated content; see the disclaimer at the top of
// this file.
