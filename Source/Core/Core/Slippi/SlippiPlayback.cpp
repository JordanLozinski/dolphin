#include <memory>
#include <mutex>

#ifdef _WIN32
#include <share.h>
#endif

#include "Common/Logging/Log.h"
#include "Core/Core.h"
#include "Core/HW/EXI/EXI_DeviceSlippi.h"
#include "Core/NetPlayClient.h"
#include "Core/State.h"
#include "SlippiPlayback.h"

#define FRAME_INTERVAL 900
#define SLEEP_TIME_MS 8

std::unique_ptr<SlippiPlaybackStatus> g_playback_status;
extern std::unique_ptr<SlippiReplayComm> g_replay_comm;

static std::mutex mtx;
static std::mutex seekMtx;
static std::mutex ffwMtx;
static std::mutex diffMtx;
static std::unique_lock<std::mutex> processingLock(diffMtx);
static std::condition_variable condVar;
static std::condition_variable cv_waitingForTargetFrame;
static std::condition_variable cv_processingDiff;
static std::atomic<int> numDiffsProcessing(0);

s32 emod(s32 a, s32 b)
{
  assert(b != 0);
  int r = a % b;
  return r >= 0 ? r : r + std::abs(b);
}

std::string processDiff(std::vector<u8> iState, std::vector<u8> cState)
{
  INFO_LOG(SLIPPI, "Processing diff");
  numDiffsProcessing += 1;
  cv_processingDiff.notify_one();
  std::string diff = std::string();
  open_vcdiff::VCDiffEncoder encoder((char*)iState.data(), iState.size());
  encoder.Encode((char*)cState.data(), cState.size(), &diff);

  INFO_LOG(SLIPPI, "done processing");
  numDiffsProcessing -= 1;
  cv_processingDiff.notify_one();
  return diff;
}

SlippiPlaybackStatus::SlippiPlaybackStatus()
{
  m_should_jump_back = false;
  m_should_jump_forward = false;
  m_in_slippi_playback = false;
  m_should_run_threads = false;
  m_hard_ffw = false;
  m_soft_ffw = false;
  m_last_ffw_frame = INT_MIN;
  m_curr_playback_frame = INT_MIN;
  m_target_frame_num = INT_MAX;
  m_last_frame = Slippi::PLAYBACK_FIRST_SAVE;
}

void SlippiPlaybackStatus::StartThreads()
{
  m_should_run_threads = true;
  m_savestate_thread = std::thread(&SlippiPlaybackStatus::SavestateThread, this);
}

void SlippiPlaybackStatus::PrepareSlippiPlayback(s32& frameIndex)
{
  // block if there's too many diffs being processed
  while (m_should_run_threads && numDiffsProcessing > 2)
  {
    INFO_LOG(SLIPPI, "Processing too many diffs, blocking main process");
    cv_processingDiff.wait(processingLock);
  }

  // Unblock thread to save a state every interval
  if (m_should_run_threads && ((m_curr_playback_frame + 122) % FRAME_INTERVAL == 0))
    condVar.notify_one();

  // TODO: figure out why sometimes playback frame increments past m_target_frame_num
  if (m_in_slippi_playback && frameIndex >= m_target_frame_num)
  {
    INFO_LOG(SLIPPI, "Reached frame %d. Target was %d. Unblocking", frameIndex, m_target_frame_num);
    cv_waitingForTargetFrame.notify_one();
  }
}

void SlippiPlaybackStatus::ResetPlayback()
{
  if (m_should_run_threads)
  {
    m_should_run_threads = false;

    if (m_savestate_thread.joinable())
      m_savestate_thread.detach();

    condVar.notify_one();  // Will allow thread to kill itself
    m_future_diffs.clear();
    m_future_diffs.rehash(0);
  }

  m_should_jump_back = false;
  m_should_jump_forward = false;
  m_hard_ffw = false;
  m_soft_ffw = false;
  m_target_frame_num = INT_MAX;
  m_in_slippi_playback = false;
}

