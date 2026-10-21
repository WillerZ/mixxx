#pragma once

#include <QObject>
#include <atomic>

#include "control/pollingcontrolproxy.h"
#include "engine/controls/enginecontrol.h"
#include "engine/deckpairtransport.h"
#include "engine/sync/syncable.h"
#include "preferences/usersettings.h"

class BpmControl;
class Rotary;
class ControlTTRotary;
class ControlObject;
class ControlPotmeter;
class ControlPushButton;
class ControlProxy;
class PositionScratchController;

// RateControl is an EngineControl that is in charge of managing the rate of
// playback of a given channel of audio in the Mixxx engine. Using input from
// various controls, RateControl will calculate the current rate.
class RateControl : public EngineControl {
    Q_OBJECT
public:
  RateControl(const QString& group, UserSettingsPointer pConfig);

  // Enumerations which hold the state of the pitchbend buttons.
  // These enumerations can be used like a bitmask.
  enum class RampDirection {
      None,      // No buttons are held down
      Down,      // Down button is being held
      Up,        // Up button is being held
      DownSmall, // DownSmall button is being held
      UpSmall,   // UpSmall button is being held
  };

  enum class RampMode {
      Stepping = 0, // pitch takes a temporary step up/down a certain amount
      Linear = 1    // pitch moves up/down in a progressively linear fashion
  };

  void setBpmControl(BpmControl* bpmcontrol);

  // Returns the current engine rate.  "reportScratching" is used to tell
  // the caller that the user is currently scratching, and this is used to
  // disable keylock.
  double calculateSpeed(
          double baserate,
          double speed,
          bool paused,
          std::size_t samplesPerBuffer,
          bool* pReportScratching,
          bool* pReportReverse);

  /// Deck pairs: run the jog accumulator's consume-and-zero (including its
  /// low-pass decay) and hand the consumed value to the pair transport's
  /// accumulator lane, which the pair leader drains at its own consumption
  /// step. A locked follower skips calculateSpeed() and with it this
  /// housekeeping; without it nothing ever re-baselines the follower's `jog`
  /// control and the value-copy relays of the pair bridge would feed stale
  /// state. Called by EngineBuffer on the engine thread once per callback
  /// while the follower is locked (DECK_PAIR_PLAN.md 5.4.1 / 9.13).
  void pairFollowerConsumeAccumulators();

  /// Deck pairs: attach the shared transport lane for the `jog` accumulator.
  /// The pair leader drains the lane inside its own jog consumption, the pair
  /// follower only receives the pointer so the consume-and-zero above can
  /// publish through it; an unpaired deck gets neither (nullptr, Unpaired).
  /// Called by EngineBuffer when the pair role is handed over, i.e. on the
  /// main thread while the pair is disarmed or before it becomes active.
  void setPairNudgeLane(EnginePairTransport* pTransport, PairRole role);

  // Set rate change when temp rate button is pressed
  static void setTemporaryRateChangeCoarseAmount(double v);
  static double getTemporaryRateChangeCoarseAmount();
  // Set rate change when temp rate small button is pressed
  static void setTemporaryRateChangeFineAmount(double v);
  static double getTemporaryRateChangeFineAmount();
  // Set rate change when perm rate button is pressed
  static void setPermanentRateChangeCoarseAmount(double v);
  static double getPermanentRateChangeCoarseAmount();
  // Set rate change when perm rate small button is pressed
  static void setPermanentRateChangeFineAmount(double v);
  static double getPermanentRateChangeFineAmount();
  // Set Rate Ramp Mode
  static void setRateRampMode(RampMode mode);
  static RampMode getRateRampMode();
  // Set Rate Ramp Sensitivity
  static void setRateRampSensitivity(int);
  static int getRateRampSensitivity();
  bool isReverseButtonPressed();
  // ReadAheadManager::getNextSamples() notifies us each time the play position
  // wrapped around during one buffer process (beatloop or track repeat) so
  // PositionScratchController can correctly interpret the sample position delta.
  void notifyWrapAround(mixxx::audio::FramePos triggerPos,
          mixxx::audio::FramePos targetPos);
  void notifySeek(mixxx::audio::FramePos position) override;
  void resetPositionScratchController();

public slots:
  void slotRateRangeChanged(double);
  void slotRateSliderChanged(double);
  void slotRateRatioChanged(double);
  void slotReverseRollActivate(double);
  void slotControlRatePermDown(double);
  void slotControlRatePermDownSmall(double);
  void slotControlRatePermUp(double);
  void slotControlRatePermUpSmall(double);
  void slotControlFastForward(double);
  void slotControlFastBack(double);

private:
  bool isPairLockedFollower();
  void processTempRate(const size_t bufferSamples);
  /// Read the `jog` accumulator and re-baseline it, the way a single deck's
  /// consumer does. Returns the raw accumulated value.
  double consumeJogAccumulator();
  double getJogFactor();
  double getWheelFactor() const;
  SyncMode getSyncMode() const;

