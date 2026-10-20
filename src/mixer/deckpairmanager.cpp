// AI-GENERATED CONTENT DISCLAIMER: the code and comments in this file were
// generated autonomously by an AI assistant on the developer's machine, as
// allowed for assistance with code generation (see AGENTS.md). They are not an
// upstream contribution: the human developer must review them, test them on a
// real DJ setup, and submit them personally. No autonomous PRs or issues.
//
#include "mixer/deckpairmanager.h"

#include "control/controlobject.h"
#include "control/controlproxy.h"
#include "engine/channels/enginedeck.h"
#include "engine/enginebuffer.h"
#include "engine/enginemixer.h"
#include "engine/sync/syncable.h"
#include "mixer/deck.h"
#include "mixer/playermanager.h"
#include "moc_deckpairmanager.cpp"
// Backport note (2.6): LoadWhenDeckPlaying lives in preferences/dialog/dlgprefdeck.h
// on this branch (2.7 moved it to preferences/interface.h).
#include "preferences/dialog/dlgprefdeck.h"
#include "util/defs.h"
#include "util/logger.h"

namespace {
const mixxx::Logger kLoggerDeckPair("DeckPair");

constexpr qsizetype kPairSlotDistance = 2;
constexpr qsizetype kNumberOfPairSlots = 2;

constexpr char kPairEnabledItem[] = "pair_enabled";

/// Controls which would make a deck a transport authority of its own. On a
/// pair follower they are refused, because the pair leader alone computes the
/// transport. All of them are owned by engine controls which confirm writes
/// themselves (`ControlObject::set()` alone does not change their value), so a
/// refusal both undoes the pending engine request and snaps the control back
/// to 0. Slip controls are deliberately *not* part of this list: slip stays
/// strictly deck-specific in every situation (R8).
const QStringList kRefusedFollowerAuthorityItems = {
        QStringLiteral("sync_enabled"),
        QStringLiteral("sync_mode"),
        QStringLiteral("sync_leader"),
        QStringLiteral("vinylcontrol_enabled"),
};

/// Controls which take the requested value immediately (no confirmation by an
/// owner). They are watched for a value change and reset instead.
const QStringList kRefusedFollowerPlainItems = {
        QStringLiteral("vinylcontrol_wantenabled"),
};

/// The controls whose engine side needs an explicit undo request.
bool isSyncAuthorityItem(const QString& item) {
    return item.startsWith(QLatin1String("sync_"));
}

/// `[ChannelN], pair_enabled` is only meaningful for real decks.
bool isPairableDeckGroup(const QString& group) {
    return PlayerManager::isDeckGroup(group);
}

// The "[Controls]" keys of the "load when deck playing" safeguard come from
// preferences/dialog/dlgprefdeck.h, which defines them in an anonymous
// namespace (backport note (2.6): on this branch that header is both the
// canonical definition and the include source, as for playermanager.cpp;
// on 2.7 the enum lives in preferences/interface.h and the keys had to be
// repeated here).

/// The user's safeguard against loading a track onto a playing deck, with the
/// same tri-state and legacy handling as the library load paths (drag & drop,
/// track menu, table view).
LoadWhenDeckPlaying loadWhenDeckPlaying(const UserSettingsPointer& pConfig) {
    if (pConfig->exists(kConfigKeyLoadWhenDeckPlaying)) {
        return static_cast<LoadWhenDeckPlaying>(
                pConfig->getValueString(kConfigKeyLoadWhenDeckPlaying).toInt());
    }
    // Support the older version of this flag.
    return pConfig->getValue<bool>(kConfigKeyAllowTrackLoadToPlayingDeck)
            ? LoadWhenDeckPlaying::Allow
            : kDefaultLoadWhenDeckPlaying;
}

bool isPlaying(const ControlProxy* pPlay) {
    return pPlay != nullptr && pPlay->toBool();
}
} // namespace

