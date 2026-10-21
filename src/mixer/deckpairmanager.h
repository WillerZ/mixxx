// AI-GENERATED CONTENT DISCLAIMER: the code and comments in this file were
// generated autonomously by an AI assistant on the developer's machine, as
// allowed for assistance with code generation (see AGENTS.md). They are not an
// upstream contribution: the human developer must review them, test them on a
// real DJ setup, and submit them personally. No autonomous PRs or issues.
//
#pragma once

#include <QObject>
#include <QString>
#include <memory>
#include <vector>

#include "engine/deckpairtransport.h"
#include "preferences/usersettings.h"
#include "track/track_decl.h"
#include "util/parented_ptr.h"

class ControlObject;
class ControlProxy;
class Deck;
class EngineBuffer;
class EngineMixer;

/// Owns the lifecycle of deck pairs: two decks (1+3 or 2+4) that act as a
/// single deck with a single transport while keeping independent audio data
/// planes.
///
/// The deck that receives the `[ChannelN], pair_enabled` write is the pair
/// leader, its track is the pair's track and (from Phase 2 on) its transport
/// computation is the pair's transport. Everything is main thread: the class
/// only writes control objects and atomics, it never touches engine buffers
/// directly except through the thread-safe role handoff.
///
/// Slip is never coupled: no slip-related control is bridged in any direction,
/// see DECK_PAIR_PLAN.md R8.
class DeckPairManager : public QObject {
    Q_OBJECT
  public:
    /// \param pEngine is only used to look up channel groups.
    DeckPairManager(UserSettingsPointer pConfig,
            EngineMixer* pEngine,
            QObject* pParent = nullptr);
    ~DeckPairManager() override;

    /// Registers a deck with the manager. Must be called once per deck, in
    /// increasing deck index order and from the main thread. Decks are
    /// grow-only, so a formed pair never loses its partner.
    void addDeck(int deckIndex, Deck* pDeck);

    /// Number of registered (i.e. existing) decks.
    int numberOfDecks() const;

    /// Partner deck index of `deckIndex` within a pair (0-based): decks 1+3 and
    /// 2+4, i.e. `deckIndex +/- 2`. Returns -1 for invalid indices.
    static int partnerDeckIndex(int deckIndex);

    /// \name Bridge tables (test hooks)
    /// The control keys the pair bridge relays (F -> L) and mirrors (L -> F).
    /// Exposed for the "no slip keys" invariant test (DECK_PAIR_PLAN.md T30):
    /// no slip-related key may ever appear in either direction, and slip is
    /// steered only through the follower's own slip plane.
    /// @{
    static QStringList bridgeRelayKeys();
    static QStringList bridgeMirrorKeys();
    /// The pending `jog` accumulation of the active pair containing `group`
    /// -- the transport's accumulator lane between the follower's gated
    /// consume-and-zero and the leader's consumption step. 0.0 when the deck
    /// is unpaired. Exposed for the lane transfer test
    /// (DECK_PAIR_PLAN.md 9.13).
    double pairAccumulated(const QString& group) const;
    /// @}

    /// \name Pair state queries (main thread)
    /// @{
    bool isPaired(const QString& group) const;
    PairRole pairRole(const QString& group) const;
    /// Group of the partner deck, or an empty string if the deck is unpaired.
    QString partnerGroup(const QString& group) const;
    QString pairDescription(const QString& group) const;
    /// @}

  signals:
    /// Emitted after a pair has been formed.
    void pairEnabled(const QString& leaderGroup, const QString& followerGroup);
    /// Emitted after a pair has been disbanded.
    void pairDisabled(const QString& formerLeaderGroup,
            const QString& formerFollowerGroup);

  private:
    /// One registered deck plus its pair-related controls.
    struct Member {
        int deckIndex = -1;
        QString group;
        Deck* pDeck = nullptr;
        EngineBuffer* pEngineBuffer = nullptr;
        std::unique_ptr<ControlObject> pPairEnabled;
        parented_ptr<ControlProxy> pPlay;
        parented_ptr<ControlProxy> pPlayLatched;
        parented_ptr<ControlProxy> pPassthrough;
        // Transport authorities which are refused on a pair follower
        // (sync and vinyl control). They bounce back to 0.
        std::vector<std::unique_ptr<ControlProxy>> pRefusedAuthorities;
    };

    /// One bridged control of a pair. Both proxies double as write objects so
    /// that the control objects' self-echo suppression applies to the bridge
    /// itself; echoes are further quiesced by equality -- a bridge write is
    /// skipped when its target already carries the value
    /// (DECK_PAIR_PLAN.md 5.5 / 9.11 b), which is what stops trigger buttons
    /// (they re-emit on every write) from ping-ponging the pair.
    struct BridgeEntry {
        QString item;
        /// True for mirrors of owner-only (read-only) status controls, which
        /// only accept the owner's forceSet protocol.
        bool forced = false;
        std::unique_ptr<ControlProxy> pFollower; // listener: relay / writer: mirror
        std::unique_ptr<ControlProxy> pLeader;   // writer: relay / listener: mirror
    };