void SlippiPlaybackStatus::ProcessInitialState()
{
  INFO_LOG(SLIPPI, "saving m_initial_state");
  State::SaveToBuffer(m_initial_state);
  // The initial save to m_curr_state causes a stutter of about 5-10 frames
  // Doing it here to get it out of the way and prevent stutters later
  // Subsequent calls to SaveToBuffer for m_curr_state take ~1 frame
  State::SaveToBuffer(m_curr_state);
  if (SConfig::GetInstance().m_slippiEnableSeek)
  {
    SConfig::GetInstance().bHideCursor = false;
  }
};

void SlippiPlaybackStatus::SavestateThread()
{
  Common::SetCurrentThreadName("Savestate thread");
  std::unique_lock<std::mutex> intervalLock(mtx);

  INFO_LOG(SLIPPI, "Entering savestate thread");

  while (m_should_run_threads)
  {
    // Wait to hit one of the intervals
    // Possible while rewinding that we hit this wait again.
    while (m_should_run_threads &&
           (m_curr_playback_frame - Slippi::PLAYBACK_FIRST_SAVE) % FRAME_INTERVAL != 0)
      condVar.wait(intervalLock);

    if (!m_should_run_threads)
      break;

    s32 fixedFrameNumber = m_curr_playback_frame;
    if (fixedFrameNumber == INT_MAX)
      continue;

    bool isStartFrame = fixedFrameNumber == Slippi::PLAYBACK_FIRST_SAVE;
    bool hasStateBeenProcessed = m_future_diffs.count(fixedFrameNumber) > 0;

    if (!m_in_slippi_playback && isStartFrame)
    {
      ProcessInitialState();
      m_in_slippi_playback = true;
    }
    else if (SConfig::GetInstance().m_slippiEnableSeek && !hasStateBeenProcessed && !isStartFrame)
    {
      INFO_LOG(SLIPPI, "saving diff at frame: %d", fixedFrameNumber);
      State::SaveToBuffer(m_curr_state);

      m_future_diffs[fixedFrameNumber] = std::async(processDiff, m_initial_state, m_curr_state);
    }
    Common::SleepCurrentThread(SLEEP_TIME_MS);
  }

  INFO_LOG(SLIPPI, "Exiting savestate thread");
}

void SlippiPlaybackStatus::SeekToFrame()
{
  if (seekMtx.try_lock())
  {
    if (m_target_frame_num < Slippi::PLAYBACK_FIRST_SAVE)
      m_target_frame_num = Slippi::PLAYBACK_FIRST_SAVE;

    if (m_target_frame_num > m_last_frame)
    {
      m_target_frame_num = m_last_frame;
    }

    std::unique_lock<std::mutex> ffwLock(ffwMtx);
    auto replayCommSettings = g_replay_comm->getSettings();
    if (replayCommSettings.mode == "queue")
      UpdateWatchSettingsStartEnd();

    auto prevState = Core::GetState();
    if (prevState != Core::State::Paused)
      Core::SetState(Core::State::Paused);

    s32 closestStateFrame =
        m_target_frame_num - emod(m_target_frame_num - Slippi::PLAYBACK_FIRST_SAVE, FRAME_INTERVAL);
    bool isLoadingStateOptimal =
        m_target_frame_num < m_curr_playback_frame || closestStateFrame > m_curr_playback_frame;

    if (isLoadingStateOptimal)
    {
      if (closestStateFrame <= Slippi::PLAYBACK_FIRST_SAVE)
      {
        State::LoadFromBuffer(m_initial_state);
      }
      else
      {
        // If this diff exists, load it
        if (m_future_diffs.count(closestStateFrame) > 0)
        {
          LoadState(closestStateFrame);
        }
        else if (m_target_frame_num < m_curr_playback_frame)
        {
          s32 closestActualStateFrame = closestStateFrame - FRAME_INTERVAL;
          while (closestActualStateFrame > Slippi::PLAYBACK_FIRST_SAVE &&
                 m_future_diffs.count(closestActualStateFrame) == 0)
            closestActualStateFrame -= FRAME_INTERVAL;
          LoadState(closestActualStateFrame);
        }
        else if (m_target_frame_num > m_curr_playback_frame)
        {
          s32 closestActualStateFrame = closestStateFrame - FRAME_INTERVAL;
          while (closestActualStateFrame > m_curr_playback_frame &&
                 m_future_diffs.count(closestActualStateFrame) == 0)
            closestActualStateFrame -= FRAME_INTERVAL;

          // only load a savestate if we find one past our current frame since we are seeking
          // forwards
          if (closestActualStateFrame > m_curr_playback_frame)
            LoadState(closestActualStateFrame);
        }
      }
    }

    // Fastforward until we get to the frame we want
    if (m_target_frame_num != closestStateFrame && m_target_frame_num != m_last_frame)
    {
      SetHardFFW(true);
      Core::SetState(Core::State::Running);
      cv_waitingForTargetFrame.wait(ffwLock);
      Core::SetState(Core::State::Paused);
      SetHardFFW(false);
    }

    // We've reached the frame we want. Reset m_target_frame_num and release mutex so another seek
    // can be performed
    g_playback_status->m_curr_playback_frame = m_target_frame_num;
    m_target_frame_num = INT_MAX;
    Core::SetState(prevState);
    seekMtx.unlock();
  }
  else
  {
    INFO_LOG(SLIPPI, "Already seeking. Ignoring this call");
  }
}

