// Copyright 2017 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#pragma once

#include <SlippiGame.h>

#include "Common/CommonTypes.h"
#include "Common/File.h"
#include "Common/FileUtil.h"
#include "Core/Slippi/SlippiGameFileLoader.h"
#include "Core/Slippi/SlippiGameReporter.h"
#include "Core/Slippi/SlippiMatchmaking.h"
#include "Core/Slippi/SlippiNetplay.h"
#include "Core/Slippi/SlippiPlayback.h"
#include "Core/Slippi/SlippiReplayComm.h"
#include "Core/Slippi/SlippiSavestate.h"
#include "Core/Slippi/SlippiSpectate.h"
#include "Core/Slippi/SlippiUser.h"
#include "EXI_Device.h"

#define ROLLBACK_MAX_FRAMES 7
#define MAX_NAME_LENGTH 15
#define CONNECT_CODE_LENGTH 8

namespace ExpansionInterface
{
// Emulated Slippi device used to receive and respond to in-game messages
class CEXISlippi : public IEXIDevice
{
public:
  CEXISlippi();
  virtual ~CEXISlippi();

  void DMAWrite(u32 _uAddr, u32 _uSize) override;
  void DMARead(u32 addr, u32 size) override;

  bool IsPresent() const override;

private:
  enum
  {
    CMD_UNKNOWN = 0x0,

    // Recording
    CMD_RECEIVE_COMMANDS = 0x35,
    CMD_RECEIVE_GAME_INFO = 0x36,
    CMD_RECEIVE_POST_FRAME_UPDATE = 0x38,
    CMD_RECEIVE_GAME_END = 0x39,
    CMD_FRAME_BOOKEND = 0x3C,
    CMD_MENU_FRAME = 0x3E,

    // Playback
    CMD_PREPARE_REPLAY = 0x75,
    CMD_READ_FRAME = 0x76,
    CMD_GET_LOCATION = 0x77,
    CMD_IS_FILE_READY = 0x88,
    CMD_IS_STOCK_STEAL = 0x89,
    CMD_GET_GECKO_CODES = 0x8A,

    // Online
    CMD_ONLINE_INPUTS = 0xB0,
    CMD_CAPTURE_SAVESTATE = 0xB1,
    CMD_LOAD_SAVESTATE = 0xB2,
    CMD_GET_MATCH_STATE = 0xB3,
    CMD_FIND_OPPONENT = 0xB4,
    CMD_SET_MATCH_SELECTIONS = 0xB5,
    CMD_OPEN_LOGIN = 0xB6,
    CMD_LOGOUT = 0xB7,
    CMD_UPDATE = 0xB8,
    CMD_GET_ONLINE_STATUS = 0xB9,
    CMD_CLEANUP_CONNECTION = 0xBA,
    CMD_SEND_CHAT_MESSAGE = 0xBB,
    CMD_GET_NEW_SEED = 0xBC,
    CMD_REPORT_GAME = 0xBD,

    // Misc
    CMD_LOG_MESSAGE = 0xD0,
    CMD_FILE_LENGTH = 0xD1,
    CMD_FILE_LOAD = 0xD2,
    CMD_GCT_LENGTH = 0xD3,
    CMD_GCT_LOAD = 0xD4,
  };

  enum
  {
    FRAME_RESP_WAIT = 0,
    FRAME_RESP_CONTINUE = 1,
    FRAME_RESP_TERMINATE = 2,
    FRAME_RESP_FASTFORWARD = 3,
  };

  std::unordered_map<u8, u32> payload_sizes = {
      // The actual size of this command will be sent in one byte
      // after the command is received. The other receive command IDs
      // and sizes will be received immediately following
      {CMD_RECEIVE_COMMANDS, 1},

      // The following are all commands used to play back a replay and
      // have fixed sizes
      {CMD_PREPARE_REPLAY, 0},
      {CMD_READ_FRAME, 4},
      {CMD_IS_STOCK_STEAL, 5},
      {CMD_GET_LOCATION, 6},
      {CMD_IS_FILE_READY, 0},
      {CMD_GET_GECKO_CODES, 0},

      // The following are used for Slippi online and also have fixed sizes
      {CMD_ONLINE_INPUTS, 17},
      {CMD_CAPTURE_SAVESTATE, 32},
      {CMD_LOAD_SAVESTATE, 32},
      {CMD_GET_MATCH_STATE, 0},
      {CMD_FIND_OPPONENT, 19},
      {CMD_SET_MATCH_SELECTIONS, 6},
      {CMD_SEND_CHAT_MESSAGE, 2},
      {CMD_OPEN_LOGIN, 0},
      {CMD_LOGOUT, 0},
      {CMD_UPDATE, 0},
      {CMD_GET_ONLINE_STATUS, 0},
      {CMD_CLEANUP_CONNECTION, 0},
      {CMD_GET_NEW_SEED, 0},
      {CMD_REPORT_GAME, 16},

      // Misc
      {CMD_LOG_MESSAGE, 0xFFFF},  // Variable size... will only work if by itself
      {CMD_FILE_LENGTH, 0x40},
      {CMD_FILE_LOAD, 0x40},
      {CMD_GCT_LENGTH, 0x0},
      {CMD_GCT_LOAD, 0x4},
  };