  // Set rate change of the temporary pitch rate
  void setRateTemp(double v);
  // Add a value to the temporary pitch rate
  void addRateTemp(double v);
  // Subtract a value from the temporary pitch rate
  void subRateTemp(double v);
  // Reset the temporary pitch rate
  void resetRateTemp(void);
  // Get the 'Raw' Temp Rate
  double getTempRate(void);

  // For Sync Lock
  BpmControl* m_pBpmControl;

  PollingControlProxy m_pSampleRate;
  std::unique_ptr<ControlObject> m_pRateRatio;
  std::unique_ptr<ControlObject> m_pRateDir;
  std::unique_ptr<ControlObject> m_pRateRange;
  std::unique_ptr<ControlPotmeter> m_pRateSlider;
  std::unique_ptr<ControlPotmeter> m_pRateSearch;

  std::unique_ptr<ControlPushButton> m_pButtonRateTempDown;
  std::unique_ptr<ControlPushButton> m_pButtonRateTempDownSmall;
  std::unique_ptr<ControlPushButton> m_pButtonRateTempUp;
  std::unique_ptr<ControlPushButton> m_pButtonRateTempUpSmall;

  std::unique_ptr<ControlPushButton> m_pButtonRatePermDown;
  std::unique_ptr<ControlPushButton> m_pButtonRatePermDownSmall;
  std::unique_ptr<ControlPushButton> m_pButtonRatePermUp;
  std::unique_ptr<ControlPushButton> m_pButtonRatePermUpSmall;

  std::unique_ptr<ControlPushButton> m_pReverseButton;
  std::unique_ptr<ControlPushButton> m_pReverseRollButton;
  std::unique_ptr<ControlObject> m_pForwardButton;
  std::unique_ptr<ControlObject> m_pBackButton;

  std::unique_ptr<ControlTTRotary> m_pWheel;
  std::unique_ptr<ControlObject> m_pScratch2;
  std::unique_ptr<ControlPushButton> m_pScratch2Enable;
  std::unique_ptr<ControlObject> m_pScratch2Scratching;

  std::unique_ptr<PositionScratchController> m_pScratchController;

  std::unique_ptr<ControlObject> m_pJog;
  std::unique_ptr<Rotary> m_pJogFilter;

  /// Deck pairs: the shared `jog` accumulator lane. The leader claims from it
  /// (`m_bPairClaimsNudge`), the follower publishes into it from
  /// pairFollowerConsumeAccumulators(); both only while the pair is active.
  /// Engine thread reads the pair, main thread writes it at pair formation and
  /// teardown, hence the atomics.
  std::atomic<EnginePairTransport*> m_pPairNudge{nullptr};
  std::atomic<bool> m_bPairClaimsNudge{false};

  ControlObject* m_pVCEnabled;
  ControlObject* m_pVCScratching;
  ControlObject* m_pVCMode;

  PollingControlProxy m_syncMode;
  PollingControlProxy m_slipEnabled;

  // Values used when temp and perm rate buttons are pressed
  static ControlValueAtomic<double> m_dTemporaryRateChangeCoarse;
  static ControlValueAtomic<double> m_dTemporaryRateChangeFine;
  static ControlValueAtomic<double> m_dPermanentRateChangeCoarse;
  static ControlValueAtomic<double> m_dPermanentRateChangeFine;

  int m_wrapAroundCount;
  mixxx::audio::FramePos m_jumpPos;
  mixxx::audio::FramePos m_targetPos;

  // This is true if we've already started to ramp the rate
  bool m_bTempStarted;
  // Set the Temporary Rate Change Mode
  static RampMode m_eRateRampMode;
  // The Rate Temp Sensitivity, the higher it is the slower it gets
  static int m_iRateRampSensitivity;
  // Factor applied to the deprecated "wheel" rate value.
  static const double kWheelMultiplier;
  // Factor applied to jogwheels when the track is paused to speed up seeking.
  static const double kPausedJogMultiplier;
  // Temporary pitchrate, added to the permanent rate for calculateRate
  double m_tempRateRatio;
  // Speed for temporary rate change
  double m_dRateTempRampChange;
};