// Set m_hard_ffw and update OC settings to speed up the FFW
void SlippiPlaybackStatus::SetHardFFW(bool enable)
{
  if (enable)
  {
    SConfig::GetInstance().m_OCEnable = true;
    SConfig::GetInstance().m_OCFactor = 4.0f;
  }
  else
  {
    SConfig::GetInstance().m_OCFactor = m_orig_oc_factor;
    SConfig::GetInstance().m_OCEnable = m_orig_oc_enable;
  }

  m_hard_ffw = enable;
}

void SlippiPlaybackStatus::LoadState(s32 closestStateFrame)
{
  if (closestStateFrame == Slippi::PLAYBACK_FIRST_SAVE)
    State::LoadFromBuffer(m_initial_state);
  else
  {
    std::string stateString;
    decoder.Decode((char*)m_initial_state.data(), m_initial_state.size(),
                   m_future_diffs[closestStateFrame].get(), &stateString);
    std::vector<u8> stateToLoad(stateString.begin(), stateString.end());
    State::LoadFromBuffer(stateToLoad);
  }
}

bool SlippiPlaybackStatus::ShouldFFWFrame(s32 frameIndex) const
{
  if (!m_soft_ffw && !m_hard_ffw)
  {
    // If no FFW at all, don't FFW this frame
    return false;
  }

  if (m_hard_ffw)
  {
    // For a hard FFW, always FFW until it's turned off
    return true;
  }

  // Here we have a soft FFW, we only want to turn on FFW for single frames once
  // every X frames to FFW in a more smooth manner
  return (frameIndex - m_last_ffw_frame) >= 15;
}

void SlippiPlaybackStatus::UpdateWatchSettingsStartEnd()
{
  int startFrame = g_replay_comm->current.startFrame;
  int endFrame = g_replay_comm->current.endFrame;
  if (startFrame != Slippi::GAME_FIRST_FRAME || endFrame != INT_MAX)
  {
    if (g_playback_status->m_target_frame_num < startFrame)
      g_replay_comm->current.startFrame = g_playback_status->m_target_frame_num;
    if (g_playback_status->m_target_frame_num > endFrame)
      g_replay_comm->current.endFrame = INT_MAX;
  }
}

SlippiPlaybackStatus::~SlippiPlaybackStatus()
{
  // Kill threads to prevent cleanup crash
  ResetPlayback();
}