    /// One pair slot: decks 1+3 (slot 0) and decks 2+4 (slot 1).
    struct Pair {
        int slotIndex = -1;
        int leaderDeckIndex = -1;
        int followerDeckIndex = -1;
        bool active = false;
        // Static lifetime: the address is handed to the engine buffers, so it
        // must never change and the object is never destroyed.
        EnginePairTransport transport;
        // The CO bridge (DECK_PAIR_PLAN.md 5.5): transport commands relayed
        // follower -> leader, statuses mirrored leader -> follower. Built on
        // formation, destroyed on disband, main thread only. Slip disconnects
        // the whole bridge in both directions: a slipping follower behaves
        // exactly like a single deck, and its replica re-anchors from the
        // leader when slip releases (R8, 9.11 m). The relay also refuses
        // commands during a mid-roll loop adoption (pFollowerBuffer, T30).
        std::vector<std::unique_ptr<BridgeEntry>> relays;
        std::vector<std::unique_ptr<BridgeEntry>> mirrors;
        std::unique_ptr<ControlProxy> pFollowerSlipEnabled;
        /// The follower's engine buffer, consulted by the relay to decide
        /// whether the follower is mid-adoption of a rolled loop
        /// (slipQuitAndAdopt): such commands stay local (R8 / T30). Set for
        /// the lifetime of an active pair, null otherwise.
        EngineBuffer* pFollowerBuffer = nullptr;
    };

    Member* member(const QString& group) const;
    Member* memberByIndex(int deckIndex) const;
    Pair* pairForDeckIndex(int deckIndex) const;
    Pair* activePairForDeckIndex(int deckIndex) const;
    bool isLockedFollower(const Member& member) const;

    /// True if the deck is audible on its own: playing, or latched-play
    /// (a running cue preview).
    /// Pairing such a deck would hijack its transport, so it is refused (R5).
    static bool isAudible(const Member& member);

    void slotPairEnabledChangeRequest(int deckIndex, double value);
    void slotLoadingTrack(int deckIndex, TrackPointer pNewTrack, TrackPointer pOldTrack);
    /// Refuses a write of a transport authority on a pair follower.
    void slotRefusedAuthorityRequest(int deckIndex, const QString& item, double value);
    void slotRefusedAuthorityChanged(const QString& group,
            const QString& item,
            double value);
    void refuseAuthority(Member& member, const QString& item);
    void slotPassthroughChanged(int deckIndex, double value);

    void formPair(Member& leader, Member& follower);
    void disbandPair(Pair& pair, const QString& requestingGroup);
    void disarmPair(Pair& pair);
    void disbandAllPairs();

    /// Builds / tears down the CO bridge of one pair (main thread).
    void buildBridge(Pair& pair, Member& leader, Member& follower);
    void clearBridge(Pair& pair);
    /// Adopts the leader's current mirror statuses on the follower once
    /// (bridge formation and follower slip release, 9.11 m).
    void refreshMirrorValues(Pair& pair);
    /// Relays one follower command to the leader (F -> L, 5.5).
    void slotBridgeRelay(Pair* pPair, BridgeEntry* pEntry, double value);
    /// Mirrors one leader status onto the follower (L -> F, 5.5).
    void slotBridgeMirror(Pair* pPair, BridgeEntry* pEntry, double value);
    /// Writes an owner-only (read-only) mirror status the way its engine
    /// owner writes it.
    static void forceMirrorValue(
            const QString& followerGroup, const QString& item, double value);
    QString activePairMemberGroup(int deckIndex) const;
    /// Switches pre-existing follower-side transport authorities (sync,
    /// vinyl control) off at pair formation (5.8).
    void neutralizeFollowerAuthorities(Member& follower);

    /// Makes both members hold the leader's track. A leader without a track
    /// adopts the follower's track instead, so that forming a pair never
    /// silently discards a loaded track.
    void synchronizeTracks(Member& leader, Member& follower);
    /// Propagates a load (or an eject, with a null track) of one member to the
    /// other one. The track comes from the load signal itself: the announcing
    /// deck may still hold the old track while it unloads.
    void propagateTrack(Member& target, TrackPointer pTrack);

    /// Requests playback stop of a member, e.g. to allow ejecting a paired
    /// deck which is still playing.
    static void stopDeck(Deck* pDeck);

    // The preferences back the "load when deck playing" gate; the EngineMixer
    // identifies the audio callback thread for the bridge (9.11 h).
    UserSettingsPointer m_pConfig;
    EngineMixer* m_pEngine;
    // std::vector rather than QVector: neither holder is copyable.
    std::vector<std::unique_ptr<Member>> m_members;
    std::vector<std::unique_ptr<Pair>> m_pairs;

    /// Re-entrancy guards: pair formation and load propagation both write
    /// through the very controls whose change signals they observe.
    bool m_bArbitrating = false;
    bool m_bPropagatingLoad = false;
    bool m_bDisbanding = false;
};

// End of autonomously AI-generated content; see the disclaimer at the top of
// this file.
