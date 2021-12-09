#pragma once

#include <climits>
#include <future>
#include <unordered_map>
#include <vector>

#include <SlippiLib/SlippiGame.h>
#include <open-vcdiff/src/google/vcdecoder.h>
#include <open-vcdiff/src/google/vcencoder.h>

#include "../../Common/CommonTypes.h"
#include "Core/ConfigManager.h"

class SlippiPlaybackStatus
{
public:
  SlippiPlaybackStatus();
  ~SlippiPlaybackStatus();

  bool m_should_jump_back = false;
  bool m_should_jump_forward = false;
  bool m_in_slippi_playback = false;
  volatile bool m_should_run_threads = false;
  bool m_hard_ffw = false;
  bool m_soft_ffw = false;
  bool m_orig_oc_enable = SConfig::GetInstance().m_OCEnable;
  float m_orig_oc_factor = SConfig::GetInstance().m_OCFactor;

  s32 m_last_ffw_frame = INT_MIN;
  s32 m_curr_playback_frame = INT_MIN;
  s32 m_target_frame_num = INT_MAX;
  s32 m_last_frame = Slippi::PLAYBACK_FIRST_SAVE;

  std::thread m_savestate_thread;

  void StartThreads(void);
  void ResetPlayback(void);
  bool ShouldFFWFrame(s32 frame_index) const;
  void PrepareSlippiPlayback(s32& frame_index);
  void SetHardFFW(bool enable);
  void SeekToFrame();

private:
  void SavestateThread(void);
  void LoadState(s32 closest_state_frame);
  void ProcessInitialState();
  void UpdateWatchSettingsStartEnd();

  std::unordered_map<int32_t, std::shared_future<std::string>>
      m_future_diffs;               // State diffs keyed by frameIndex, processed async
  std::vector<u8> m_initial_state;  // The initial state
  std::vector<u8> m_curr_state;     // The current (latest) state

  open_vcdiff::VCDiffDecoder decoder;
  open_vcdiff::VCDiffEncoder* encoder = NULL;
};