  struct WriteMessage
  {
    std::vector<u8> data;
    std::string operation;
  };

  // .slp File creation stuff
  u32 m_written_byte_count = 0;

  // vars for metadata generation
  time_t m_game_start_time;
  s32 m_last_frame;
  std::unordered_map<u8, std::unordered_map<u8, u32>> character_usage;

  void UpdateMetadataFields(u8* payload, u32 length);
  void ConfigureCommands(u8* payload, u8 length);
  void WriteToFileAsync(u8* payload, u32 length, std::string file_option);
  void WriteToFile(std::unique_ptr<WriteMessage> msg);
  std::vector<u8> GenerateMetadata();
  void CreateNewFile();
  void CloseFile();
  std::string GenerateFileName();
  bool CheckFrameFullyFetched(s32 frameIndex);
  // bool ShouldFFWFrame(s32 frameIndex);

  // std::ofstream log;

  File::IOFile m_file;
  std::vector<u8> m_payload;

  // online play stuff
  u16 GetRandomStage();
  bool IsDisconnected();
  void HandleOnlineInputs(u8* payload);
  void PrepareOpponentInputs(u8* payload);
  void HandleSendInputs(u8* payload);
  void HandleCaptureSavestate(u8* payload);
  void HandleLoadSavestate(u8* payload);
  void StartFindMatch(u8* payload);
  void PrepareOnlineMatchState();
  void SetMatchSelections(u8* payload);
  bool ShouldSkipOnlineFrame(s32 frame);
  void HandleLogInRequest();
  void HandleLogOutRequest();
  void HandleUpdateAppRequest();
  void PrepareOnlineStatus();
  void HandleConnectionCleanup();
  void PrepareNewSeed();
  void HandleReportGame(u8* payload);

  // replay playback stuff
  void PrepareGameInfo(u8* payload);
  void PrepareGeckoList();
  void PrepareCharacterFrameData(Slippi::FrameData* frame, u8 port, u8 is_follower);
  void PrepareFrameData(u8* payload);
  void PrepareIsStockSteal(u8* payload);
  void PrepareIsFileReady();

  // misc stuff
  void HandleChatMessage(u8* payload);
  void LogMessageFromGame(u8* payload);
  void PrepareFileLength(u8* payload);
  void PrepareFileLoad(u8* payload);
  void PrepareGctLength();
  void PrepareGctLoad(u8* payload);
  int GetCharColor(u8 char_id, u8 team_id);

  void FileWriteThread(void);

  std::queue<std::unique_ptr<WriteMessage>> m_file_write_queue;
  bool m_write_thread_running = false;
  std::thread m_file_write_thread;

  std::unordered_map<u8, std::string> GetNetplayNames();

  std::vector<u8> m_playback_savestate_payload;
  std::vector<u8> m_gecko_list;

  u32 m_stall_frame_count = 0;
  bool m_connection_stalled = false;

  std::vector<u8> m_read_queue;
  std::unique_ptr<Slippi::SlippiGame> m_current_game = nullptr;
  SlippiSpectateServer* m_slippi_server = nullptr;
  SlippiMatchmaking::MatchSearchSettings m_last_search;

  std::vector<u16> m_stage_pool;

  u32 m_frame_seq_idx = 0;

  bool m_enet_initialized = false;
  bool m_first_match = true;

  std::default_random_engine generator;

  std::string m_forced_error = "";

  // Used to determine when to detect when a new session has started
  bool m_play_session_active = false;

  // Frame skipping variables
  int m_frames_to_skip = 0;
  bool m_currently_skipping = false;

protected:
  void TransferByte(u8& byte) override;

private:
  SlippiPlayerSelections m_local_selections;

  std::unique_ptr<SlippiUser> m_user;
  std::unique_ptr<SlippiGameFileLoader> m_game_file_loader;
  std::unique_ptr<SlippiNetplayClient> m_slippi_netplay;
  std::unique_ptr<SlippiMatchmaking> m_matchmaking;
  std::unique_ptr<SlippiGameReporter> m_game_reporter;

  std::map<s32, std::unique_ptr<SlippiSavestate>> m_active_savestates;
  std::deque<std::unique_ptr<SlippiSavestate>> m_available_savestates;
};
}  // namespace ExpansionInterface