DeckPairManager::DeckPairManager(UserSettingsPointer pConfig,
        EngineMixer* pEngine,
        QObject* pParent)
        : QObject(pParent),
          m_pConfig(pConfig),
          m_pEngine(pEngine) {
    m_members.resize(kMaxNumberOfDecks);
    m_pairs.reserve(kNumberOfPairSlots);
    for (int slot = 0; slot < kNumberOfPairSlots; ++slot) {
        auto pPair = std::make_unique<Pair>();
        pPair->slotIndex = slot;
        m_pairs.push_back(std::move(pPair));
    }
}

DeckPairManager::~DeckPairManager() {
    // Disarm everything before the decks (and therefore the engine buffers
    // holding a pointer to our static transports) are destroyed.
    disbandAllPairs();
}

void DeckPairManager::addDeck(int deckIndex, Deck* pDeck) {
    VERIFY_OR_DEBUG_ASSERT(pDeck != nullptr) {
        return;
    }
    VERIFY_OR_DEBUG_ASSERT(deckIndex >= 0 &&
            deckIndex < static_cast<int>(m_members.size())) {
        qWarning() << Q_FUNC_INFO << "ignoring deck index" << deckIndex;
        return;
    }
    VERIFY_OR_DEBUG_ASSERT(m_members[deckIndex] == nullptr) {
        qWarning() << Q_FUNC_INFO << "deck index" << deckIndex << "is already registered";
        return;
    }
    const QString group = pDeck->getGroup();
    VERIFY_OR_DEBUG_ASSERT(isPairableDeckGroup(group)) {
        qWarning() << Q_FUNC_INFO << group << "is not a deck group";
        return;
    }

    auto pMember = std::make_unique<Member>();
    pMember->deckIndex = deckIndex;
    pMember->group = group;
    pMember->pDeck = pDeck;
    pMember->pEngineBuffer = pDeck->getEngineDeck()->getEngineBuffer();
    VERIFY_OR_DEBUG_ASSERT(pMember->pEngineBuffer != nullptr) {
        return;
    }

    // The pair_enabled control is owned by the manager: every write is
    // arbitrated here, following the [App], num_decks pattern. The value is
    // not persisted: a pair is a runtime-only state.
    pMember->pPairEnabled = std::make_unique<ControlObject>(
            ConfigKey(group, QString::fromLatin1(kPairEnabledItem)));
    pMember->pPairEnabled->setDescription(tr(
            "Act together with the partner deck as a single deck"));
    pMember->pPairEnabled->connectValueChangeRequest(
            this,
            [this, deckIndex](double value) {
                slotPairEnabledChangeRequest(deckIndex, value);
            },
            Qt::DirectConnection);

    pMember->pPlay = make_parented<ControlProxy>(
            group, QStringLiteral("play"), this);
    pMember->pPlayLatched = make_parented<ControlProxy>(
            group, QStringLiteral("play_latched"), this);
    pMember->pPassthrough = make_parented<ControlProxy>(
            group, QStringLiteral("passthrough"), this);
    pMember->pPassthrough->connectValueChanged(
            this,
            [this, deckIndex](
                    double value) { slotPassthroughChanged(deckIndex, value); },
            Qt::DirectConnection);

    // Transport authorities of the decks themselves. They exist once the deck
    // has been constructed; missing controls are simply not refused.
    for (const QString& item : kRefusedFollowerAuthorityItems) {
        const ConfigKey key(group, item);
        ControlObject* pControl = ControlObject::getControl(key);
        if (pControl == nullptr) {
            continue;
        }
        // The owner of these controls confirms every write, so a change
        // request is the earliest point at which a refusal can intervene. Our
        // handler runs after the owner confirmed the value.
        pControl->connectValueChangeRequest(
                this,
                [this, deckIndex, item](double value) {
                    slotRefusedAuthorityRequest(deckIndex, item, value);
                },
                Qt::DirectConnection);
        auto pProxy = std::make_unique<ControlProxy>(key, this);
        pProxy->connectValueChanged(
                this,
                [this, group, item](double value) {
                    slotRefusedAuthorityChanged(group, item, value);
                },
                Qt::DirectConnection);
        pMember->pRefusedAuthorities.push_back(std::move(pProxy));
    }
    for (const QString& item : kRefusedFollowerPlainItems) {
        const ConfigKey key(group, item);
        if (!ControlObject::exists(key)) {
            continue;
        }
        auto pProxy = std::make_unique<ControlProxy>(key, this);
        pProxy->connectValueChanged(
                this,
                [this, group, item](double value) {
                    slotRefusedAuthorityChanged(group, item, value);
                },
                Qt::DirectConnection);
        pMember->pRefusedAuthorities.push_back(std::move(pProxy));
    }

    // Load and eject of a paired deck affect both members. loadingTrack()
    // fires for every load entry point (library, QML, AutoDJ, clone, ...).
    connect(pDeck,
            &BaseTrackPlayer::loadingTrack,
            this,
            [this, deckIndex](TrackPointer pNewTrack, TrackPointer pOldTrack) {
                slotLoadingTrack(deckIndex, pNewTrack, pOldTrack);
            });

    kLoggerDeckPair.debug() << Q_FUNC_INFO << deckIndex << group;
    m_members[deckIndex] = std::move(pMember);
}

