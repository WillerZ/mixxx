// AI-GENERATED CONTENT DISCLAIMER: the code and comments in this file were
// generated autonomously by an AI assistant on the developer's machine, as
// allowed for assistance with code generation (see AGENTS.md). They are not an
// upstream contribution: the human developer must review them, test them on a
// real DJ setup, and submit them personally. No autonomous PRs or issues.
//
// Unit tests for the deck pair transport control plane.
// DECK_PAIR_PLAN.md test T1: snapshot seqlock publish/consume and the
// role/active transitions.

#include "engine/deckpairtransport.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <thread>

namespace {
constexpr double kBufferFrames = 1024.0;
constexpr int kStressIterations = 100000;

PairTransportSnapshot makeSnapshot(int index) {
    PairTransportSnapshot snapshot;
    snapshot.posStart = index * kBufferFrames;
    snapshot.posEnd = snapshot.posStart + kBufferFrames;
    snapshot.rate = 1.0 + (index % 3) * 0.25;
    snapshot.speed = snapshot.rate;
    snapshot.playing = true;
    snapshot.scratching = index % 7 == 0;
    snapshot.reverse = index % 11 == 0;
    return snapshot;
}
} // namespace

TEST(EnginePairTransportTest, ConsumeFailsUntilAPairIsLiveAndPublished) {
    EnginePairTransport transport;
    PairTransportSnapshot consumed{};

    // Unpaired: nothing to consume, not even after a publish.
    transport.publish(makeSnapshot(1));
    EXPECT_FALSE(transport.consume(&consumed));

    transport.setActive(true);
    EXPECT_TRUE(transport.isActive());
    // Live, but the leader has not run a single callback yet.
    EXPECT_FALSE(transport.hasSnapshot());
    EXPECT_FALSE(transport.consume(&consumed));
}

TEST(EnginePairTransportTest, PublishConsumeRoundTrip) {
    EnginePairTransport transport;
    transport.setActive(true);

    const PairTransportSnapshot published = makeSnapshot(42);
    transport.publish(published);

    PairTransportSnapshot consumed{};
    ASSERT_TRUE(transport.consume(&consumed));
    EXPECT_EQ(published, consumed);
    EXPECT_DOUBLE_EQ(42 * kBufferFrames, consumed.posStart);
    EXPECT_TRUE(consumed.playing);

    // Consume is idempotent: the same snapshot is replayed until the leader
    // publishes the next one.
    PairTransportSnapshot again{};
    ASSERT_TRUE(transport.consume(&again));
    EXPECT_EQ(published, again);
}

TEST(EnginePairTransportTest, LatestSnapshotWins) {
    EnginePairTransport transport;
    transport.setActive(true);
    transport.publish(makeSnapshot(1));
    transport.publish(makeSnapshot(2));
    transport.publish(makeSnapshot(3));

    PairTransportSnapshot consumed{};
    ASSERT_TRUE(transport.consume(&consumed));
    EXPECT_EQ(makeSnapshot(3), consumed);
}

TEST(EnginePairTransportTest, InactivePairYieldsNoSnapshot) {
    EnginePairTransport transport;
    transport.setActive(true);
    transport.publish(makeSnapshot(5));

    PairTransportSnapshot consumed{};
    ASSERT_TRUE(transport.consume(&consumed));

    // Tearing down a pair stops the transport sharing with the next callback,
    // the follower falls back to its own transport.
    transport.setActive(false);
    EXPECT_FALSE(transport.consume(&consumed));
    // The stale snapshot must not leak into the next pair.
    transport.reset();
    transport.setActive(true);
    EXPECT_FALSE(transport.hasSnapshot());
    EXPECT_FALSE(transport.consume(&consumed));
}

TEST(EnginePairTransportTest, RoleTransitions) {
    EnginePairTransport transport;
    EXPECT_EQ(PairRole::Unpaired, transport.leaderRole());
    EXPECT_EQ(PairRole::Unpaired, transport.followerRole());

    transport.setRoles(PairRole::PairLeader, PairRole::PairFollower);
    EXPECT_EQ(PairRole::PairLeader, transport.leaderRole());
    EXPECT_EQ(PairRole::PairFollower, transport.followerRole());

    transport.reset();
    EXPECT_EQ(PairRole::Unpaired, transport.leaderRole());
    EXPECT_EQ(PairRole::Unpaired, transport.followerRole());
    EXPECT_FALSE(transport.isActive());
}

TEST(EnginePairTransportTest, ConcurrentPublishConsumeNeverTears) {
    // Producer/consumer stress of the seqlock: the reader must never see
    // values from two different snapshots, and it must never see a partially
    // written one. Every snapshot fulfils posEnd == posStart + buffer frames
    // and speed == rate, and posStart increases monotonically.
    EnginePairTransport transport;
    transport.setActive(true);

    std::atomic<bool> producerDone{false};
    std::thread producer([&]() {
        for (int i = 1; i <= kStressIterations; ++i) {
            transport.publish(makeSnapshot(i));
        }
        producerDone.store(true);
    });

    const double kFinalPos = kStressIterations * kBufferFrames;
    double lastPosStart = 0.0;
    int consumedCount = 0;
    int polls = 0;
    while (lastPosStart < kFinalPos && polls < 20 * kStressIterations) {
        PairTransportSnapshot consumed{};
        if (transport.consume(&consumed)) {
            EXPECT_DOUBLE_EQ(consumed.posStart + kBufferFrames, consumed.posEnd)
                    << "torn snapshot at poll " << polls;
            EXPECT_DOUBLE_EQ(consumed.rate, consumed.speed);
            EXPECT_GE(consumed.posStart, lastPosStart);
            lastPosStart = std::max(lastPosStart, consumed.posStart);
            ++consumedCount;
        }
        ++polls;
    }
    producer.join();

    EXPECT_TRUE(producerDone.load());
    EXPECT_GT(consumedCount, 0);
    EXPECT_DOUBLE_EQ(kFinalPos, lastPosStart)
            << "the final snapshot was never observed as a whole";
}

// End of autonomously AI-generated content; see the disclaimer at the top of
// this file.