int DeckPairManager::numberOfDecks() const {
    int count = 0;
    for (const auto& pMember : m_members) {
        if (pMember) {
            ++count;
        }
    }
    return count;
}

// static
int DeckPairManager::partnerDeckIndex(int deckIndex) {
    if (deckIndex < 0 || deckIndex >= kMaxNumberOfDecks) {
        return -1;
    }
    // Decks are paired across the slot distance: 0<->2 and 1<->3.
    const int partner = deckIndex < kPairSlotDistance
            ? deckIndex + kPairSlotDistance
            : deckIndex - kPairSlotDistance;
    return (partner >= 0 && partner < kMaxNumberOfDecks) ? partner : -1;
}

// static
bool DeckPairManager::isAudible(const Member& member) {
    if (isPlaying(member.pPlay.get())) {
        return true;
    }
    // A latched play covers a running cue preview: the deck is audible and
    // pairing it would hijack its transport. (cue_indicator is deliberately
    // not consulted: it also blinks in the ordinary stopped state.)
    return member.pPlayLatched && member.pPlayLatched->toBool();
}

bool DeckPairManager::isPaired(const QString& group) const {
    const Member* pMember = member(group);
    return pMember != nullptr && activePairForDeckIndex(pMember->deckIndex) != nullptr;
}

PairRole DeckPairManager::pairRole(const QString& group) const {
    const Member* pMember = member(group);
    if (pMember == nullptr) {
        return PairRole::Unpaired;
    }
    const Pair* pPair = activePairForDeckIndex(pMember->deckIndex);
    if (pPair == nullptr) {
        return PairRole::Unpaired;
    }
    return pPair->leaderDeckIndex == pMember->deckIndex ? PairRole::PairLeader
                                                        : PairRole::PairFollower;
}

QString DeckPairManager::partnerGroup(const QString& group) const {
    const Member* pMember = member(group);
    if (pMember == nullptr) {
        return QString();
    }
    const Pair* pPair = activePairForDeckIndex(pMember->deckIndex);
    if (pPair == nullptr) {
        return QString();
    }
    const int partnerIndex = pPair->leaderDeckIndex == pMember->deckIndex
            ? pPair->followerDeckIndex
            : pPair->leaderDeckIndex;
    const Member* pPartner = memberByIndex(partnerIndex);
    return pPartner ? pPartner->group : QString();
}

QString DeckPairManager::pairDescription(const QString& group) const {
    const QString partner = partnerGroup(group);
    if (partner.isEmpty()) {
        return QStringLiteral("%1: unpaired").arg(group);
    }
    return QStringLiteral("%1 (%2): %3")
            .arg(group,
                    pairRole(group) == PairRole::PairLeader ? QStringLiteral("leader")
                                                            : QStringLiteral("follower"),
                    partner);
}

void DeckPairManager::slotPairEnabledChangeRequest(int deckIndex, double value) {
    if (m_bArbitrating) {
        // Our own guarded write to the partner's control.
        return;
    }
    Member* pSelf = memberByIndex(deckIndex);
    if (pSelf == nullptr) {
        return;
    }
    VERIFY_OR_DEBUG_ASSERT(pSelf->pPairEnabled) {
        return;
    }

    if (value <= 0.0) {
        Pair* pPair = activePairForDeckIndex(deckIndex);
        if (pPair != nullptr) {
            disbandPair(*pPair, pSelf->group);
        }
        pSelf->pPairEnabled->setAndConfirm(0.0);
        return;
    }

    // --- pairing request ---
    Pair* pSlot = pairForDeckIndex(deckIndex);
    if (pSlot == nullptr) {
        // No partner deck index at all (decks beyond 4).
        kLoggerDeckPair.warning() << pSelf->group
                                  << "has no partner deck, refusing to pair";
        pSelf->pPairEnabled->setAndConfirm(0.0);
        return;
    }
    if (pSlot->active) {
        // A pair slot only contains these two decks, so an active slot means
        // this very deck is a member of that pair: the write is redundant and
        // must not reset the partner's pair_enabled.
        kLoggerDeckPair.debug() << pSelf->group << "is already paired";
        pSelf->pPairEnabled->setAndConfirm(1.0);
        return;
    }

    const int partnerIndex = partnerDeckIndex(deckIndex);
    Member* pPartner = memberByIndex(partnerIndex);
    if (pPartner == nullptr) {
        kLoggerDeckPair.warning() << pSelf->group << "refuses to pair:"
                                  << "partner deck" << (partnerIndex + 1)
                                  << "does not exist";
        pSelf->pPairEnabled->setAndConfirm(0.0);
        return;
    }

    if (isAudible(*pPartner)) {
        kLoggerDeckPair.warning() << pSelf->group << "refuses to pair with deck"
                                  << (partnerIndex + 1)
                                  << "because it is currently playing";
        pSelf->pPairEnabled->setAndConfirm(0.0);
        return;
    }

    for (const Member* pCandidate : {pSelf, pPartner}) {
        if (pCandidate->pPassthrough && pCandidate->pPassthrough->toBool()) {
            kLoggerDeckPair.warning() << pCandidate->group
                                      << "refuses to pair: passthrough is active";
            pSelf->pPairEnabled->setAndConfirm(0.0);
            return;
        }
    }

    // Confirm the initiating write: like [App], num_decks the value only
    // lands when the owner of the control confirms it.
    pSelf->pPairEnabled->setAndConfirm(1.0);
    formPair(*pSelf, *pPartner);
}

void DeckPairManager::formPair(Member& leader, Member& follower) {
    Pair* pPair = pairForDeckIndex(leader.deckIndex);
    VERIFY_OR_DEBUG_ASSERT(pPair != nullptr && !pPair->active) {
        return;
    }
    VERIFY_OR_DEBUG_ASSERT(pPair == pairForDeckIndex(follower.deckIndex)) {
        return;
    }

    kLoggerDeckPair.info() << "Pairing" << leader.group << "(leader) with"
                           << follower.group << "(follower)";

    pPair->leaderDeckIndex = leader.deckIndex;
    pPair->followerDeckIndex = follower.deckIndex;

    // Hand the shared transport over to the buffers. The objects live as long
    // as the manager, so the pointers can never dangle while flags say Unpaired.
    pPair->transport.setRoles(PairRole::PairLeader, PairRole::PairFollower);
    leader.pEngineBuffer->setPairTransport(&pPair->transport, PairRole::PairLeader);
    follower.pEngineBuffer->setPairTransport(&pPair->transport, PairRole::PairFollower);
    pPair->transport.setActive(true);
    pPair->active = true;

    m_bArbitrating = true;
    follower.pPairEnabled->setAndConfirm(1.0);
    // The leader's own write is confirmed by the caller.
    m_bArbitrating = false;

    synchronizeTracks(leader, follower);

    emit pairEnabled(leader.group, follower.group);
}

void DeckPairManager::disbandPair(Pair& pair, const QString& requestingGroup) {
    Member* pLeader = memberByIndex(pair.leaderDeckIndex);
    Member* pFollower = memberByIndex(pair.followerDeckIndex);
    kLoggerDeckPair.info() << "Unpairing" << requestingGroup;

    // Teardown order (DECK_PAIR_PLAN.md 5.1): stop the bridge (Phase 3), clear
    // the pair-active flag so the engine stops publishing/consuming with the
    // next callback, release the follower's slip-plane feed (Phase 4), restore
    // the follower's sync/vinyl acceptability. No position writes are needed:
    // the follower's position already is the pair plane.
    disarmPair(pair);

    m_bArbitrating = true;
    if (pLeader && pLeader->pPairEnabled) {
        pLeader->pPairEnabled->setAndConfirm(0.0);
    }
    if (pFollower && pFollower->pPairEnabled) {
        pFollower->pPairEnabled->setAndConfirm(0.0);
    }
    m_bArbitrating = false;

    const QString leaderGroup = pLeader ? pLeader->group : QString();
    const QString followerGroup = pFollower ? pFollower->group : QString();
    emit pairDisabled(leaderGroup, followerGroup);
}

void DeckPairManager::disarmPair(Pair& pair) {
    Member* pLeader = memberByIndex(pair.leaderDeckIndex);
    Member* pFollower = memberByIndex(pair.followerDeckIndex);

    // Clear the engine-visible flags first, then drop the pointer handoff.
    pair.transport.setActive(false);
    if (pLeader && pLeader->pEngineBuffer) {
        pLeader->pEngineBuffer->setPairTransport(nullptr, PairRole::Unpaired);
    }
    if (pFollower && pFollower->pEngineBuffer) {
        pFollower->pEngineBuffer->setPairTransport(nullptr, PairRole::Unpaired);
    }
    pair.transport.reset();

    pair.leaderDeckIndex = -1;
    pair.followerDeckIndex = -1;
    pair.active = false;
}

void DeckPairManager::disbandAllPairs() {
    for (const auto& pPair : m_pairs) {
        if (pPair->active) {
            m_bDisbanding = true;
            disbandPair(*pPair, QStringLiteral("(shutdown)"));
            m_bDisbanding = false;
        }
    }
}

void DeckPairManager::synchronizeTracks(Member& leader, Member& follower) {
    // No "load when deck playing" check is needed here: pairing a deck which
    // is audible is refused outright (R5), so the deck adopting a track is
    // never playing at formation -- neither the follower (the leader's track
    // is adopted) nor an empty leader (it holds no track to play).
    TrackPointer pLeaderTrack = leader.pDeck->getLoadedTrack();
    TrackPointer pFollowerTrack = follower.pDeck->getLoadedTrack();

    m_bPropagatingLoad = true;
    if (pLeaderTrack) {
        if (pLeaderTrack != pFollowerTrack) {
            // The leader's track is the pair's track (R3). Reuse the clone
            // path so the first buffer is aligned, but keep the follower's own
            // stem selection: stem mute/solo/gain are strictly per deck (R9).
            kLoggerDeckPair.debug() << "Follower" << follower.group
                                    << "adopts the track of leader" << leader.group;
            follower.pDeck->cloneFromChannel(
                    leader.pDeck->getEngineDeck(),
#ifdef __STEM__
                    follower.pEngineBuffer->getStemMask(),
#endif
                    isPlaying(leader.pPlay.get()));
        }
    } else if (pFollowerTrack) {
        // An empty leader takes over the follower's track instead of ejecting
        // it: the pair stays loadable and no loaded track is lost.
        kLoggerDeckPair.debug() << "Empty leader" << leader.group
                                << "adopts the track of follower" << follower.group;
        leader.pDeck->cloneFromChannel(
                follower.pDeck->getEngineDeck(),
#ifdef __STEM__
                leader.pEngineBuffer->getStemMask(),
#endif
                false);
    }
    m_bPropagatingLoad = false;

    VERIFY_OR_DEBUG_ASSERT(leader.pDeck->getLoadedTrack() ==
            follower.pDeck->getLoadedTrack()) {
        kLoggerDeckPair.warning() << "Pair members" << leader.group << follower.group
                                  << "do not hold the same track";
    }
}

void DeckPairManager::slotLoadingTrack(
        int deckIndex, TrackPointer pNewTrack, TrackPointer pOldTrack) {
    if (m_bPropagatingLoad) {
        return;
    }
    if (!pNewTrack && !pOldTrack) {
        // Neither a load nor an eject.
        return;
    }

    const Member* pSource = memberByIndex(deckIndex);
    if (pSource == nullptr) {
        return;
    }
    const Pair* pPair = activePairForDeckIndex(deckIndex);
    if (pPair == nullptr) {
        return;
    }
    const int otherIndex = pPair->leaderDeckIndex == deckIndex
            ? pPair->followerDeckIndex
            : pPair->leaderDeckIndex;
    Member* pTarget = memberByIndex(otherIndex);
    if (pTarget == nullptr) {
        return;
    }

    // A load or an eject on any member affects both decks (R7). The leader's
    // transport remains the authority: a load on the follower is applied to
    // the leader as well, which restarts its transport on the new track.
    kLoggerDeckPair.debug() << "Propagating"
                            << (pNewTrack ? "load of" : "eject from")
                            << pSource->group << "to" << pTarget->group;
    propagateTrack(*pTarget, pNewTrack);
}

void DeckPairManager::propagateTrack(Member& target, TrackPointer pTrack) {
    TrackPointer pTargetTrack = target.pDeck->getLoadedTrack();
    if (pTrack == pTargetTrack) {
        return;
    }

    // The user's safeguard against touching a deck that is playing applies to
    // a partner deck as well: a pair must not be a way to get around it. Both
    // a propagated load and a propagated eject go through the same tri-state
    // rule and the legacy flag behind it.
    //
    // A refusal leaves the two members with different tracks for the moment.
    // Once the transport lock and the play-state mirror are in place it is an
    // invariant that either both members play or neither does, and the intended
    // end behaviour is all-or-nothing: the whole pair operation completes on
    // both decks or is rejected on both. Revisit this here when the pair is
    // bench-tested at the end of the implementation (see DECK_PAIR_PLAN.md 2).
    bool playAfterLoad = false;
    if (isPlaying(target.pPlay.get())) {
        switch (loadWhenDeckPlaying(m_pConfig)) {
        case LoadWhenDeckPlaying::Reject: {
            const char* const action = pTrack
                    ? "loading a track onto it is refused"
                    : "ejecting it is refused";
            kLoggerDeckPair.warning()
                    << target.group << "is playing:" << action
                    << " (Preferences > Decks > Load when deck playing)."
                    << " The pair holds different tracks until it is resolved.";
            return;
        }
        case LoadWhenDeckPlaying::Allow:
            // A load keeps playing the new track from its load point; an eject
            // leaves the deck stopped either way.
            playAfterLoad = static_cast<bool>(pTrack);
            break;
        case LoadWhenDeckPlaying::AllowButStopDeck:
            // Loading stops the deck: unloadTrack() releases play for us.
            break;
        }
    }

    m_bPropagatingLoad = true;
    if (!pTrack) {
        // Eject both. slotEjectTrack() refuses while playing, so the partner is
        // stopped first -- reached only when the safeguard above allows it.
        if (isPlaying(target.pPlay.get())) {
            stopDeck(target.pDeck);
        }
        target.pDeck->slotEjectTrack(1.0);
    } else {
#ifdef __STEM__
        const auto stemMask = target.pEngineBuffer
                ? target.pEngineBuffer->getStemMask()
                : mixxx::StemChannelSelection();
#endif
        target.pDeck->slotLoadTrack(pTrack,
#ifdef __STEM__
                stemMask,
#endif
                playAfterLoad);
    }
    m_bPropagatingLoad = false;
}

void DeckPairManager::slotRefusedAuthorityRequest(
        int deckIndex, const QString& item, double value) {
    if (value <= 0.0) {
        // Disabling is always allowed.
        return;
    }
    Member* pMember = memberByIndex(deckIndex);
    if (pMember == nullptr || !isLockedFollower(*pMember)) {
        return;
    }

    // The deck is a pair follower: its tempo and its transport come from the
    // pair leader. A follower-side tempo correction would be applied twice,
    // and an active follower would negotiate leadership for what is a single
    // transport, so the request is refused. The owner of the control has
    // already confirmed the value at this point, therefore the pending engine
    // request is undone and the control is snapped back to 0.
    refuseAuthority(*pMember, item);
}

void DeckPairManager::slotRefusedAuthorityChanged(
        const QString& group, const QString& item, double value) {
    if (value <= 0.0) {
        return;
    }
    Member* pMember = member(group);
    if (pMember == nullptr || !isLockedFollower(*pMember)) {
        return;
    }
    if (!kRefusedFollowerPlainItems.contains(item)) {
        // Confirmed controls are handled where the request arrives; this hook
        // only catches values which the engine itself wrote to the follower.
        ControlObject* pControl = ControlObject::getControl(ConfigKey(group, item));
        if (pControl != nullptr) {
            kLoggerDeckPair.warning() << group << "," << item
                                      << "was activated on a pair follower -- disabled";
            pControl->set(0.0);
        }
        return;
    }
    refuseAuthority(*pMember, item);
}

void DeckPairManager::refuseAuthority(Member& member, const QString& item) {
    VERIFY_OR_DEBUG_ASSERT(isLockedFollower(member)) {
        return;
    }
    kLoggerDeckPair.warning() << member.group << "," << item
                              << "is not available on a pair follower -- disabled";

    if (isSyncAuthorityItem(item)) {
        // Undo the sync request the owner queued on our behalf. Without this
        // the deck would join the sync group for one engine callback.
        member.pEngineBuffer->requestSyncMode(SyncMode::None);
    }

    ControlObject* pControl = ControlObject::getControl(
            ConfigKey(member.group, item));
    if (pControl != nullptr) {
        // Snap back. Plain set() goes through the owner which confirms the
        // value, so both the control and its engine state end up at 0.
        pControl->set(0.0);
    }
}

void DeckPairManager::slotPassthroughChanged(int deckIndex, double value) {
    if (value <= 0.0 || m_bDisbanding) {
        return;
    }
    // EngineDeck::process() short-circuits before EngineBuffer::process() while
    // passthrough is active, so the shared transport would freeze. Passthrough
    // and deck pairs are mutually exclusive: the pair is disbanded rather than
    // silently swallowing the user's passthrough request.
    Pair* pPair = activePairForDeckIndex(deckIndex);
    if (pPair == nullptr) {
        return;
    }
    const Member* pMember = memberByIndex(deckIndex);
    kLoggerDeckPair.warning() << (pMember ? pMember->group : QString())
                              << "enabled passthrough: disbanding the deck pair";
    m_bDisbanding = true;
    disbandPair(*pPair, pMember ? pMember->group : QString());
    m_bDisbanding = false;
}

DeckPairManager::Member* DeckPairManager::member(const QString& group) const {
    for (const auto& pMember : m_members) {
        if (pMember && pMember->group == group) {
            return pMember.get();
        }
    }
    return nullptr;
}

DeckPairManager::Member* DeckPairManager::memberByIndex(int deckIndex) const {
    if (deckIndex < 0 || deckIndex >= static_cast<int>(m_members.size())) {
        return nullptr;
    }
    return m_members[deckIndex].get();
}

DeckPairManager::Pair* DeckPairManager::pairForDeckIndex(int deckIndex) const {
    const int partner = partnerDeckIndex(deckIndex);
    if (partner < 0) {
        return nullptr;
    }
    const int slot = deckIndex % kPairSlotDistance;
    VERIFY_OR_DEBUG_ASSERT(slot >= 0 && slot < static_cast<int>(m_pairs.size())) {
        return nullptr;
    }
    return m_pairs[slot].get();
}

DeckPairManager::Pair* DeckPairManager::activePairForDeckIndex(int deckIndex) const {
    Pair* pPair = pairForDeckIndex(deckIndex);
    if (pPair == nullptr || !pPair->active) {
        return nullptr;
    }
    if (pPair->leaderDeckIndex != deckIndex && pPair->followerDeckIndex != deckIndex) {
        return nullptr;
    }
    return pPair;
}

bool DeckPairManager::isLockedFollower(const Member& member) const {
    return pairRole(member.group) == PairRole::PairFollower;
}

// static
void DeckPairManager::stopDeck(Deck* pDeck) {
    VERIFY_OR_DEBUG_ASSERT(pDeck != nullptr) {
        return;
    }
    ControlObject::set(ConfigKey(pDeck->getGroup(), QStringLiteral("play")), 0.0);
}

// End of autonomously AI-generated content; see the disclaimer at the top of
// this file.
