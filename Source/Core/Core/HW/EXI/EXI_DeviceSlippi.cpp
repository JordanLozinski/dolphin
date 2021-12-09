// Copyright 2017 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#include <algorithm>
#include <semver/include/semver200.h>
#include <utility>  // std::move

#include "Common/CommonPaths.h"
#include "Common/CommonTypes.h"

#include "Common/Logging/Log.h"
#include "Common/MemoryUtil.h"
#include "Common/MsgHandler.h"
#include "Common/StringUtil.h"
#include "Common/Thread.h"
#include "Common/Version.h"

#include "Core/ConfigManager.h"
#include "Core/Core.h"
#include "Core/CoreTiming.h"
#include "Core/Debugger/Debugger_SymbolMap.h"
#include "Core/GeckoCode.h"
#include "Core/HW/EXI/EXI_DeviceSlippi.h"
#include "Core/HW/Memmap.h"
#include "Core/HW/SystemTimers.h"
#include "Core/Host.h"
#include "Core/NetPlayClient.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/Slippi/SlippiPlayback.h"
#include "Core/Slippi/SlippiReplayComm.h"
#include "Core/State.h"

#define FRAME_INTERVAL 900
#define SLEEP_TIME_MS 8
#define WRITE_FILE_SLEEP_TIME_MS 85

//#define LOCAL_TESTING
//#define CREATE_DIFF_FILES
extern std::unique_ptr<SlippiPlaybackStatus> g_playback_status;
extern std::unique_ptr<SlippiReplayComm> g_replay_comm;
extern bool g_need_input_for_frame;

#ifdef LOCAL_TESTING
bool is_local_connected = false;
int local_chat_msg_id = 0;
#endif

namespace ExpansionInterface
{
static std::unordered_map<u8, std::string> slippi_names;
static std::unordered_map<u8, std::string> slippi_connect_codes;

template <typename T>
bool IsFutureReady(std::future<T>& t)
{
  return t.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
}

std::vector<u8> uint16ToVector(u16 num)
{
  u8 byte0 = num >> 8;
  u8 byte1 = num & 0xFF;

  return std::vector<u8>({byte0, byte1});
}

std::vector<u8> uint32ToVector(u32 num)
{
  u8 byte0 = num >> 24;
  u8 byte1 = (num & 0xFF0000) >> 16;
  u8 byte2 = (num & 0xFF00) >> 8;
  u8 byte3 = num & 0xFF;

  return std::vector<u8>({byte0, byte1, byte2, byte3});
}

std::vector<u8> int32ToVector(int32_t num)
{
  u8 byte0 = num >> 24;
  u8 byte1 = (num & 0xFF0000) >> 16;
  u8 byte2 = (num & 0xFF00) >> 8;
  u8 byte3 = num & 0xFF;

  return std::vector<u8>({byte0, byte1, byte2, byte3});
}

void AppendWordToBuffer(std::vector<u8>* buf, u32 word)
{
  auto word_vector = uint32ToVector(word);
  buf->insert(buf->end(), word_vector.begin(), word_vector.end());
}

void AppendHalfToBuffer(std::vector<u8>* buf, u16 word)
{
  auto half_vector = uint16ToVector(word);
  buf->insert(buf->end(), half_vector.begin(), half_vector.end());
}

std::string ConvertConnectCodeForGame(const std::string& input)
{
  // Shift-Jis '#' symbol is two bytes (0x8194), followed by 0x00 null terminator
  char full_width_shift_jis_hashtag[] = {-127, -108, 0};  // 0x81, 0x94, 0x00
  std::string connect_code(input);
  // SLIPPITODO：Not the best use of ReplaceAll. potential bug if more than one '#' found.
  connect_code = ReplaceAll(connect_code, "#", std::string(full_width_shift_jis_hashtag));
  connect_code.resize(CONNECT_CODE_LENGTH +
                      2);  // fixed length + full width (two byte) hashtag +1, null terminator +1
  return connect_code;
}

CEXISlippi::CEXISlippi()
{
  INFO_LOG(SLIPPI, "EXI SLIPPI Constructor called.");

  m_user = std::make_unique<SlippiUser>();
  g_playback_status = std::make_unique<SlippiPlaybackStatus>();
  m_matchmaking = std::make_unique<SlippiMatchmaking>(m_user.get());
  m_game_file_loader = std::make_unique<SlippiGameFileLoader>();
  m_game_reporter = std::make_unique<SlippiGameReporter>(m_user.get());
  g_replay_comm = std::make_unique<SlippiReplayComm>();

  generator = std::default_random_engine(Common::Timer::GetTimeMs());

  // Loggers will check 5 bytes, make sure we own that memory
  m_read_queue.reserve(5);

  // Initialize local selections to empty
  m_local_selections.Reset();

  // Forces savestate to re-init regions when a new ISO is loaded
  SlippiSavestate::should_force_init = true;

  // Update user file and then listen for User
#ifndef IS_PLAYBACK
  m_user->ListenForLogIn();
#endif

#ifdef CREATE_DIFF_FILES
  // MnMaAll.usd
  std::string origStr;
  std::string modifiedStr;
  File::ReadFileToString(
      "C:\\Users\\Jas\\Documents\\Melee\\Textures\\Slippi\\MainMenu\\MnMaAll.usd", origStr);
  File::ReadFileToString(
      "C:\\Users\\Jas\\Documents\\Melee\\Textures\\Slippi\\MainMenu\\MnMaAll-new.usd", modifiedStr);
  std::vector<u8> orig(origStr.begin(), origStr.end());
  std::vector<u8> modified(modifiedStr.begin(), modifiedStr.end());
  auto diff = processDiff(orig, modified);
  File::WriteStringToFile(
      diff, "C:\\Users\\Jas\\Documents\\Melee\\Textures\\Slippi\\MainMenu\\MnMaAll.usd.diff");
  File::WriteStringToFile(diff, "C:\\Dolphin\\IshiiDev\\Sys\\GameFiles\\GALE01\\MnMaAll.usd.diff");

  // MnExtAll.usd
  File::ReadFileToString("C:\\Users\\Jas\\Documents\\Melee\\Textures\\Slippi\\CSS\\MnExtAll.usd",
                         origStr);
  File::ReadFileToString(
      "C:\\Users\\Jas\\Documents\\Melee\\Textures\\Slippi\\CSS\\MnExtAll-new.usd", modifiedStr);
  orig = std::vector<u8>(origStr.begin(), origStr.end());
  modified = std::vector<u8>(modifiedStr.begin(), modifiedStr.end());
  diff = processDiff(orig, modified);
  File::WriteStringToFile(
      diff, "C:\\Users\\Jas\\Documents\\Melee\\Textures\\Slippi\\CSS\\MnExtAll.usd.diff");
  File::WriteStringToFile(diff, "C:\\Dolphin\\IshiiDev\\Sys\\GameFiles\\GALE01\\MnExtAll.usd.diff");

  // SdMenu.usd
  File::ReadFileToString("C:\\Users\\Jas\\Documents\\Melee\\Textures\\Slippi\\MainMenu\\SdMenu.usd",
                         origStr);
  File::ReadFileToString(
      "C:\\Users\\Jas\\Documents\\Melee\\Textures\\Slippi\\MainMenu\\SdMenu-new.usd", modifiedStr);
  orig = std::vector<u8>(origStr.begin(), origStr.end());
  modified = std::vector<u8>(modifiedStr.begin(), modifiedStr.end());
  diff = processDiff(orig, modified);
  File::WriteStringToFile(
      diff, "C:\\Users\\Jas\\Documents\\Melee\\Textures\\Slippi\\MainMenu\\SdMenu.usd.diff");
  File::WriteStringToFile(diff, "C:\\Dolphin\\IshiiDev\\Sys\\GameFiles\\GALE01\\SdMenu.usd.diff");

  // Japanese Files
  // MnMaAll.dat
  File::ReadFileToString(
      "C:\\Users\\Jas\\Documents\\Melee\\Textures\\Slippi\\MainMenu\\MnMaAll.dat", origStr);
  File::ReadFileToString(
      "C:\\Users\\Jas\\Documents\\Melee\\Textures\\Slippi\\MainMenu\\MnMaAll-new.dat", modifiedStr);
  orig = std::vector<u8>(origStr.begin(), origStr.end());
  modified = std::vector<u8>(modifiedStr.begin(), modifiedStr.end());
  diff = processDiff(orig, modified);
  File::WriteStringToFile(
      diff, "C:\\Users\\Jas\\Documents\\Melee\\Textures\\Slippi\\MainMenu\\MnMaAll.dat.diff");
  File::WriteStringToFile(diff, "C:\\Dolphin\\IshiiDev\\Sys\\GameFiles\\GALE01\\MnMaAll.dat.diff");

  // MnExtAll.dat
  File::ReadFileToString("C:\\Users\\Jas\\Documents\\Melee\\Textures\\Slippi\\CSS\\MnExtAll.dat",
                         origStr);
  File::ReadFileToString(
      "C:\\Users\\Jas\\Documents\\Melee\\Textures\\Slippi\\CSS\\MnExtAll-new.dat", modifiedStr);
  orig = std::vector<u8>(origStr.begin(), origStr.end());
  modified = std::vector<u8>(modifiedStr.begin(), modifiedStr.end());
  diff = processDiff(orig, modified);
  File::WriteStringToFile(
      diff, "C:\\Users\\Jas\\Documents\\Melee\\Textures\\Slippi\\CSS\\MnExtAll.dat.diff");
  File::WriteStringToFile(diff, "C:\\Dolphin\\IshiiDev\\Sys\\GameFiles\\GALE01\\MnExtAll.dat.diff");

  // SdMenu.dat
  File::ReadFileToString("C:\\Users\\Jas\\Documents\\Melee\\Textures\\Slippi\\MainMenu\\SdMenu.dat",
                         origStr);
  File::ReadFileToString(
      "C:\\Users\\Jas\\Documents\\Melee\\Textures\\Slippi\\MainMenu\\SdMenu-new.dat", modifiedStr);
  orig = std::vector<u8>(origStr.begin(), origStr.end());
  modified = std::vector<u8>(modifiedStr.begin(), modifiedStr.end());
  diff = processDiff(orig, modified);
  File::WriteStringToFile(
      diff, "C:\\Users\\Jas\\Documents\\Melee\\Textures\\Slippi\\MainMenu\\SdMenu.dat.diff");
  File::WriteStringToFile(diff, "C:\\Dolphin\\IshiiDev\\Sys\\GameFiles\\GALE01\\SdMenu.dat.diff");

  // TEMP - Restore orig
  // std::string stateString;
  // decoder.Decode((char *)orig.data(), orig.size(), diff, &stateString);
  // File::WriteStringToFile(stateString,
  //                        "C:\\Users\\Jas\\Documents\\Melee\\Textures\\Slippi\\MainMenu\\MnMaAll-restored.usd");
#endif
}

CEXISlippi::~CEXISlippi()
{
  u8 empty[1];

  // Closes file gracefully to prevent file corruption when emulation
  // suddenly stops. This would happen often on netplay when the opponent
  // would close the emulation before the file successfully finished writing
  WriteToFileAsync(&empty[0], 0, "close");
  m_write_thread_running = false;
  if (m_file_write_thread.joinable())
  {
    m_file_write_thread.join();
  }

  SlippiSpectateServer::getInstance().write(&empty[0], 0);
  SlippiSpectateServer::getInstance().endGame();

  m_local_selections.Reset();

  g_playback_status->ResetPlayback();

  // TODO: ENET shutdown should maybe be done at app shutdown instead.
  // Right now this might be problematic in the case where someone starts a netplay client
  // and then queues into online matchmaking, and then stops the game. That might deinit
  // the ENET libraries so that they can't be used anymore for the netplay lobby? Course
  // you'd have to be kinda dumb to do that sequence of stuff anyway so maybe it's nbd
  if (m_enet_initialized)
    enet_deinitialize();
}

void CEXISlippi::ConfigureCommands(u8* payload, u8 length)
{
  for (int i = 1; i < length; i += 3)
  {
    // Go through the receive commands payload and set up other commands
    u8 command_byte = payload[i];
    u32 command_payload_size = payload[i + 1] << 8 | payload[i + 2];
    payload_sizes[command_byte] = command_payload_size;
  }
}

void CEXISlippi::UpdateMetadataFields(u8* payload, u32 length)
{
  if (length <= 0 || payload[0] != CMD_RECEIVE_POST_FRAME_UPDATE)
  {
    // Only need to update if this is a post frame update
    return;
  }

  // Keep track of last frame
  m_last_frame = payload[1] << 24 | payload[2] << 16 | payload[3] << 8 | payload[4];

  // Keep track of character usage
  u8 player_index = payload[5];
  u8 internalCharacterId = payload[7];
  if (!character_usage.count(player_index) ||
      !character_usage[player_index].count(internalCharacterId))
  {
    character_usage[player_index][internalCharacterId] = 0;
  }
  character_usage[player_index][internalCharacterId] += 1;
}

std::unordered_map<u8, std::string> CEXISlippi::GetNetplayNames()
{
  std::unordered_map<u8, std::string> names;

  if (slippi_names.size())
  {
    names = slippi_names;
  }

  return names;
}

std::vector<u8> CEXISlippi::GenerateMetadata()
{
  std::vector<u8> metadata({'U', 8, 'm', 'e', 't', 'a', 'd', 'a', 't', 'a', '{'});

  // TODO: Abstract out UBJSON functions to make this cleaner

  // Add game start time
  u8 date_time_str_len = sizeof "2011-10-08T07:07:09Z";
  std::vector<char> date_time_buf(date_time_str_len);
  strftime(&date_time_buf[0], date_time_str_len, "%FT%TZ", gmtime(&m_game_start_time));
  date_time_buf.pop_back();  // Removes the \0 from the back of string
  metadata.insert(metadata.end(),
                  {'U', 7, 's', 't', 'a', 'r', 't', 'A', 't', 'S', 'U', (u8)date_time_buf.size()});
  metadata.insert(metadata.end(), date_time_buf.begin(), date_time_buf.end());

  // Add game duration
  std::vector<u8> last_frame_to_write = int32ToVector(m_last_frame);
  metadata.insert(metadata.end(), {'U', 9, 'l', 'a', 's', 't', 'F', 'r', 'a', 'm', 'e', 'l'});
  metadata.insert(metadata.end(), last_frame_to_write.begin(), last_frame_to_write.end());

  // Add players elements to metadata, one per player index
  metadata.insert(metadata.end(), {'U', 7, 'p', 'l', 'a', 'y', 'e', 'r', 's', '{'});

  auto player_names = GetNetplayNames();

  for (auto it = character_usage.begin(); it != character_usage.end(); ++it)
  {
    auto player_index = it->first;
    auto player_character_usage = it->second;

    metadata.push_back('U');
    std::string player_index_str = std::to_string(player_index);
    metadata.push_back((u8)player_index_str.length());
    metadata.insert(metadata.end(), player_index_str.begin(), player_index_str.end());
    metadata.push_back('{');

    // Add names element for this player
    metadata.insert(metadata.end(), {'U', 5, 'n', 'a', 'm', 'e', 's', '{'});

    if (player_names.count(player_index))
    {
      auto player_name = player_names[player_index];
      // Add netplay element for this player name
      metadata.insert(metadata.end(), {'U', 7, 'n', 'e', 't', 'p', 'l', 'a', 'y', 'S', 'U'});
      metadata.push_back((u8)player_name.length());
      metadata.insert(metadata.end(), player_name.begin(), player_name.end());
    }

    if (slippi_connect_codes.count(player_index))
    {
      auto connect_code = slippi_connect_codes[player_index];
      // Add connection code element for this player name
      metadata.insert(metadata.end(), {'U', 4, 'c', 'o', 'd', 'e', 'S', 'U'});
      metadata.push_back((u8)connect_code.length());
      metadata.insert(metadata.end(), connect_code.begin(), connect_code.end());
    }

    metadata.push_back('}');  // close names

    // Add character element for this player
    metadata.insert(metadata.end(),
                    {'U', 10, 'c', 'h', 'a', 'r', 'a', 'c', 't', 'e', 'r', 's', '{'});
    for (auto it2 = player_character_usage.begin(); it2 != player_character_usage.end(); ++it2)
    {
      metadata.push_back('U');
      std::string internal_char_id_str = std::to_string(it2->first);
      metadata.push_back((u8)internal_char_id_str.length());
      metadata.insert(metadata.end(), internal_char_id_str.begin(), internal_char_id_str.end());

      metadata.push_back('l');
      std::vector<u8> frame_count = uint32ToVector(it2->second);
      metadata.insert(metadata.end(), frame_count.begin(), frame_count.end());
    }
    metadata.push_back('}');  // close characters

    metadata.push_back('}');  // close player
  }
  metadata.push_back('}');

  // Indicate this was played on dolphin
  metadata.insert(metadata.end(), {'U', 8,   'p', 'l', 'a', 'y', 'e', 'd', 'O', 'n',
                                   'S', 'U', 7,   'd', 'o', 'l', 'p', 'h', 'i', 'n'});

  metadata.push_back('}');
  return metadata;
}

void CEXISlippi::WriteToFileAsync(u8* payload, u32 length, std::string file_option)
{
  if (!SConfig::GetInstance().m_slippiSaveReplays)
  {
    return;
  }

  if (file_option == "create" && !m_write_thread_running)
  {
    WARN_LOG(SLIPPI, "Creating file write thread...");
    m_write_thread_running = true;
    m_file_write_thread = std::thread(&CEXISlippi::FileWriteThread, this);
  }

  if (!m_write_thread_running)
  {
    return;
  }

  std::vector<u8> payload_data;
  payload_data.insert(payload_data.end(), payload, payload + length);

  auto write_msg = std::make_unique<WriteMessage>();
  write_msg->data = payload_data;
  write_msg->operation = file_option;

  m_file_write_queue.push(std::move(write_msg));
}

void CEXISlippi::FileWriteThread(void)
{
  while (m_write_thread_running || !m_file_write_queue.empty())
  {
    // Process all messages
    while (!m_file_write_queue.empty())
    {
      WriteToFile(std::move(m_file_write_queue.front()));
      m_file_write_queue.pop();

      Common::SleepCurrentThread(0);
    }

    Common::SleepCurrentThread(WRITE_FILE_SLEEP_TIME_MS);
  }
}

void CEXISlippi::WriteToFile(std::unique_ptr<WriteMessage> msg)
{
  if (!msg)
  {
    ERROR_LOG(SLIPPI, "Unexpected error: write message is falsy.");
    return;
  }

  u8* payload = &msg->data[0];
  u32 length = (u32)msg->data.size();
  std::string file_option = msg->operation;

  std::vector<u8> data_to_write;
  if (file_option == "create")
  {
    // If the game sends over option 1 that means a file should be created
    CreateNewFile();

    // Start ubjson file and prepare the "raw" element that game
    // data output will be dumped into. The size of the raw output will
    // be initialized to 0 until all of the data has been received
    std::vector<u8> header_bytes({'{', 'U', 3, 'r', 'a', 'w', '[', '$', 'U', '#', 'l', 0, 0, 0, 0});
    data_to_write.insert(data_to_write.end(), header_bytes.begin(), header_bytes.end());

    // Used to keep track of how many bytes have been written to the file
    m_written_byte_count = 0;

    // Used to track character usage (sheik/zelda)
    character_usage.clear();

    // Reset m_last_frame
    m_last_frame = Slippi::GAME_FIRST_FRAME;

    // Get display names and connection codes from slippi netplay client
    if (m_slippi_netplay)
    {
      auto player_info = m_matchmaking->GetPlayerInfo();

      for (int i = 0; i < player_info.size(); i++)
      {
        slippi_names[i] = player_info[i].display_name;
        slippi_connect_codes[i] = player_info[i].connect_code;
      }
    }
  }

  // If no file, do nothing
  if (!m_file)
  {
    return;
  }

  // Update fields relevant to generating metadata at the end
  UpdateMetadataFields(payload, length);

  // Add the payload to data to write
  data_to_write.insert(data_to_write.end(), payload, payload + length);
  m_written_byte_count += length;

  // If we are going to close the file, generate data to complete the UBJSON file
  if (file_option == "close")
  {
    // This option indicates we are done sending over body
    std::vector<u8> closing_bytes = GenerateMetadata();
    closing_bytes.push_back('}');
    data_to_write.insert(data_to_write.end(), closing_bytes.begin(), closing_bytes.end());

    // Reset display names and connect codes retrieved from netplay client
    slippi_names.clear();
    slippi_connect_codes.clear();
  }

  // Write data to file
  bool result = m_file.WriteBytes(&data_to_write[0], data_to_write.size());
  if (!result)
  {
    ERROR_LOG(EXPANSIONINTERFACE, "Failed to write data to file.");
  }

  // If file should be closed, close it
  if (file_option == "close")
  {
    // Write the number of bytes for the raw output
    std::vector<u8> sizeBytes = uint32ToVector(m_written_byte_count);
    m_file.Seek(11, 0);
    m_file.WriteBytes(&sizeBytes[0], sizeBytes.size());

    // Close file
    CloseFile();
  }
}

void CEXISlippi::CreateNewFile()
{
  if (m_file)
  {
    // If there's already a file open, close that one
    CloseFile();
  }

  std::string dir_path = SConfig::GetInstance().m_strSlippiReplayDir;
  // in case the config value just gets lost somehow
  if (dir_path.empty())
  {
    SConfig::GetInstance().m_strSlippiReplayDir = File::GetHomeDirectory() + DIR_SEP + "Slippi";
    dir_path = SConfig::GetInstance().m_strSlippiReplayDir;
  }

  // Remove a trailing / or \\ if the user managed to have that in their config
  char dirpathEnd = dir_path.back();
  if (dirpathEnd == '/' || dirpathEnd == '\\')
  {
    dir_path.pop_back();
  }

  // First, ensure that the root Slippi replay directory is created
  File::CreateFullPath(dir_path + "/");

  // Now we have a dir such as /home/Replays but we need to make one such
  // as /home/Replays/2020-06 if month categorization is enabled
  if (SConfig::GetInstance().m_slippiReplayMonthFolders)
  {
    dir_path.push_back('/');

    // Append YYYY-MM to the directory path
    uint8_t year_month_str_len = sizeof "2020-06";
    std::vector<char> year_month_buf(year_month_str_len);
    strftime(&year_month_buf[0], year_month_str_len, "%Y-%m", localtime(&m_game_start_time));

    std::string yearMonth(&year_month_buf[0]);
    dir_path.append(yearMonth);

    // Ensure that the subfolder directory is created
    File::CreateDir(dir_path);
  }

  std::string file_path = dir_path + DIR_SEP + GenerateFileName();
  INFO_LOG(SLIPPI, "EXI_DeviceSlippi.cpp: Creating new replay file %s", file_path.c_str());

#ifdef _WIN32
  m_file = File::IOFile(file_path, "wb", _SH_DENYWR);
#else
  m_file = File::IOFile(file_path, "wb");
#endif

  if (!m_file)
  {
    PanicAlertT("Could not create .slp replay file [%s].\n\n"
                "The replay folder's path might be invalid, or you might "
                "not have permission to write to it.\n\n"
                "You can change the replay folder in Config > Slippi > "
                "Slippi Replay Settings.",
                file_path.c_str());
  }
}

std::string CEXISlippi::GenerateFileName()
{
  // Add game start time
  u8 date_time_str_len = sizeof "20171015T095717";
  std::vector<char> date_time_buf(date_time_str_len);
  strftime(&date_time_buf[0], date_time_str_len, "%Y%m%dT%H%M%S", localtime(&m_game_start_time));

  std::string str(&date_time_buf[0]);
  return StringFromFormat("Game_%s.slp", str.c_str());
}

void CEXISlippi::CloseFile()
{
  if (!m_file)
  {
    // If we have no file or payload is not game end, do nothing
    return;
  }

  // If this is the end of the game end payload, reset the file so that we create a new one
  m_file.Close();
  m_file = nullptr;
}

void CEXISlippi::PrepareGameInfo(u8* payload)
{
  // Since we are prepping new data, clear any existing data
  m_read_queue.clear();

  if (!m_current_game)
  {
    // Do nothing if we don't have a game loaded
    return;
  }

  if (!m_current_game->AreSettingsLoaded())
  {
    m_read_queue.push_back(0);
    return;
  }

  // Return success code
  m_read_queue.push_back(1);

  // Prepare playback savestate payload
  m_playback_savestate_payload.clear();
  AppendWordToBuffer(&m_playback_savestate_payload,
                     0);  // This space will be used to set frame index
  int bkp_pos = 0;
  while ((*(u32*)(&payload[bkp_pos * 8])) != 0)
  {
    bkp_pos += 1;
  }
  m_playback_savestate_payload.insert(m_playback_savestate_payload.end(), payload,
                                      payload + (bkp_pos * 8 + 4));

  Slippi::GameSettings* settings = m_current_game->GetSettings();

  // Unlikely but reset the overclocking in case we quit during a hard ffw in a previous play
  SConfig::GetInstance().m_OCEnable = g_playback_status->m_orig_oc_enable;
  SConfig::GetInstance().m_OCFactor = g_playback_status->m_orig_oc_factor;

  // Start in Fast Forward if this is mirrored
  auto replay_comm_settings = g_replay_comm->getSettings();
  if (!g_playback_status->m_hard_ffw)
    g_playback_status->m_hard_ffw = replay_comm_settings.mode == "mirror";
  g_playback_status->m_last_ffw_frame = INT_MIN;

  // Build a word containing the stage and the presence of the characters
  u32 random_seed = settings->randomSeed;
  AppendWordToBuffer(&m_read_queue, random_seed);

  // This is kinda dumb but we need to handle the case where a player transforms
  // into sheik/zelda immediately. This info is not stored in the game info header
  // and so let's overwrite those values
  int player_1_pos = 24;  // This is the index of the first players character info
  std::array<u32, Slippi::GAME_INFO_HEADER_SIZE> game_info_header = settings->header;
  for (int i = 0; i < 4; i++)
  {
    // check if this player is actually in the game
    bool player_exists = m_current_game->DoesPlayerExist(i);
    if (!player_exists)
    {
      continue;
    }

    // check if the player is playing sheik or zelda
    u8 external_char_id = settings->players[i].characterId;
    if (external_char_id != 0x12 && external_char_id != 0x13)
    {
      continue;
    }

    // this is the position in the array that this player's character info is stored
    int pos = player_1_pos + (9 * i);

    // here we have determined the player is playing sheik or zelda...
    // at this point let's overwrite the player's character with the one
    // that they are playing
    game_info_header[pos] &= 0x00FFFFFF;
    game_info_header[pos] |= external_char_id << 24;
  }

  // Write entire header to game
  for (int i = 0; i < Slippi::GAME_INFO_HEADER_SIZE; i++)
  {
    AppendWordToBuffer(&m_read_queue, game_info_header[i]);
  }

  // Write UCF toggles
  std::array<u32, Slippi::UCF_TOGGLE_SIZE> ucf_toggles = settings->ucfToggles;
  for (int i = 0; i < Slippi::UCF_TOGGLE_SIZE; i++)
  {
    AppendWordToBuffer(&m_read_queue, ucf_toggles[i]);
  }

  // Write nametags
  for (int i = 0; i < 4; i++)
  {
    auto player = settings->players[i];
    for (int j = 0; j < Slippi::NAMETAG_SIZE; j++)
    {
      AppendHalfToBuffer(&m_read_queue, player.nametag[j]);
    }
  }

  // Write PAL byte
  m_read_queue.push_back(settings->isPAL);

  // Get replay version numbers
  auto replay_version = m_current_game->GetVersion();
  auto major_version = replay_version[0];
  auto minor_version = replay_version[1];

  // Write PS pre-load byte
  auto should_preload_ps = major_version > 1 || (major_version == 1 && minor_version > 2);
  m_read_queue.push_back(should_preload_ps);

  // Write PS Frozen byte
  m_read_queue.push_back(settings->isFrozenPS);

  // Write should resync setting
  m_read_queue.push_back(replay_comm_settings.shouldResync ? 1 : 0);

  // Write display names
  for (int i = 0; i < 4; i++)
  {
    auto displayName = settings->players[i].displayName;
    m_read_queue.insert(m_read_queue.end(), displayName.begin(), displayName.end());
  }

  // Return the size of the gecko code list
  PrepareGeckoList();
  AppendWordToBuffer(&m_read_queue, (u32)m_gecko_list.size());

  // Initialize frame sequence index value for reading rollbacks
  m_frame_seq_idx = 0;

  if (replay_comm_settings.rollbackDisplayMethod != "off")
  {
    // Prepare savestates
    m_available_savestates.clear();
    m_active_savestates.clear();

    // Prepare savestates for online play
    for (int i = 0; i < ROLLBACK_MAX_FRAMES; i++)
    {
      m_available_savestates.push_back(std::make_unique<SlippiSavestate>());
    }
  }
  else
  {
    // Prepare savestates
    m_available_savestates.clear();
    m_active_savestates.clear();

    // Add savestate for testing
    m_available_savestates.push_back(std::make_unique<SlippiSavestate>());
  }

  // Reset playback frame to begining
  g_playback_status->m_curr_playback_frame = Slippi::GAME_FIRST_FRAME;

  // Initialize replay related threads if not viewing rollback versions of relays
  if (replay_comm_settings.rollbackDisplayMethod == "off" &&
      (replay_comm_settings.mode == "normal" || replay_comm_settings.mode == "queue"))
  {
    g_playback_status->StartThreads();
  }
}

void CEXISlippi::PrepareGeckoList()
{
  // TODO: How do I move this somewhere else?
  // This contains all of the codes required to play legacy replays (UCF, PAL, Frz Stadium)
  static std::vector<u8> s_default_code_list = {
      0xC2, 0x0C, 0x9A, 0x44, 0x00, 0x00, 0x00, 0x2F,  // #External/UCF + Arduino Toggle UI/UCF/UCF
                                                       // 0.74 Dashback - Check for Toggle.asm
      0xD0, 0x1F, 0x00, 0x2C, 0x88, 0x9F, 0x06, 0x18, 0x38, 0x62, 0xF2, 0x28, 0x7C, 0x63, 0x20,
      0xAE, 0x2C, 0x03, 0x00, 0x01, 0x41, 0x82, 0x00, 0x14, 0x38, 0x62, 0xF2, 0x2C, 0x7C, 0x63,
      0x20, 0xAE, 0x2C, 0x03, 0x00, 0x01, 0x40, 0x82, 0x01, 0x50, 0x7C, 0x08, 0x02, 0xA6, 0x90,
      0x01, 0x00, 0x04, 0x94, 0x21, 0xFF, 0x50, 0xBE, 0x81, 0x00, 0x08, 0x48, 0x00, 0x01, 0x21,
      0x7F, 0xC8, 0x02, 0xA6, 0xC0, 0x3F, 0x08, 0x94, 0xC0, 0x5E, 0x00, 0x00, 0xFC, 0x01, 0x10,
      0x40, 0x40, 0x82, 0x01, 0x18, 0x80, 0x8D, 0xAE, 0xB4, 0xC0, 0x3F, 0x06, 0x20, 0xFC, 0x20,
      0x0A, 0x10, 0xC0, 0x44, 0x00, 0x3C, 0xFC, 0x01, 0x10, 0x40, 0x41, 0x80, 0x01, 0x00, 0x88,
      0x7F, 0x06, 0x70, 0x2C, 0x03, 0x00, 0x02, 0x40, 0x80, 0x00, 0xF4, 0x88, 0x7F, 0x22, 0x1F,
      0x54, 0x60, 0x07, 0x39, 0x40, 0x82, 0x00, 0xE8, 0x3C, 0x60, 0x80, 0x4C, 0x60, 0x63, 0x1F,
      0x78, 0x8B, 0xA3, 0x00, 0x01, 0x38, 0x7D, 0xFF, 0xFE, 0x88, 0x9F, 0x06, 0x18, 0x48, 0x00,
      0x00, 0x8D, 0x7C, 0x7C, 0x1B, 0x78, 0x7F, 0xA3, 0xEB, 0x78, 0x88, 0x9F, 0x06, 0x18, 0x48,
      0x00, 0x00, 0x7D, 0x7C, 0x7C, 0x18, 0x50, 0x7C, 0x63, 0x19, 0xD6, 0x2C, 0x03, 0x15, 0xF9,
      0x40, 0x81, 0x00, 0xB0, 0x38, 0x00, 0x00, 0x01, 0x90, 0x1F, 0x23, 0x58, 0x90, 0x1F, 0x23,
      0x40, 0x80, 0x9F, 0x00, 0x04, 0x2C, 0x04, 0x00, 0x0A, 0x40, 0xA2, 0x00, 0x98, 0x88, 0x7F,
      0x00, 0x0C, 0x38, 0x80, 0x00, 0x01, 0x3D, 0x80, 0x80, 0x03, 0x61, 0x8C, 0x41, 0x8C, 0x7D,
      0x89, 0x03, 0xA6, 0x4E, 0x80, 0x04, 0x21, 0x2C, 0x03, 0x00, 0x00, 0x41, 0x82, 0x00, 0x78,
      0x80, 0x83, 0x00, 0x2C, 0x80, 0x84, 0x1E, 0xCC, 0xC0, 0x3F, 0x00, 0x2C, 0xD0, 0x24, 0x00,
      0x18, 0xC0, 0x5E, 0x00, 0x04, 0xFC, 0x01, 0x10, 0x40, 0x41, 0x81, 0x00, 0x0C, 0x38, 0x60,
      0x00, 0x80, 0x48, 0x00, 0x00, 0x08, 0x38, 0x60, 0x00, 0x7F, 0x98, 0x64, 0x00, 0x06, 0x48,
      0x00, 0x00, 0x48, 0x7C, 0x85, 0x23, 0x78, 0x38, 0x63, 0xFF, 0xFF, 0x2C, 0x03, 0x00, 0x00,
      0x40, 0x80, 0x00, 0x08, 0x38, 0x63, 0x00, 0x05, 0x3C, 0x80, 0x80, 0x46, 0x60, 0x84, 0xB1,
      0x08, 0x1C, 0x63, 0x00, 0x30, 0x7C, 0x84, 0x1A, 0x14, 0x1C, 0x65, 0x00, 0x0C, 0x7C, 0x84,
      0x1A, 0x14, 0x88, 0x64, 0x00, 0x02, 0x7C, 0x63, 0x07, 0x74, 0x4E, 0x80, 0x00, 0x20, 0x4E,
      0x80, 0x00, 0x21, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xBA, 0x81, 0x00, 0x08,
      0x80, 0x01, 0x00, 0xB4, 0x38, 0x21, 0x00, 0xB0, 0x7C, 0x08, 0x03, 0xA6, 0x00, 0x00, 0x00,
      0x00, 0xC2, 0x09, 0x98, 0xA4, 0x00, 0x00, 0x00,
      0x2B,  // #External/UCF + Arduino Toggle UI/UCF/UCF
             // 0.74 Shield Drop - Check for Toggle.asm
      0x7C, 0x08, 0x02, 0xA6, 0x90, 0x01, 0x00, 0x04, 0x94, 0x21, 0xFF, 0x50, 0xBE, 0x81, 0x00,
      0x08, 0x7C, 0x7E, 0x1B, 0x78, 0x83, 0xFE, 0x00, 0x2C, 0x48, 0x00, 0x01, 0x01, 0x7F, 0xA8,
      0x02, 0xA6, 0x88, 0x9F, 0x06, 0x18, 0x38, 0x62, 0xF2, 0x28, 0x7C, 0x63, 0x20, 0xAE, 0x2C,
      0x03, 0x00, 0x01, 0x41, 0x82, 0x00, 0x14, 0x38, 0x62, 0xF2, 0x30, 0x7C, 0x63, 0x20, 0xAE,
      0x2C, 0x03, 0x00, 0x01, 0x40, 0x82, 0x00, 0xF8, 0xC0, 0x3F, 0x06, 0x3C, 0x80, 0x6D, 0xAE,
      0xB4, 0xC0, 0x03, 0x03, 0x14, 0xFC, 0x01, 0x00, 0x40, 0x40, 0x81, 0x00, 0xE4, 0xC0, 0x3F,
      0x06, 0x20, 0x48, 0x00, 0x00, 0x71, 0xD0, 0x21, 0x00, 0x90, 0xC0, 0x3F, 0x06, 0x24, 0x48,
      0x00, 0x00, 0x65, 0xC0, 0x41, 0x00, 0x90, 0xEC, 0x42, 0x00, 0xB2, 0xEC, 0x21, 0x00, 0x72,
      0xEC, 0x21, 0x10, 0x2A, 0xC0, 0x5D, 0x00, 0x0C, 0xFC, 0x01, 0x10, 0x40, 0x41, 0x80, 0x00,
      0xB4, 0x88, 0x9F, 0x06, 0x70, 0x2C, 0x04, 0x00, 0x03, 0x40, 0x81, 0x00, 0xA8, 0xC0, 0x1D,
      0x00, 0x10, 0xC0, 0x3F, 0x06, 0x24, 0xFC, 0x00, 0x08, 0x40, 0x40, 0x80, 0x00, 0x98, 0xBA,
      0x81, 0x00, 0x08, 0x80, 0x01, 0x00, 0xB4, 0x38, 0x21, 0x00, 0xB0, 0x7C, 0x08, 0x03, 0xA6,
      0x80, 0x61, 0x00, 0x1C, 0x83, 0xE1, 0x00, 0x14, 0x38, 0x21, 0x00, 0x18, 0x38, 0x63, 0x00,
      0x08, 0x7C, 0x68, 0x03, 0xA6, 0x4E, 0x80, 0x00, 0x20, 0xFC, 0x00, 0x0A, 0x10, 0xC0, 0x3D,
      0x00, 0x00, 0xEC, 0x00, 0x00, 0x72, 0xC0, 0x3D, 0x00, 0x04, 0xEC, 0x00, 0x08, 0x28, 0xFC,
      0x00, 0x00, 0x1E, 0xD8, 0x01, 0x00, 0x80, 0x80, 0x61, 0x00, 0x84, 0x38, 0x63, 0x00, 0x02,
      0x3C, 0x00, 0x43, 0x30, 0xC8, 0x5D, 0x00, 0x14, 0x6C, 0x63, 0x80, 0x00, 0x90, 0x01, 0x00,
      0x80, 0x90, 0x61, 0x00, 0x84, 0xC8, 0x21, 0x00, 0x80, 0xEC, 0x01, 0x10, 0x28, 0xC0, 0x3D,
      0x00, 0x00, 0xEC, 0x20, 0x08, 0x24, 0x4E, 0x80, 0x00, 0x20, 0x4E, 0x80, 0x00, 0x21, 0x42,
      0xA0, 0x00, 0x00, 0x37, 0x27, 0x00, 0x00, 0x43, 0x30, 0x00, 0x00, 0x3F, 0x80, 0x00, 0x00,
      0xBF, 0x4C, 0xCC, 0xCD, 0x43, 0x30, 0x00, 0x00, 0x80, 0x00, 0x00, 0x00, 0x7F, 0xC3, 0xF3,
      0x78, 0x7F, 0xE4, 0xFB, 0x78, 0xBA, 0x81, 0x00, 0x08, 0x80, 0x01, 0x00, 0xB4, 0x38, 0x21,
      0x00, 0xB0, 0x7C, 0x08, 0x03, 0xA6, 0x60, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xC2,
      0x16, 0xE7, 0x50, 0x00, 0x00, 0x00,
      0x33,  // #Common/StaticPatches/ToggledStaticOverwrites.asm
      0x88, 0x62, 0xF2, 0x34, 0x2C, 0x03, 0x00, 0x00, 0x41, 0x82, 0x00, 0x14, 0x48, 0x00, 0x00,
      0x75, 0x7C, 0x68, 0x02, 0xA6, 0x48, 0x00, 0x01, 0x3D, 0x48, 0x00, 0x00, 0x14, 0x48, 0x00,
      0x00, 0x95, 0x7C, 0x68, 0x02, 0xA6, 0x48, 0x00, 0x01, 0x2D, 0x48, 0x00, 0x00, 0x04, 0x88,
      0x62, 0xF2, 0x38, 0x2C, 0x03, 0x00, 0x00, 0x41, 0x82, 0x00, 0x14, 0x48, 0x00, 0x00, 0xB9,
      0x7C, 0x68, 0x02, 0xA6, 0x48, 0x00, 0x01, 0x11, 0x48, 0x00, 0x00, 0x10, 0x48, 0x00, 0x00,
      0xC9, 0x7C, 0x68, 0x02, 0xA6, 0x48, 0x00, 0x01, 0x01, 0x88, 0x62, 0xF2, 0x3C, 0x2C, 0x03,
      0x00, 0x00, 0x41, 0x82, 0x00, 0x14, 0x48, 0x00, 0x00, 0xD1, 0x7C, 0x68, 0x02, 0xA6, 0x48,
      0x00, 0x00, 0xE9, 0x48, 0x00, 0x01, 0x04, 0x48, 0x00, 0x00, 0xD1, 0x7C, 0x68, 0x02, 0xA6,
      0x48, 0x00, 0x00, 0xD9, 0x48, 0x00, 0x00, 0xF4, 0x4E, 0x80, 0x00, 0x21, 0x80, 0x3C, 0xE4,
      0xD4, 0x00, 0x24, 0x04, 0x64, 0x80, 0x07, 0x96, 0xE0, 0x60, 0x00, 0x00, 0x00, 0x80, 0x2B,
      0x7E, 0x54, 0x48, 0x00, 0x00, 0x88, 0x80, 0x2B, 0x80, 0x8C, 0x48, 0x00, 0x00, 0x84, 0x80,
      0x12, 0x39, 0xA8, 0x60, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0x4E, 0x80, 0x00, 0x21,
      0x80, 0x3C, 0xE4, 0xD4, 0x00, 0x20, 0x00, 0x00, 0x80, 0x07, 0x96, 0xE0, 0x3A, 0x40, 0x00,
      0x01, 0x80, 0x2B, 0x7E, 0x54, 0x88, 0x7F, 0x22, 0x40, 0x80, 0x2B, 0x80, 0x8C, 0x2C, 0x03,
      0x00, 0x02, 0x80, 0x10, 0xFC, 0x48, 0x90, 0x05, 0x21, 0xDC, 0x80, 0x10, 0xFB, 0x68, 0x90,
      0x05, 0x21, 0xDC, 0x80, 0x12, 0x39, 0xA8, 0x90, 0x1F, 0x1A, 0x5C, 0xFF, 0xFF, 0xFF, 0xFF,
      0x4E, 0x80, 0x00, 0x21, 0x80, 0x1D, 0x46, 0x10, 0x48, 0x00, 0x00, 0x4C, 0x80, 0x1D, 0x47,
      0x24, 0x48, 0x00, 0x00, 0x3C, 0x80, 0x1D, 0x46, 0x0C, 0x80, 0x9F, 0x00, 0xEC, 0xFF, 0xFF,
      0xFF, 0xFF, 0x4E, 0x80, 0x00, 0x21, 0x80, 0x1D, 0x46, 0x10, 0x38, 0x83, 0x7F, 0x9C, 0x80,
      0x1D, 0x47, 0x24, 0x88, 0x1B, 0x00, 0xC4, 0x80, 0x1D, 0x46, 0x0C, 0x3C, 0x60, 0x80, 0x3B,
      0xFF, 0xFF, 0xFF, 0xFF, 0x4E, 0x80, 0x00, 0x21, 0x80, 0x1D, 0x45, 0xFC, 0x48, 0x00, 0x09,
      0xDC, 0xFF, 0xFF, 0xFF, 0xFF, 0x4E, 0x80, 0x00, 0x21, 0x80, 0x1D, 0x45, 0xFC, 0x40, 0x80,
      0x09, 0xDC, 0xFF, 0xFF, 0xFF, 0xFF, 0x38, 0xA3, 0xFF, 0xFC, 0x84, 0x65, 0x00, 0x04, 0x2C,
      0x03, 0xFF, 0xFF, 0x41, 0x82, 0x00, 0x10, 0x84, 0x85, 0x00, 0x04, 0x90, 0x83, 0x00, 0x00,
      0x4B, 0xFF, 0xFF, 0xEC, 0x4E, 0x80, 0x00, 0x20, 0x3C, 0x60, 0x80, 0x00, 0x3C, 0x80, 0x00,
      0x3B, 0x60, 0x84, 0x72, 0x2C, 0x3D, 0x80, 0x80, 0x32, 0x61, 0x8C, 0x8F, 0x50, 0x7D, 0x89,
      0x03, 0xA6, 0x4E, 0x80, 0x04, 0x21, 0x3C, 0x60, 0x80, 0x17, 0x3C, 0x80, 0x80, 0x17, 0x00,
      0x00, 0x00, 0x00, 0xC2, 0x1D, 0x14, 0xC8, 0x00, 0x00, 0x00,
      0x04,  // #Common/Preload Stadium
             // Transformations/Handlers/Init
             // isLoaded Bool.asm
      0x88, 0x62, 0xF2, 0x38, 0x2C, 0x03, 0x00, 0x00, 0x41, 0x82, 0x00, 0x0C, 0x38, 0x60, 0x00,
      0x00, 0x98, 0x7F, 0x00, 0xF0, 0x3B, 0xA0, 0x00, 0x01, 0x60, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0xC2, 0x1D, 0x45, 0xEC, 0x00, 0x00, 0x00, 0x1B,  // #Common/Preload Stadium
                                                                   // Transformations/Handlers/Load
                                                                   // Transformation.asm
      0x88, 0x62, 0xF2, 0x38, 0x2C, 0x03, 0x00, 0x00, 0x41, 0x82, 0x00, 0xC4, 0x88, 0x7F, 0x00,
      0xF0, 0x2C, 0x03, 0x00, 0x00, 0x40, 0x82, 0x00, 0xB8, 0x38, 0x60, 0x00, 0x04, 0x3D, 0x80,
      0x80, 0x38, 0x61, 0x8C, 0x05, 0x80, 0x7D, 0x89, 0x03, 0xA6, 0x4E, 0x80, 0x04, 0x21, 0x54,
      0x60, 0x10, 0x3A, 0xA8, 0x7F, 0x00, 0xE2, 0x3C, 0x80, 0x80, 0x3B, 0x60, 0x84, 0x7F, 0x9C,
      0x7C, 0x84, 0x00, 0x2E, 0x7C, 0x03, 0x20, 0x00, 0x41, 0x82, 0xFF, 0xD4, 0x90, 0x9F, 0x00,
      0xEC, 0x2C, 0x04, 0x00, 0x03, 0x40, 0x82, 0x00, 0x0C, 0x38, 0x80, 0x00, 0x00, 0x48, 0x00,
      0x00, 0x34, 0x2C, 0x04, 0x00, 0x04, 0x40, 0x82, 0x00, 0x0C, 0x38, 0x80, 0x00, 0x01, 0x48,
      0x00, 0x00, 0x24, 0x2C, 0x04, 0x00, 0x09, 0x40, 0x82, 0x00, 0x0C, 0x38, 0x80, 0x00, 0x02,
      0x48, 0x00, 0x00, 0x14, 0x2C, 0x04, 0x00, 0x06, 0x40, 0x82, 0x00, 0x00, 0x38, 0x80, 0x00,
      0x03, 0x48, 0x00, 0x00, 0x04, 0x3C, 0x60, 0x80, 0x3E, 0x60, 0x63, 0x12, 0x48, 0x54, 0x80,
      0x10, 0x3A, 0x7C, 0x63, 0x02, 0x14, 0x80, 0x63, 0x03, 0xD8, 0x80, 0x9F, 0x00, 0xCC, 0x38,
      0xBF, 0x00, 0xC8, 0x3C, 0xC0, 0x80, 0x1D, 0x60, 0xC6, 0x42, 0x20, 0x38, 0xE0, 0x00, 0x00,
      0x3D, 0x80, 0x80, 0x01, 0x61, 0x8C, 0x65, 0x80, 0x7D, 0x89, 0x03, 0xA6, 0x4E, 0x80, 0x04,
      0x21, 0x38, 0x60, 0x00, 0x01, 0x98, 0x7F, 0x00, 0xF0, 0x80, 0x7F, 0x00, 0xD8, 0x60, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xC2, 0x1D, 0x4F, 0x14, 0x00, 0x00, 0x00,
      0x04,  // #Common/Preload
             // Stadium
             // Transformations/Handlers/Reset
             // isLoaded.asm
      0x88, 0x62, 0xF2, 0x38, 0x2C, 0x03, 0x00, 0x00, 0x41, 0x82, 0x00, 0x0C, 0x38, 0x60, 0x00,
      0x00, 0x98, 0x7F, 0x00, 0xF0, 0x80, 0x6D, 0xB2, 0xD8, 0x60, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0xC2, 0x06, 0x8F, 0x30, 0x00, 0x00, 0x00, 0x9D,  // #Common/PAL/Handlers/Character
                                                                   // DAT Patcher.asm
      0x88, 0x62, 0xF2, 0x34, 0x2C, 0x03, 0x00, 0x00, 0x41, 0x82, 0x04, 0xD4, 0x7C, 0x08, 0x02,
      0xA6, 0x90, 0x01, 0x00, 0x04, 0x94, 0x21, 0xFF, 0x50, 0xBE, 0x81, 0x00, 0x08, 0x83, 0xFE,
      0x01, 0x0C, 0x83, 0xFF, 0x00, 0x08, 0x3B, 0xFF, 0xFF, 0xE0, 0x80, 0x7D, 0x00, 0x00, 0x2C,
      0x03, 0x00, 0x1B, 0x40, 0x80, 0x04, 0x9C, 0x48, 0x00, 0x00, 0x71, 0x48, 0x00, 0x00, 0xA9,
      0x48, 0x00, 0x00, 0xB9, 0x48, 0x00, 0x01, 0x51, 0x48, 0x00, 0x01, 0x79, 0x48, 0x00, 0x01,
      0x79, 0x48, 0x00, 0x02, 0x29, 0x48, 0x00, 0x02, 0x39, 0x48, 0x00, 0x02, 0x81, 0x48, 0x00,
      0x02, 0xF9, 0x48, 0x00, 0x03, 0x11, 0x48, 0x00, 0x03, 0x11, 0x48, 0x00, 0x03, 0x11, 0x48,
      0x00, 0x03, 0x11, 0x48, 0x00, 0x03, 0x21, 0x48, 0x00, 0x03, 0x21, 0x48, 0x00, 0x03, 0x89,
      0x48, 0x00, 0x03, 0x89, 0x48, 0x00, 0x03, 0x91, 0x48, 0x00, 0x03, 0x91, 0x48, 0x00, 0x03,
      0xA9, 0x48, 0x00, 0x03, 0xA9, 0x48, 0x00, 0x03, 0xB9, 0x48, 0x00, 0x03, 0xB9, 0x48, 0x00,
      0x03, 0xC9, 0x48, 0x00, 0x03, 0xC9, 0x48, 0x00, 0x03, 0xC9, 0x48, 0x00, 0x04, 0x29, 0x7C,
      0x88, 0x02, 0xA6, 0x1C, 0x63, 0x00, 0x04, 0x7C, 0x84, 0x1A, 0x14, 0x80, 0xA4, 0x00, 0x00,
      0x54, 0xA5, 0x01, 0xBA, 0x7C, 0xA4, 0x2A, 0x14, 0x80, 0x65, 0x00, 0x00, 0x80, 0x85, 0x00,
      0x04, 0x2C, 0x03, 0x00, 0xFF, 0x41, 0x82, 0x00, 0x14, 0x7C, 0x63, 0xFA, 0x14, 0x90, 0x83,
      0x00, 0x00, 0x38, 0xA5, 0x00, 0x08, 0x4B, 0xFF, 0xFF, 0xE4, 0x48, 0x00, 0x03, 0xF0, 0x00,
      0x00, 0x33, 0x44, 0x3F, 0x54, 0x7A, 0xE1, 0x00, 0x00, 0x33, 0x60, 0x42, 0xC4, 0x00, 0x00,
      0x00, 0x00, 0x00, 0xFF, 0x00, 0x00, 0x37, 0x9C, 0x42, 0x92, 0x00, 0x00, 0x00, 0x00, 0x39,
      0x08, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00, 0x39, 0x0C, 0x40, 0x86, 0x66, 0x66, 0x00, 0x00,
      0x39, 0x10, 0x3D, 0xEA, 0x0E, 0xA1, 0x00, 0x00, 0x39, 0x28, 0x41, 0xA0, 0x00, 0x00, 0x00,
      0x00, 0x3C, 0x04, 0x2C, 0x01, 0x48, 0x0C, 0x00, 0x00, 0x47, 0x20, 0x1B, 0x96, 0x80, 0x13,
      0x00, 0x00, 0x47, 0x34, 0x1B, 0x96, 0x80, 0x13, 0x00, 0x00, 0x47, 0x3C, 0x04, 0x00, 0x00,
      0x09, 0x00, 0x00, 0x4A, 0x40, 0x2C, 0x00, 0x68, 0x11, 0x00, 0x00, 0x4A, 0x4C, 0x28, 0x1B,
      0x00, 0x13, 0x00, 0x00, 0x4A, 0x50, 0x0D, 0x00, 0x01, 0x0B, 0x00, 0x00, 0x4A, 0x54, 0x2C,
      0x80, 0x68, 0x11, 0x00, 0x00, 0x4A, 0x60, 0x28, 0x1B, 0x00, 0x13, 0x00, 0x00, 0x4A, 0x64,
      0x0D, 0x00, 0x01, 0x0B, 0x00, 0x00, 0x4B, 0x24, 0x2C, 0x00, 0x68, 0x0D, 0x00, 0x00, 0x4B,
      0x30, 0x0F, 0x10, 0x40, 0x13, 0x00, 0x00, 0x4B, 0x38, 0x2C, 0x80, 0x38, 0x0D, 0x00, 0x00,
      0x4B, 0x44, 0x0F, 0x10, 0x40, 0x13, 0x00, 0x00, 0x00, 0xFF, 0x00, 0x00, 0x38, 0x0C, 0x00,
      0x00, 0x00, 0x07, 0x00, 0x00, 0x4E, 0xF8, 0x2C, 0x00, 0x38, 0x03, 0x00, 0x00, 0x4F, 0x08,
      0x0F, 0x80, 0x00, 0x0B, 0x00, 0x00, 0x4F, 0x0C, 0x2C, 0x80, 0x20, 0x03, 0x00, 0x00, 0x4F,
      0x1C, 0x0F, 0x80, 0x00, 0x0B, 0x00, 0x00, 0x00, 0xFF, 0x00, 0x00, 0x00, 0xFF, 0x00, 0x00,
      0x4D, 0x10, 0x3F, 0xC0, 0x00, 0x00, 0x00, 0x00, 0x4D, 0x70, 0x42, 0x94, 0x00, 0x00, 0x00,
      0x00, 0x4D, 0xD4, 0x41, 0x90, 0x00, 0x00, 0x00, 0x00, 0x4D, 0xE0, 0x41, 0x90, 0x00, 0x00,
      0x00, 0x00, 0x83, 0xAC, 0x2C, 0x00, 0x00, 0x09, 0x00, 0x00, 0x83, 0xB8, 0x34, 0x8C, 0x80,
      0x11, 0x00, 0x00, 0x84, 0x00, 0x34, 0x8C, 0x80, 0x11, 0x00, 0x00, 0x84, 0x30, 0x05, 0x00,
      0x00, 0x8B, 0x00, 0x00, 0x84, 0x38, 0x04, 0x1A, 0x05, 0x00, 0x00, 0x00, 0x84, 0x44, 0x05,
      0x00, 0x00, 0x8B, 0x00, 0x00, 0x84, 0xDC, 0x05, 0x78, 0x05, 0x78, 0x00, 0x00, 0x85, 0xB8,
      0x10, 0x00, 0x01, 0x0B, 0x00, 0x00, 0x85, 0xC0, 0x03, 0xE8, 0x01, 0xF4, 0x00, 0x00, 0x85,
      0xCC, 0x10, 0x00, 0x01, 0x0B, 0x00, 0x00, 0x85, 0xD4, 0x03, 0x84, 0x03, 0xE8, 0x00, 0x00,
      0x85, 0xE0, 0x10, 0x00, 0x01, 0x0B, 0x00, 0x00, 0x88, 0x18, 0x0B, 0x00, 0x01, 0x0B, 0x00,
      0x00, 0x88, 0x2C, 0x0B, 0x00, 0x01, 0x0B, 0x00, 0x00, 0x88, 0xF8, 0x04, 0x1A, 0x0B, 0xB8,
      0x00, 0x00, 0x89, 0x3C, 0x04, 0x1A, 0x0B, 0xB8, 0x00, 0x00, 0x89, 0x80, 0x04, 0x1A, 0x0B,
      0xB8, 0x00, 0x00, 0x89, 0xE0, 0x04, 0xFE, 0xF7, 0x04, 0x00, 0x00, 0x00, 0xFF, 0x00, 0x00,
      0x36, 0xCC, 0x42, 0xEC, 0x00, 0x00, 0x00, 0x00, 0x37, 0xC4, 0x0C, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0xFF, 0x00, 0x00, 0x34, 0x68, 0x3F, 0x66, 0x66, 0x66, 0x00, 0x00, 0x39, 0xD8,
      0x44, 0x0C, 0x00, 0x00, 0x00, 0x00, 0x3A, 0x44, 0xB4, 0x99, 0x00, 0x11, 0x00, 0x00, 0x3A,
      0x48, 0x1B, 0x8C, 0x00, 0x8F, 0x00, 0x00, 0x3A, 0x58, 0xB4, 0x99, 0x00, 0x11, 0x00, 0x00,
      0x3A, 0x5C, 0x1B, 0x8C, 0x00, 0x8F, 0x00, 0x00, 0x3A, 0x6C, 0xB4, 0x99, 0x00, 0x11, 0x00,
      0x00, 0x3A, 0x70, 0x1B, 0x8C, 0x00, 0x8F, 0x00, 0x00, 0x3B, 0x30, 0x44, 0x0C, 0x00, 0x00,
      0x00, 0x00, 0x00, 0xFF, 0x00, 0x00, 0x45, 0xC8, 0x2C, 0x01, 0x50, 0x10, 0x00, 0x00, 0x45,
      0xD4, 0x2D, 0x19, 0x80, 0x13, 0x00, 0x00, 0x45, 0xDC, 0x2C, 0x80, 0xB0, 0x10, 0x00, 0x00,
      0x45, 0xE8, 0x2D, 0x19, 0x80, 0x13, 0x00, 0x00, 0x49, 0xC4, 0x2C, 0x00, 0x68, 0x0A, 0x00,
      0x00, 0x49, 0xD0, 0x28, 0x1B, 0x80, 0x13, 0x00, 0x00, 0x49, 0xD8, 0x2C, 0x80, 0x78, 0x0A,
      0x00, 0x00, 0x49, 0xE4, 0x28, 0x1B, 0x80, 0x13, 0x00, 0x00, 0x49, 0xF0, 0x2C, 0x00, 0x68,
      0x08, 0x00, 0x00, 0x49, 0xFC, 0x23, 0x1B, 0x80, 0x13, 0x00, 0x00, 0x4A, 0x04, 0x2C, 0x80,
      0x78, 0x08, 0x00, 0x00, 0x4A, 0x10, 0x23, 0x1B, 0x80, 0x13, 0x00, 0x00, 0x5C, 0x98, 0x1E,
      0x0C, 0x80, 0x80, 0x00, 0x00, 0x5C, 0xF4, 0xB4, 0x80, 0x0C, 0x90, 0x00, 0x00, 0x5D, 0x08,
      0xB4, 0x80, 0x0C, 0x90, 0x00, 0x00, 0x00, 0xFF, 0x00, 0x00, 0x3A, 0x1C, 0xB4, 0x94, 0x00,
      0x13, 0x00, 0x00, 0x3A, 0x64, 0x2C, 0x00, 0x00, 0x15, 0x00, 0x00, 0x3A, 0x70, 0xB4, 0x92,
      0x80, 0x13, 0x00, 0x00, 0x00, 0xFF, 0x00, 0x00, 0x00, 0xFF, 0x00, 0x00, 0x00, 0xFF, 0x00,
      0x00, 0x00, 0xFF, 0x00, 0x00, 0x64, 0x7C, 0xB4, 0x9A, 0x40, 0x17, 0x00, 0x00, 0x64, 0x80,
      0x64, 0x00, 0x10, 0x97, 0x00, 0x00, 0x00, 0xFF, 0x00, 0x00, 0x00, 0xFF, 0x00, 0x00, 0x33,
      0xE4, 0x42, 0xDE, 0x00, 0x00, 0x00, 0x00, 0x45, 0x28, 0x2C, 0x01, 0x30, 0x11, 0x00, 0x00,
      0x45, 0x34, 0xB4, 0x98, 0x80, 0x13, 0x00, 0x00, 0x45, 0x3C, 0x2C, 0x81, 0x30, 0x11, 0x00,
      0x00, 0x45, 0x48, 0xB4, 0x98, 0x80, 0x13, 0x00, 0x00, 0x45, 0x50, 0x2D, 0x00, 0x20, 0x11,
      0x00, 0x00, 0x45, 0x5C, 0xB4, 0x98, 0x80, 0x13, 0x00, 0x00, 0x45, 0xF8, 0x2C, 0x01, 0x30,
      0x0F, 0x00, 0x00, 0x46, 0x08, 0x0F, 0x00, 0x01, 0x0B, 0x00, 0x00, 0x46, 0x0C, 0x2C, 0x81,
      0x28, 0x0F, 0x00, 0x00, 0x46, 0x1C, 0x0F, 0x00, 0x01, 0x0B, 0x00, 0x00, 0x4A, 0xEC, 0x2C,
      0x00, 0x70, 0x03, 0x00, 0x00, 0x4B, 0x00, 0x2C, 0x80, 0x38, 0x03, 0x00, 0x00, 0x00, 0xFF,
      0x00, 0x00, 0x00, 0xFF, 0x00, 0x00, 0x48, 0x5C, 0x2C, 0x00, 0x00, 0x0F, 0x00, 0x00, 0x00,
      0xFF, 0x00, 0x00, 0x00, 0xFF, 0x00, 0x00, 0x37, 0xB0, 0x3F, 0x59, 0x99, 0x9A, 0x00, 0x00,
      0x37, 0xCC, 0x42, 0xAA, 0x00, 0x00, 0x00, 0x00, 0x55, 0x20, 0x87, 0x11, 0x80, 0x13, 0x00,
      0x00, 0x00, 0xFF, 0x00, 0x00, 0x00, 0xFF, 0x00, 0x00, 0x3B, 0x8C, 0x44, 0x0C, 0x00, 0x00,
      0x00, 0x00, 0x3D, 0x0C, 0x44, 0x0C, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFF, 0x00, 0x00, 0x00,
      0xFF, 0x00, 0x00, 0x50, 0xE4, 0xB4, 0x99, 0x00, 0x13, 0x00, 0x00, 0x50, 0xF8, 0xB4, 0x99,
      0x00, 0x13, 0x00, 0x00, 0x00, 0xFF, 0x00, 0x00, 0x00, 0xFF, 0x00, 0x00, 0x00, 0xFF, 0x00,
      0x00, 0x4E, 0xB0, 0x02, 0xBC, 0xFF, 0x38, 0x00, 0x00, 0x4E, 0xBC, 0x14, 0x00, 0x01, 0x23,
      0x00, 0x00, 0x4E, 0xC4, 0x03, 0x84, 0x01, 0xF4, 0x00, 0x00, 0x4E, 0xD0, 0x14, 0x00, 0x01,
      0x23, 0x00, 0x00, 0x4E, 0xD8, 0x04, 0x4C, 0x04, 0xB0, 0x00, 0x00, 0x4E, 0xE4, 0x14, 0x00,
      0x01, 0x23, 0x00, 0x00, 0x50, 0x5C, 0x2C, 0x00, 0x68, 0x15, 0x00, 0x00, 0x50, 0x6C, 0x14,
      0x08, 0x01, 0x23, 0x00, 0x00, 0x50, 0x70, 0x2C, 0x80, 0x60, 0x15, 0x00, 0x00, 0x50, 0x80,
      0x14, 0x08, 0x01, 0x23, 0x00, 0x00, 0x50, 0x84, 0x2D, 0x00, 0x20, 0x15, 0x00, 0x00, 0x50,
      0x94, 0x14, 0x08, 0x01, 0x23, 0x00, 0x00, 0x00, 0xFF, 0x00, 0x00, 0x00, 0xFF, 0xBA, 0x81,
      0x00, 0x08, 0x80, 0x01, 0x00, 0xB4, 0x38, 0x21, 0x00, 0xB0, 0x7C, 0x08, 0x03, 0xA6, 0x3C,
      0x60, 0x80, 0x3C, 0x60, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xC2, 0x2F, 0x9A, 0x3C,
      0x00, 0x00, 0x00, 0x08,  // #Common/PAL/Handlers/PAL Stock Icons.asm
      0x88, 0x62, 0xF2, 0x34, 0x2C, 0x03, 0x00, 0x00, 0x41, 0x82, 0x00, 0x30, 0x48, 0x00, 0x00,
      0x21, 0x7C, 0x88, 0x02, 0xA6, 0x80, 0x64, 0x00, 0x00, 0x90, 0x7D, 0x00, 0x2C, 0x90, 0x7D,
      0x00, 0x30, 0x80, 0x64, 0x00, 0x04, 0x90, 0x7D, 0x00, 0x3C, 0x48, 0x00, 0x00, 0x10, 0x4E,
      0x80, 0x00, 0x21, 0x3F, 0x59, 0x99, 0x9A, 0xC1, 0xA8, 0x00, 0x00, 0x80, 0x1D, 0x00, 0x14,
      0x00, 0x00, 0x00, 0x00, 0xC2, 0x10, 0xFC, 0x44, 0x00, 0x00, 0x00,
      0x04,  // #Common/PAL/Handlers/DK
             // Up B/Aerial Up B.asm
      0x88, 0x82, 0xF2, 0x34, 0x2C, 0x04, 0x00, 0x00, 0x41, 0x82, 0x00, 0x10, 0x3C, 0x00, 0x80,
      0x11, 0x60, 0x00, 0x00, 0x74, 0x48, 0x00, 0x00, 0x08, 0x38, 0x03, 0xD7, 0x74, 0x00, 0x00,
      0x00, 0x00, 0xC2, 0x10, 0xFB, 0x64, 0x00, 0x00, 0x00, 0x04,  // #Common/PAL/Handlers/DK Up
                                                                   // B/Grounded Up B.asm
      0x88, 0x82, 0xF2, 0x34, 0x2C, 0x04, 0x00, 0x00, 0x41, 0x82, 0x00, 0x10, 0x3C, 0x00, 0x80,
      0x11, 0x60, 0x00, 0x00, 0x74, 0x48, 0x00, 0x00, 0x08, 0x38, 0x03, 0xD7, 0x74, 0x00, 0x00,
      0x00, 0x00, 0xFF, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00  // Termination sequence
  };

  static std::unordered_map<u32, bool> s_deny_list = {
      {0x8008d698, true},  // Recording/GetLCancelStatus/GetLCancelStatus.asm
      {0x8006c324, true},  // Recording/GetLCancelStatus/ResetLCancelStatus.asm
      {0x800679bc, true},  // Recording/ExtendPlayerBlock.asm
      {0x802fef88, true},  // Recording/FlushFrameBuffer.asm
      {0x80005604, true},  // Recording/IsVSMode.asm
      {0x8016d30c, true},  // Recording/SendGameEnd.asm
      {0x8016e74c, true},  // Recording/SendGameInfo.asm
      {0x8006c5d8, true},  // Recording/SendGamePostFrame.asm
      {0x8006b0dc, true},  // Recording/SendGamePreFrame.asm
      {0x803219ec, true},  // 3.4.0: Recording/FlushFrameBuffer.asm (Have to keep old ones for
                           // backward compatibility)
      {0x8006da34, true},  // 3.4.0: Recording/SendGamePostFrame.asm
      {0x8016d884, true},  // 3.7.0: Recording/SendGameEnd.asm

      {0x8021aae4, true},  // Binary/FasterMeleeSettings/DisableFdTransitions.bin
      {0x801cbb90, true},  // Binary/FasterMeleeSettings/LaglessFod.bin
      {0x801CC8AC, true},  // Binary/FasterMeleeSettings/LaglessFod.bin
      {0x801CBE9C, true},  // Binary/FasterMeleeSettings/LaglessFod.bin
      {0x801CBEF0, true},  // Binary/FasterMeleeSettings/LaglessFod.bin
      {0x801CBF54, true},  // Binary/FasterMeleeSettings/LaglessFod.bin
      {0x80390838, true},  // Binary/FasterMeleeSettings/LaglessFod.bin
      {0x801CD250, true},  // Binary/FasterMeleeSettings/LaglessFod.bin
      {0x801CCDCC, true},  // Binary/FasterMeleeSettings/LaglessFod.bin
      {0x801C26B0, true},  // Binary/FasterMeleeSettings/RandomStageMusic.bin
      {0x803761ec, true},  // Binary/NormalLagReduction.bin
      {0x800198a4, true},  // Binary/PerformanceLagReduction.bin
      {0x80019620, true},  // Binary/PerformanceLagReduction.bin
      {0x801A5054, true},  // Binary/PerformanceLagReduction.bin
      {0x80397878, true},  // Binary/OsReportPrintOnCrash.bin
      {0x801A4DA0, true},  // Binary/LagReduction/PD.bin
      {0x801A4DB4, true},  // Binary/LagReduction/PD.bin
      {0x80019860, true},  // Binary/LagReduction/PD.bin
      {0x801A4C24, true},  // Binary/LagReduction/PD+VB.bin
      {0x8001985C, true},  // Binary/LagReduction/PD+VB.bin
      {0x80019860, true},  // Binary/LagReduction/PD+VB.bin
      {0x80376200, true},  // Binary/LagReduction/PD+VB.bin
      {0x801A5018, true},  // Binary/LagReduction/PD+VB.bin
      {0x80218D68, true},  // Binary/LagReduction/PD+VB.bin
      {0x8016E9AC, true},  // Binary/Force2PCenterHud.bin
      {0x80030E44, true},  // Binary/DisableScreenShake.bin
      {0x803761EC, true},  // Binary/NormalLagReduction.bin
      {0x80376238, true},  // Binary/NormalLagReduction.bin

      {0x800055f0, true},  // Common/EXITransferBuffer.asm
      {0x800055f8, true},  // Common/GetIsFollower.asm
      {0x800055fc, true},  // Common/Gecko/ProcessCodeList.asm
      {0x8016d294, true},  // Common/IncrementFrameIndex.asm
      {0x80376a24, true},  // Common/UseInGameDelay/ApplyInGameDelay.asm
      {0x8016e9b0, true},  // Common/UseInGameDelay/InitializeInGameDelay.asm
      {0x8000561c, true},  // Common/GetCommonMinorID/GetCommonMinorID.asm
      {0x802f666c, true},  // Common/UseInGameDelay/InitializeInGameDelay.asm v2

      {0x801a5b14, true},  // External/Salty Runback/Salty Runback.asm
      {0x801a4570, true},  // External/LagReduction/ForceHD/480pDeflickerOff.asm
      {0x802fccd8, true},  // External/Hide Nametag When Invisible/Hide Nametag When Invisible.asm

      {0x804ddb30, true},  // External/Widescreen/Adjust Offscreen Scissor/Fix Bubble
                           // Positions/Adjust Corner Value 1.asm
      {0x804ddb34, true},  // External/Widescreen/Adjust Offscreen Scissor/Fix Bubble
                           // Positions/Adjust Corner Value 2.asm
      {0x804ddb2c, true},  // External/Widescreen/Adjust Offscreen Scissor/Fix Bubble
                           // Positions/Extend Negative Vertical Bound.asm
      {0x804ddb28, true},  // External/Widescreen/Adjust Offscreen Scissor/Fix Bubble
                           // Positions/Extend Positive Vertical Bound.asm
      {0x804ddb4c, true},  // External/Widescreen/Adjust Offscreen Scissor/Fix Bubble
                           // Positions/Widen Bubble Region.asm
      {0x804ddb58, true},  // External/Widescreen/Adjust Offscreen Scissor/Adjust Bubble Zoom.asm
      {0x80086b24, true},  // External/Widescreen/Adjust Offscreen Scissor/Draw High Poly Models.asm
      {0x80030C7C, true},  // External/Widescreen/Adjust Offscreen Scissor/Left Camera Bound.asm
      {0x80030C88, true},  // External/Widescreen/Adjust Offscreen Scissor/Right Camera Bound.asm
      {0x802fcfc4,
       true},  // External/Widescreen/Nametag Fixes/Adjust Nametag Background X Scale.asm
      {0x804ddb84, true},  // External/Widescreen/Nametag Fixes/Adjust Nametag Text X Scale.asm
      {0x803BB05C, true},  // External/Widescreen/Fix Screen Flash.asm
      {0x8036A4A8, true},  // External/Widescreen/Overwrite CObj Values.asm
      {0x80302784, true},  // External/Monitor4-3/Add Shutters.asm
      {0x800C0148, true},  // External/FlashRedFailedLCancel/ChangeColor.asm
      {0x8008D690, true},  // External/FlashRedFailedLCancel/TriggerColor.asm

      {0x801A4DB4, true},  // Online/Core/ForceEngineOnRollback.asm
      {0x8016D310, true},  // Online/Core/HandleLRAS.asm
      {0x8034DED8, true},  // Online/Core/HandleRumble.asm
      {0x8016E748, true},  // Online/Core/InitOnlinePlay.asm
      {0x8016e904, true},  // Online/Core/InitPause.asm
      {0x801a5014, true},  // Online/Core/LoopEngineForRollback.asm
      {0x801a4de4, true},  // Online/Core/StartEngineLoop.asm
      {0x80376A28, true},  // Online/Core/TriggerSendInput.asm
      {0x801a4cb4, true},  // Online/Core/EXIFileLoad/AllocBuffer.asm
      {0x800163fc, true},  // Online/Core/EXIFileLoad/GetFileSize.asm
      {0x800166b8, true},  // Online/Core/EXIFileLoad/TransferFile.asm
      {0x80019260, true},  // Online/Core/Hacks/ForceNoDiskCrash.asm
      {0x80376304, true},  // Online/Core/Hacks/ForceNoVideoAssert.asm
      {0x80321d70, true},  // Online/Core/Hacks/PreventCharacterCrowdChants.asm
      {0x80019608, true},  // Online/Core/Hacks/PreventPadAlarmDuringRollback.asm
      {0x8038D224, true},  // Online/Core/Sound/AssignSoundInstanceId.asm
      {0x80088224, true},  // Online/Core/Sound/NoDestroyVoice.asm
      {0x800882B0, true},  // Online/Core/Sound/NoDestroyVoice2.asm
      {0x8038D0B0, true},  // Online/Core/Sound/PreventDuplicateSounds.asm
      {0x803775b8, true},  // Online/Logging/LogInputOnCopy.asm
      {0x8016e9b4, true},  // Online/Menus/InGame/InitInGame.asm
      {0x80185050, true},  // Online/Menus/VSScreen/HideStageDisplay/PreventEarlyR3Overwrite.asm
      {0x80184b1c, true},  // Online/Menus/VSScreen/HideStageText/SkipStageNumberShow.asm
      {0x801A45BC, true},  // Online/Slippi Online Scene/main.asm
      {0x801a45b8, true},  // Online/Slippi Online Scene/main.asm (https://bit.ly/3kxohf4)
      {0x801BFA20, true},  // Online/Slippi Online Scene/boot.asm
      {0x800cc818, true},  // External/GreenDuringWait/fall.asm
      {0x8008a478, true},  // External/GreenDuringWait/wait.asm

      {0x802f6690, true},  // HUD Transparency v1.1
                           // (https://smashboards.com/threads/transparent-hud-v1-1.508509/)
      {0x802F71E0,
       true},  // Smaller "Ready, GO!" (https://smashboards.com/threads/smaller-ready-go.509740/)
  };

  std::unordered_map<u32, bool> deny_list;
  deny_list.insert(s_deny_list.begin(), s_deny_list.end());

  auto replay_comm_settings = g_replay_comm->getSettings();
  if (replay_comm_settings.rollbackDisplayMethod == "off")
  {
    // Some codes should only be blacklisted when not displaying rollbacks, these are codes
    // that are required for things to not break when using Slippi savestates. Perhaps this
    // should be handled by actually applying these codes in the playback ASM instead? not sure
    deny_list[0x8038add0] = true;  // Online/Core/PreventFileAlarms/PreventMusicAlarm.asm
    deny_list[0x80023FFC] = true;  // Online/Core/PreventFileAlarms/MuteMusic.asm
  }

  m_gecko_list.clear();

  Slippi::GameSettings* settings = m_current_game->GetSettings();
  if (settings->geckoCodes.empty())
  {
    m_gecko_list = s_default_code_list;
    return;
  }

  std::vector<u8> source = settings->geckoCodes;
  INFO_LOG(SLIPPI, "Booting codes with source size: %d", source.size());

  int idx = 0;
  while (idx < source.size())
  {
    u8 code_type = source[idx] & 0xFE;
    u32 address =
        source[idx] << 24 | source[idx + 1] << 16 | source[idx + 2] << 8 | source[idx + 3];
    address = (address & 0x01FFFFFF) | 0x80000000;

    u32 code_offset = 8;  // Default code offset. Most codes are this length
    switch (code_type)
    {
    case 0xC0:
    case 0xC2:
    {
      u32 line_count =
          source[idx + 4] << 24 | source[idx + 5] << 16 | source[idx + 6] << 8 | source[idx + 7];
      code_offset = 8 + (line_count * 8);
      break;
    }
    case 0x08:
      code_offset = 16;
      break;
    case 0x06:
    {
      u32 byte_len =
          source[idx + 4] << 24 | source[idx + 5] << 16 | source[idx + 6] << 8 | source[idx + 7];
      code_offset =
          8 + ((byte_len + 7) & 0xFFFFFFF8);  // Round up to next 8 bytes and add the first 8 bytes
      break;
    }
    }

    idx += code_offset;

    // If this address is blacklisted, we don't add it to what we will send to game
    if (deny_list.count(address))
      continue;

    INFO_LOG(SLIPPI, "Codetype [%x] Inserting section: %d - %d (%x, %d)", code_type,
             idx - code_offset, idx, address, code_offset);

    // If not blacklisted, add code to return vector
    m_gecko_list.insert(m_gecko_list.end(), source.begin() + (idx - code_offset),
                        source.begin() + idx);
  }

  // Add the termination sequence
  m_gecko_list.insert(m_gecko_list.end(), {0xFF, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00});
}

void CEXISlippi::PrepareCharacterFrameData(Slippi::FrameData* frame, u8 port, u8 is_follower)
{
  std::unordered_map<uint8_t, Slippi::PlayerFrameData> source;
  source = is_follower ? frame->followers : frame->players;

  // This must be updated if new data is added
  int character_data_len = 49;

  // Check if player exists
  if (!source.count(port))
  {
    // If player does not exist, insert blank section
    m_read_queue.insert(m_read_queue.end(), character_data_len, 0);
    return;
  }

  // Get data for this player
  Slippi::PlayerFrameData data = source[port];

  // log << frameIndex << "\t" << port << "\t" << data.locationX << "\t" << data.locationY << "\t"
  // << data.animation
  // << "\n";

  // WARN_LOG(EXPANSIONINTERFACE, "[Frame %d] [Player %d] Positions: %f | %f", frameIndex, port,
  // data.locationX, data.locationY);

  // Add all of the inputs in order
  AppendWordToBuffer(&m_read_queue, data.randomSeed);
  AppendWordToBuffer(&m_read_queue, *(u32*)&data.joystickX);
  AppendWordToBuffer(&m_read_queue, *(u32*)&data.joystickY);
  AppendWordToBuffer(&m_read_queue, *(u32*)&data.cstickX);
  AppendWordToBuffer(&m_read_queue, *(u32*)&data.cstickY);
  AppendWordToBuffer(&m_read_queue, *(u32*)&data.trigger);
  AppendWordToBuffer(&m_read_queue, data.buttons);
  AppendWordToBuffer(&m_read_queue, *(u32*)&data.locationX);
  AppendWordToBuffer(&m_read_queue, *(u32*)&data.locationY);
  AppendWordToBuffer(&m_read_queue, *(u32*)&data.facingDirection);
  AppendWordToBuffer(&m_read_queue, (u32)data.animation);
  m_read_queue.push_back(data.joystickXRaw);
  AppendWordToBuffer(&m_read_queue, *(u32*)&data.percent);
  // NOTE TO DEV: If you add data here, make sure to increase the size above
}

bool CEXISlippi::CheckFrameFullyFetched(s32 frameIndex)
{
  auto does_frame_exist = m_current_game->DoesFrameExist(frameIndex);
  if (!does_frame_exist)
    return false;

  Slippi::FrameData* frame = m_current_game->GetFrame(frameIndex);

  version::Semver200_version last_finalized_version("3.7.0");
  version::Semver200_version current_version(m_current_game->GetVersionString());

  bool frame_is_finalized = true;
  if (current_version >= last_finalized_version)
  {
    // If latest finalized frame should exist, check it as well. This will prevent us
    // from loading a non-committed frame when mirroring a rollback game
    frame_is_finalized = m_current_game->GetLastFinalizedFrame() >= frameIndex;
  }

  // This flag is set to true after a post frame update has been received. At that point
  // we know we have received all of the input data for the frame
  return frame->inputsFullyFetched && frame_is_finalized;
}

void CEXISlippi::PrepareFrameData(u8* payload)
{
  // Since we are prepping new data, clear any existing data
  m_read_queue.clear();

  if (!m_current_game)
  {
    // Do nothing if we don't have a game loaded
    return;
  }

  // Parse input
  s32 frame_index = payload[0] << 24 | payload[1] << 16 | payload[2] << 8 | payload[3];

  // If loading from queue, move on to the next replay if we have past endFrame
  auto watch_settings = g_replay_comm->current;
  if (frame_index > watch_settings.endFrame)
  {
    INFO_LOG(SLIPPI, "Killing game because we are past endFrame");
    m_read_queue.push_back(FRAME_RESP_TERMINATE);
    return;
  }

  // If a new replay should be played, terminate the current game
  auto new_replay = g_replay_comm->isNewReplay();
  if (new_replay)
  {
    m_read_queue.push_back(FRAME_RESP_TERMINATE);
    return;
  }

  auto processing_complete = m_current_game->IsProcessingComplete();
  // Wait until frame exists in our data before reading it. We also wait until
  // next frame has been found to ensure we have actually received all of the
  // data from this frame. Don't wait until next frame is processing is complete
  // (this is the last frame, in that case)
  auto frame_found = m_current_game->DoesFrameExist(frame_index);
  g_playback_status->m_last_frame = m_current_game->GetLatestIndex();
  auto frame_complete = CheckFrameFullyFetched(frame_index);
  auto frame_ready = frame_found && (processing_complete || frame_complete);

  // If there is a startFrame configured, manage the fast-forward flag
  if (watch_settings.startFrame > Slippi::GAME_FIRST_FRAME)
  {
    if (frame_index < watch_settings.startFrame)
    {
      g_playback_status->SetHardFFW(true);
    }
    else if (frame_index == watch_settings.startFrame)
    {
      // TODO: This might disable fast forward on first frame when we dont want to?
      g_playback_status->SetHardFFW(false);
    }
  }

  auto commSettings = g_replay_comm->getSettings();
  if (commSettings.rollbackDisplayMethod == "normal")
  {
    auto next_frame = m_current_game->GetFrameAt(m_frame_seq_idx);
    bool should_hard_ffw =
        next_frame && next_frame->frame <= g_playback_status->m_curr_playback_frame;
    g_playback_status->SetHardFFW(should_hard_ffw);

    if (next_frame)
    {
      // This feels jank but without this g_playback_status ends up getting updated to
      // a value beyond the frame that actually gets played causes too much FFW
      frame_index = next_frame->frame;
    }
  }

  // If RealTimeMode is enabled, let's trigger fast forwarding under certain conditions
  auto far_behind = g_playback_status->m_last_frame - frame_index > 2;
  auto very_far_behind = g_playback_status->m_last_frame - frame_index > 25;
  if (far_behind && commSettings.mode == "mirror" && commSettings.isRealTimeMode)
  {
    g_playback_status->m_soft_ffw = true;

    // Once m_hard_ffw has been turned on, do not turn it off with this condition, should
    // hard FFW to the latest point
    if (!g_playback_status->m_hard_ffw)
      g_playback_status->m_hard_ffw = very_far_behind;
  }

  if (g_playback_status->m_last_frame == frame_index)
  {
    // The reason to disable fast forwarding here is in hopes
    // of disabling it on the last frame that we have actually received.
    // Doing this will allow the rendering logic to run to display the
    // last frame instead of the frame previous to fast forwarding.
    // Not sure if this fully works with partial frames
    g_playback_status->m_soft_ffw = false;
    g_playback_status->SetHardFFW(false);
  }

  bool should_ffw = g_playback_status->ShouldFFWFrame(frame_index);
  u8 request_result_code = should_ffw ? FRAME_RESP_FASTFORWARD : FRAME_RESP_CONTINUE;
  if (!frame_ready)
  {
    // If processing is complete, the game has terminated early. Tell our playback
    // to end the game as well.
    auto should_terminate_game = processing_complete;
    request_result_code = should_terminate_game ? FRAME_RESP_TERMINATE : FRAME_RESP_WAIT;
    m_read_queue.push_back(request_result_code);

    // Disable fast forward here too... this shouldn't be necessary but better
    // safe than sorry I guess
    g_playback_status->m_soft_ffw = false;
    g_playback_status->SetHardFFW(false);

    if (request_result_code == FRAME_RESP_TERMINATE)
    {
      ERROR_LOG(EXPANSIONINTERFACE, "Game should terminate on frame %d [%X]", frame_index,
                frame_index);
    }

    return;
  }

  u8 rollback_code = 0;  // 0 = not rollback, 1 = rollback, perhaps other options in the future?

  // Increment frame index if greater
  if (frame_index > g_playback_status->m_curr_playback_frame)
  {
    g_playback_status->m_curr_playback_frame = frame_index;
  }
  else if (commSettings.rollbackDisplayMethod != "off")
  {
    rollback_code = 1;
  }

  // Keep track of last FFW frame, used for soft FFW's
  if (should_ffw)
  {
    WARN_LOG(SLIPPI, "[Frame %d] FFW frame, behind by: %d frames.", frame_index,
             g_playback_status->m_last_frame - frame_index);
    g_playback_status->m_last_ffw_frame = frame_index;
  }

  // Return success code
  m_read_queue.push_back(request_result_code);

  // Get frame
  Slippi::FrameData* frame = m_current_game->GetFrame(frame_index);
  if (commSettings.rollbackDisplayMethod != "off")
  {
    auto previous_frame = m_current_game->GetFrameAt(m_frame_seq_idx - 1);
    frame = m_current_game->GetFrameAt(m_frame_seq_idx);

    *(s32*)(&m_playback_savestate_payload[0]) = Common::swap32(frame->frame);

    if (previous_frame && frame->frame <= previous_frame->frame)
    {
      // Here we should load a savestate
      HandleLoadSavestate(&m_playback_savestate_payload[0]);
    }

    // Here we should save a savestate
    HandleCaptureSavestate(&m_playback_savestate_payload[0]);

    m_frame_seq_idx += 1;
  }

  // For normal replays, modify slippi seek/playback data as needed
  // TODO: maybe handle other modes too?
  if (commSettings.mode == "normal" || commSettings.mode == "queue")
  {
    g_playback_status->PrepareSlippiPlayback(frame->frame);
  }

  // Push RB code
  m_read_queue.push_back(rollback_code);

  // Add frame rng seed to be restored at priority 0
  u8 rng_result = frame->randomSeedExists ? 1 : 0;
  m_read_queue.push_back(rng_result);
  AppendWordToBuffer(&m_read_queue, *(u32*)&frame->randomSeed);

  // Add frame data for every character
  for (u8 port = 0; port < 4; port++)
  {
    PrepareCharacterFrameData(frame, port, 0);
    PrepareCharacterFrameData(frame, port, 1);
  }
}

void CEXISlippi::PrepareIsStockSteal(u8* payload)
{
  // Since we are prepping new data, clear any existing data
  m_read_queue.clear();

  if (!m_current_game)
  {
    // Do nothing if we don't have a game loaded
    return;
  }

  // Parse args
  s32 frame_index = payload[0] << 24 | payload[1] << 16 | payload[2] << 8 | payload[3];
  u8 player_index = payload[4];

  // I'm not sure checking for the frame should be necessary. Theoretically this
  // should get called after the frame request so the frame should already exist
  auto frame_found = m_current_game->DoesFrameExist(frame_index);
  if (!frame_found)
  {
    m_read_queue.push_back(0);
    return;
  }

  // Load the data from this frame into the read buffer
  Slippi::FrameData* frame = m_current_game->GetFrame(frame_index);
  auto players = frame->players;

  u8 player_is_back = players.count(player_index) ? 1 : 0;
  m_read_queue.push_back(player_is_back);
}

void CEXISlippi::PrepareIsFileReady()
{
  m_read_queue.clear();

  auto new_replay = g_replay_comm->isNewReplay();
  if (!new_replay)
  {
    g_replay_comm->nextReplay();
    m_read_queue.push_back(0);
    return;
  }

  // Attempt to load game if there is a new replay file
  // this can come pack falsy if the replay file does not exist
  m_current_game = g_replay_comm->loadGame();
  if (!m_current_game)
  {
    // Do not start if replay file doesn't exist
    // TODO: maybe display error message?
    INFO_LOG(SLIPPI, "EXI_DeviceSlippi.cpp: Replay file does not exist?");
    m_read_queue.push_back(0);
    return;
  }

  INFO_LOG(SLIPPI, "EXI_DeviceSlippi.cpp: Replay file loaded successfully!?");

  // Clear playback control related vars
  g_playback_status->ResetPlayback();

  // Start the playback!
  m_read_queue.push_back(1);
}

bool CEXISlippi::IsDisconnected()
{
  if (!m_slippi_netplay)
    return true;

  auto status = m_slippi_netplay->GetSlippiConnectStatus();
  return status != SlippiNetplayClient::SlippiConnectStatus::NET_CONNECT_STATUS_CONNECTED;
}

static int s_temp_test_count = 0;
void CEXISlippi::HandleOnlineInputs(u8* payload)
{
  m_read_queue.clear();

  int32_t frame = payload[0] << 24 | payload[1] << 16 | payload[2] << 8 | payload[3];

  if (frame == 1)
  {
    m_available_savestates.clear();
    m_active_savestates.clear();

    // Prepare savestates for online play
    for (int i = 0; i < ROLLBACK_MAX_FRAMES; i++)
    {
      m_available_savestates.push_back(std::make_unique<SlippiSavestate>());
    }

    // Reset stall counter
    m_connection_stalled = false;
    m_stall_frame_count = 0;

    // Reset character selections as they are no longer needed
    m_local_selections.Reset();
    m_slippi_netplay->StartSlippiGame();
  }

  if (IsDisconnected())
  {
    m_read_queue.push_back(3);  // Indicate we disconnected
    return;
  }

  if (ShouldSkipOnlineFrame(frame))
  {
    // Send inputs that have not yet been acked
    m_slippi_netplay->SendSlippiPad(nullptr);
    m_read_queue.push_back(2);
    return;
  }

  HandleSendInputs(payload);
  PrepareOpponentInputs(payload);
}

bool CEXISlippi::ShouldSkipOnlineFrame(s32 frame)
{
  auto status = m_slippi_netplay->GetSlippiConnectStatus();
  bool connection_failed =
      status == SlippiNetplayClient::SlippiConnectStatus::NET_CONNECT_STATUS_FAILED;
  bool disconnected =
      status == SlippiNetplayClient::SlippiConnectStatus::NET_CONNECT_STATUS_DISCONNECTED;
  if (connection_failed || disconnected)
  {
    // If connection failed just continue the game
    return false;
  }

  if (m_connection_stalled)
  {
    return false;
  }

  // Return true if we are too far ahead for rollback. ROLLBACK_MAX_FRAMES is the number of frames
  // we can receive for the opponent at one time and is our "look-ahead" limit
  int32_t latest_remote_frame = m_slippi_netplay->GetSlippiLatestRemoteFrame();
  if (frame - latest_remote_frame >= ROLLBACK_MAX_FRAMES)
  {
    m_stall_frame_count++;
    if (m_stall_frame_count > 60 * 7)
    {
      // 7 second stall will disconnect game
      m_connection_stalled = true;
    }

    WARN_LOG(SLIPPI_ONLINE,
             "Halting for one frame due to rollback limit (frame: %d | latest: %d)...", frame,
             latest_remote_frame);
    return true;
  }

  m_stall_frame_count = 0;

  // Return true if we are over 60% of a frame ahead of our opponent. Currently limiting how
  // often this happens because I'm worried about jittery data causing a lot of unneccesary delays.
  // Only skip once for a given frame because our time detection method doesn't take into
  // consideration waiting for a frame. Also it's less jarring and it happens often enough that it
  // will smoothly get to the right place
  auto time_sync_frame = frame % SLIPPI_ONLINE_LOCKSTEP_INTERVAL;  // Only time sync every 30 frames
  if (time_sync_frame == 0 && !m_currently_skipping)
  {
    auto offset_us = m_slippi_netplay->CalcTimeOffsetUs();
    INFO_LOG(SLIPPI_ONLINE, "[Frame %d] Offset is: %d us", frame, offset_us);

    // TODO: figure out a better solution here for doubles?
    if (offset_us > 10000)
    {
      m_currently_skipping = true;

      int max_skip_frames = frame <= 120 ? 5 : 1;  // On early frames, support skipping more frames
      m_frames_to_skip = ((offset_us - 10000) / 16683) + 1;
      m_frames_to_skip = m_frames_to_skip > max_skip_frames ?
                             max_skip_frames :
                             m_frames_to_skip;  // Only skip 5 frames max

      WARN_LOG(SLIPPI_ONLINE, "Halting on frame %d due to time sync. Offset: %d us. Frames: %d...",
               frame, offset_us, m_frames_to_skip);
    }
  }

  // Handle the skipped frames
  if (m_frames_to_skip > 0)
  {
    // If ahead by 60% of a frame, stall. I opted to use 60% instead of half a frame
    // because I was worried about two systems continuously stalling for each other
    m_frames_to_skip = m_frames_to_skip - 1;
    return true;
  }

  m_currently_skipping = false;

  return false;
}

void CEXISlippi::HandleSendInputs(u8* payload)
{
  if (m_connection_stalled)
    return;

  int32_t frame = payload[0] << 24 | payload[1] << 16 | payload[2] << 8 | payload[3];
  u8 delay = payload[4];

  // On the first frame sent, we need to queue up empty dummy pads for as many
  // frames as we have delay
  if (frame == 1)
  {
    for (int i = 1; i <= delay; i++)
    {
      auto empty = std::make_unique<SlippiPad>(i);
      m_slippi_netplay->SendSlippiPad(std::move(empty));
    }
  }

  auto pad = std::make_unique<SlippiPad>(frame + delay, &payload[5]);

  m_slippi_netplay->SendSlippiPad(std::move(pad));
}

void CEXISlippi::PrepareOpponentInputs(u8* payload)
{
  m_read_queue.clear();

  u8 frame_result = 1;  // Indicates to continue frame

  auto state = m_slippi_netplay->GetSlippiConnectStatus();
  if (state != SlippiNetplayClient::SlippiConnectStatus::NET_CONNECT_STATUS_CONNECTED ||
      m_connection_stalled)
  {
    frame_result = 3;  // Indicates we have disconnected
  }

  m_read_queue.push_back(frame_result);  // Indicate a continue frame

  u8 remote_player_count = m_matchmaking->RemotePlayerCount();
  m_read_queue.push_back(remote_player_count);  // Indicate the number of remote players

  int32_t frame = payload[0] << 24 | payload[1] << 16 | payload[2] << 8 | payload[3];

  std::unique_ptr<SlippiRemotePadOutput> results[SLIPPI_REMOTE_PLAYER_MAX];
  int offset[SLIPPI_REMOTE_PLAYER_MAX];
  INFO_LOG(SLIPPI_ONLINE, "Preparing pad data for frame %d", frame);

  // Get pad data for each remote player and write each of their latest frame nums to the buf
  for (int i = 0; i < remote_player_count; i++)
  {
    results[i] = m_slippi_netplay->GetSlippiRemotePad(frame, i);

    // determine offset from which to copy data
    offset[i] = (results[i]->latestFrame - frame) * SLIPPI_PAD_FULL_SIZE;
    offset[i] = offset[i] < 0 ? 0 : offset[i];

    // add latest frame we are transfering to begining of return buf
    int32_t latest_frame = results[i]->latestFrame;
    if (latest_frame > frame)
      latest_frame = frame;
    AppendWordToBuffer(&m_read_queue, *(u32*)&latest_frame);
    // INFO_LOG(SLIPPI_ONLINE, "Sending frame num %d for pIdx %d (offset: %d)", latest_frame, i,
    // offset[i]);
  }
  // Send the current frame for any unused player slots.
  for (int i = remote_player_count; i < SLIPPI_REMOTE_PLAYER_MAX; i++)
  {
    AppendWordToBuffer(&m_read_queue, *(u32*)&frame);
  }

  // copy pad data over
  for (int i = 0; i < SLIPPI_REMOTE_PLAYER_MAX; i++)
  {
    std::vector<u8> tx;

    // Get pad data if this remote player exists
    if (i < remote_player_count)
    {
      auto tx_start = results[i]->data.begin() + offset[i];
      auto tx_end = results[i]->data.end();
      tx.insert(tx.end(), tx_start, tx_end);
    }

    tx.resize(SLIPPI_PAD_FULL_SIZE * ROLLBACK_MAX_FRAMES, 0);

    m_read_queue.insert(m_read_queue.end(), tx.begin(), tx.end());
  }

  m_slippi_netplay->DropOldRemoteInputs(frame);

  // ERROR_LOG(SLIPPI_ONLINE, "EXI: [%d] %X %X %X %X %X %X %X %X", latest_frame, m_read_queue[5],
  // m_read_queue[6], m_read_queue[7], m_read_queue[8], m_read_queue[9], m_read_queue[10],
  // m_read_queue[11], m_read_queue[12]);
}

void CEXISlippi::HandleCaptureSavestate(u8* payload)
{
  if (IsDisconnected())
    return;

  s32 frame = payload[0] << 24 | payload[1] << 16 | payload[2] << 8 | payload[3];

  u64 start_time = Common::Timer::GetTimeUs();

  // Grab an available savestate
  std::unique_ptr<SlippiSavestate> ss;
  if (!m_available_savestates.empty())
  {
    ss = std::move(m_available_savestates.back());
    m_available_savestates.pop_back();
  }
  else
  {
    // If there were no available savestates, use the oldest one
    auto it = m_active_savestates.begin();
    ss = std::move(it->second);
    m_active_savestates.erase(it->first);
  }

  // If there is already a savestate for this frame, remove it and add it to available
  if (m_active_savestates.count(frame))
  {
    m_available_savestates.push_back(std::move(m_active_savestates[frame]));
    m_active_savestates.erase(frame);
  }

  ss->Capture();
  m_active_savestates[frame] = std::move(ss);

  u32 time_diff = (u32)(Common::Timer::GetTimeUs() - start_time);
  INFO_LOG(SLIPPI_ONLINE, "SLIPPI ONLINE: Captured savestate for frame %d in: %f ms", frame,
           ((double)time_diff) / 1000);
}

void CEXISlippi::HandleLoadSavestate(u8* payload)
{
  s32 frame = payload[0] << 24 | payload[1] << 16 | payload[2] << 8 | payload[3];
  u32* preserve_arr = (u32*)(&payload[4]);

  if (!m_active_savestates.count(frame))
  {
    // This savestate does not exist... uhhh? What do we do?
    ERROR_LOG(SLIPPI_ONLINE, "SLIPPI ONLINE: Savestate for frame %d does not exist.", frame);
    return;
  }

  u64 start_time = Common::Timer::GetTimeUs();

  // Fetch preservation blocks
  std::vector<SlippiSavestate::PreserveBlock> blocks;

  // Get preservation blocks
  int idx = 0;
  while (Common::swap32(preserve_arr[idx]) != 0)
  {
    SlippiSavestate::PreserveBlock p = {Common::swap32(preserve_arr[idx]),
                                        Common::swap32(preserve_arr[idx + 1])};
    blocks.push_back(p);
    idx += 2;
  }

  // Load savestate
  m_active_savestates[frame]->Load(blocks);

  // Move all active savestates to available
  for (auto it = m_active_savestates.begin(); it != m_active_savestates.end(); ++it)
  {
    m_available_savestates.push_back(std::move(it->second));
  }

  m_active_savestates.clear();

  u32 time_diff = (u32)(Common::Timer::GetTimeUs() - start_time);
  INFO_LOG(SLIPPI_ONLINE, "SLIPPI ONLINE: Loaded savestate for frame %d in: %f ms", frame,
           ((double)time_diff) / 1000);
}

void CEXISlippi::StartFindMatch(u8* payload)
{
  SlippiMatchmaking::MatchSearchSettings search;
  search.mode = (SlippiMatchmaking::OnlinePlayMode)payload[0];

  std::string shift_jis_code;
  shift_jis_code.insert(shift_jis_code.begin(), &payload[1], &payload[1] + 18);
  shift_jis_code.erase(std::find(shift_jis_code.begin(), shift_jis_code.end(), 0x00),
                       shift_jis_code.end());

  // TODO: Make this work so we dont have to pass shiftJis to mm server
  // search.connectCode = SHIFTJISToUTF8(shift_jis_code).c_str();
  search.connectCode = shift_jis_code;

  // Store this search so we know what was queued for
  m_last_search = search;

  // While we do have another condition that checks characters after being connected, it's nice to
  // give someone an early error before they even queue so that they wont enter the queue and make
  // someone else get force removed from queue and have to requeue
  auto direct_mode = SlippiMatchmaking::OnlinePlayMode::DIRECT;
  if (search.mode < direct_mode && m_local_selections.characterId >= 26)
  {
    m_forced_error = "The character you selected is not allowed in this mode";
    return;
  }

#ifndef LOCAL_TESTING
  if (!m_enet_initialized)
  {
    // Initialize enet
    auto res = enet_initialize();
    if (res < 0)
      ERROR_LOG(SLIPPI_ONLINE, "Failed to initialize enet res: %d", res);

    m_enet_initialized = true;
  }

  m_matchmaking->FindMatch(search);
#endif
}

void CEXISlippi::PrepareOnlineMatchState()
{
  // This match block is a VS match with P1 Red Falco vs P2 Red Bowser vs P3 Young Link vs P4 Young
  // Link on Battlefield. The proper values will be overwritten
  static std::vector<u8> s_online_match_block = {
      0x32, 0x01, 0x86, 0x4C, 0xC3, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0x6E, 0x00,
      0x1F, 0x00, 0x00, 0x01, 0xE0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00, 0x3F,
      0x80, 0x00, 0x00, 0x3F, 0x80, 0x00, 0x00, 0x3F, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x14, 0x00, 0x04, 0x01, 0x00, 0x00, 0x00, 0x00, 0x09,
      0x00, 0x78, 0x00, 0xC0, 0x00, 0x04, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x3F, 0x80, 0x00, 0x00, 0x3F, 0x80, 0x00, 0x00, 0x3F, 0x80, 0x00, 0x00, 0x05, 0x00, 0x04,
      0x01, 0x00, 0x01, 0x00, 0x00, 0x09, 0x00, 0x78, 0x00, 0xC0, 0x00, 0x04, 0x01, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x3F, 0x80, 0x00, 0x00, 0x3F, 0x80, 0x00, 0x00, 0x3F,
      0x80, 0x00, 0x00, 0x15, 0x03, 0x04, 0x00, 0x00, 0xFF, 0x00, 0x00, 0x09, 0x00, 0x78, 0x00,
      0xC0, 0x00, 0x04, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x3F, 0x80, 0x00,
      0x00, 0x3F, 0x80, 0x00, 0x00, 0x3F, 0x80, 0x00, 0x00, 0x15, 0x03, 0x04, 0x00, 0x00, 0xFF,
      0x00, 0x00, 0x09, 0x00, 0x78, 0x00, 0xC0, 0x00, 0x04, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x3F, 0x80, 0x00, 0x00, 0x3F, 0x80, 0x00, 0x00, 0x3F, 0x80, 0x00, 0x00,
      0x21, 0x03, 0x04, 0x00, 0x00, 0xFF, 0x00, 0x00, 0x09, 0x00, 0x78, 0x00, 0x40, 0x00, 0x04,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x3F, 0x80, 0x00, 0x00, 0x3F, 0x80,
      0x00, 0x00, 0x3F, 0x80, 0x00, 0x00, 0x21, 0x03, 0x04, 0x00, 0x00, 0xFF, 0x00, 0x00, 0x09,
      0x00, 0x78, 0x00, 0x40, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x3F, 0x80, 0x00, 0x00, 0x3F, 0x80, 0x00, 0x00, 0x3F, 0x80, 0x00, 0x00,
  };

  m_read_queue.clear();

  auto error_state = SlippiMatchmaking::ProcessState::ERROR_ENCOUNTERED;

  SlippiMatchmaking::ProcessState mm_state =
      !m_forced_error.empty() ? error_state : m_matchmaking->GetMatchmakeState();

#ifdef LOCAL_TESTING
  if (m_local_selections.isCharacterSelected || is_local_connected)
  {
    mm_state = SlippiMatchmaking::ProcessState::CONNECTION_SUCCESS;
    is_local_connected = true;
  }
#endif

  m_read_queue.push_back(mm_state);  // Matchmaking State

  u8 local_player_ready = m_local_selections.isCharacterSelected;
  u8 remote_players_ready = 0;
  u8 local_player_index = m_matchmaking->LocalPlayerIndex();
  u8 remote_player_index = 1;

  auto user_info = m_user->GetUserInfo();

  if (mm_state == SlippiMatchmaking::ProcessState::CONNECTION_SUCCESS)
  {
    if (!m_slippi_netplay)
    {
#ifdef LOCAL_TESTING
      m_slippi_netplay = std::make_unique<SlippiNetplayClient>(true);
#else
      m_slippi_netplay = m_matchmaking->GetNetplayClient();
#endif

      m_slippi_netplay->SetMatchSelections(m_local_selections);
    }

#ifdef LOCAL_TESTING
    bool connected = true;
#else
    auto status = m_slippi_netplay->GetSlippiConnectStatus();
    bool connected =
        status == SlippiNetplayClient::SlippiConnectStatus::NET_CONNECT_STATUS_CONNECTED;
#endif

    if (connected)
    {
      auto match_info = m_slippi_netplay->GetMatchInfo();
#ifdef LOCAL_TESTING
      remote_players_ready = true;
#else
      remote_players_ready = 1;
      u8 remote_player_count = m_matchmaking->RemotePlayerCount();
      for (int i = 0; i < remote_player_count; i++)
      {
        if (!match_info->remotePlayerSelections[i].isCharacterSelected)
        {
          remote_players_ready = 0;
        }
      }

      if (remote_player_count == 1)
      {
        auto is_decider = m_slippi_netplay->IsDecider();
        local_player_index = is_decider ? 0 : 1;
        remote_player_index = is_decider ? 1 : 0;
      }
#endif

      auto is_decider = m_slippi_netplay->IsDecider();
      local_player_index = is_decider ? 0 : 1;
      remote_player_index = is_decider ? 1 : 0;
    }
    else
    {
#ifndef LOCAL_TESTING
      // If we get here, our opponent likely disconnected. Let's trigger a clean up
      HandleConnectionCleanup();
      PrepareOnlineMatchState();  // run again with new state
      return;
#endif
    }
    // Here we are connected, check to see if we should init play session
    if (!m_play_session_active)
    {
      std::vector<std::string> uids;

      auto mm_players = m_matchmaking->GetPlayerInfo();
      for (auto mmp : mm_players)
      {
        uids.push_back(mmp.uid);
      }

      m_game_reporter->StartNewSession(uids);

      m_play_session_active = true;
    }
  }
  else
  {
    m_slippi_netplay = nullptr;
  }

  u32 rng_offset = 0;
  std::string local_player_name = "";
  std::string opp_name = "";
  std::string p1_name = "";
  std::string p2_name = "";
  u8 chat_message_id = 0;
  u8 chat_message_player_idx = 0;
  u8 sent_chat_message_id = 0;

#ifdef LOCAL_TESTING
  local_player_index = 0;
  chat_message_id = local_chat_msg_id;
  chat_message_player_idx = 0;
  local_chat_msg_id = 0;
  // in CSS p1 is always current player and p2 is opponent
  local_player_name = p1_name = "Player 1";
  opp_name = p2_name = "Player 2";
#endif

  m_read_queue.push_back(local_player_ready);    // Local player ready
  m_read_queue.push_back(remote_players_ready);  // Remote players ready
  m_read_queue.push_back(local_player_index);    // Local player index
  m_read_queue.push_back(remote_player_index);   // Remote player index

  // Set chat message if any
  if (m_slippi_netplay)
  {
    auto remote_msg_selection = m_slippi_netplay->GetSlippiRemoteChatMessage();
    chat_message_id = remote_msg_selection.messageId;
    chat_message_player_idx = remote_msg_selection.playerIdx;
    sent_chat_message_id = m_slippi_netplay->GetSlippiRemoteSentChatMessage();
    // in CSS p1 is always current player and p2 is opponent
    local_player_name = p1_name = user_info.display_name;
  }

  auto direct_mode = SlippiMatchmaking::OnlinePlayMode::DIRECT;

  std::vector<u8> left_team_players = {};
  std::vector<u8> right_team_players = {};

  if (local_player_ready && remote_players_ready)
  {
    auto is_decider = m_slippi_netplay->IsDecider();
    u8 remote_player_count = m_matchmaking->RemotePlayerCount();
    auto match_info = m_slippi_netplay->GetMatchInfo();
    SlippiPlayerSelections lps = match_info->localPlayerSelections;
    auto rps = match_info->remotePlayerSelections;

#ifdef LOCAL_TESTING
    lps.playerIdx = 0;

    // By default Local testing for teams is against
    // 1 RED TEAM Falco
    // 2 BLUE TEAM Falco
    for (int i = 0; i <= SLIPPI_REMOTE_PLAYER_MAX; i++)
    {
      if (i == 0)
      {
        rps[i].characterColor = 1;
        rps[i].teamId = 0;
      }
      else
      {
        rps[i].characterColor = 2;
        rps[i].teamId = 1;
      }

      rps[i].characterId = 0x14;
      rps[i].playerIdx = i + 1;
      rps[i].isCharacterSelected = true;
    }

    if (m_last_search.mode == SlippiMatchmaking::OnlinePlayMode::TEAMS)
    {
      remotePlayerCount = 3;
    }

    opp_name = std::string("Player");
#endif

    // Check if someone is picking dumb characters in non-direct
    auto local_char_ok = lps.characterId < 26;
    auto remote_char_ok = true;
    INFO_LOG(SLIPPI_ONLINE, "remote_player_count: %d", remote_player_count);
    for (int i = 0; i < remote_player_count; i++)
    {
      if (rps[i].characterId >= 26)
        remote_char_ok = false;
    }
    if (m_last_search.mode < direct_mode && (!local_char_ok || !remote_char_ok))
    {
      // If we get here, someone is doing something bad, clear the lobby
      HandleConnectionCleanup();
      if (!local_char_ok)
        m_forced_error = "The character you selected is not allowed in this mode";
      PrepareOnlineMatchState();
      return;
    }

    // Overwrite local player character
    s_online_match_block[0x60 + (lps.playerIdx) * 0x24] = lps.characterId;
    s_online_match_block[0x63 + (lps.playerIdx) * 0x24] = lps.characterColor;
    s_online_match_block[0x67 + (lps.playerIdx) * 0x24] = 0;
    s_online_match_block[0x69 + (lps.playerIdx) * 0x24] = lps.teamId;

    // Overwrite remote player character
    for (int i = 0; i < remote_player_count; i++)
    {
      u8 idx = rps[i].playerIdx;
      onlineMatchBlock[0x60 + idx * 0x24] = rps[i].characterId;
      onlineMatchBlock[0x63 + idx * 0x24] = rps[i].characterColor;
      onlineMatchBlock[0x69 + idx * 0x24] = rps[i].teamId;
    }

    // Handle Singles/Teams specific logic
    if (remote_player_count < 3)
    {
      s_online_match_block[0x8] = 0;  // is Teams = false

      // Set p3/p4 player type to none
      s_online_match_block[0x61 + 2 * 0x24] = 3;
      s_online_match_block[0x61 + 3 * 0x24] = 3;

      // Make one character lighter if same character, same color
      bool ish_sheik_vs_zelda = lps.characterId == 0x12 && rps[0].characterId == 0x13 ||
                                lps.characterId == 0x13 && rps[0].characterId == 0x12;
      bool char_match = lps.characterId == rps[0].characterId || ish_sheik_vs_zelda;
      bool color_match = lps.characterColor == rps[0].characterColor;

      s_online_match_block[0x67 + 0x24] = char_match && color_match ? 1 : 0;
    }
    else
    {
      s_online_match_block[0x8] = 1;  // is Teams = true

      // Set p3/p4 player type to human
      s_online_match_block[0x61 + 2 * 0x24] = 0;
      s_online_match_block[0x61 + 3 * 0x24] = 0;
    }

    // Overwrite stage
    u16 stage_id;
    if (is_decider)
    {
      stage_id = lps.isStageSelected ? lps.stageId : rps[0].stageId;
    }
    else
    {
      stage_id = rps[0].isStageSelected ? rps[0].stageId : lps.stageId;
    }

    u16* stage = (u16*)&s_online_match_block[0xE];
    *stage = Common::swap16(stage_id);

    // Set rng offset
    rng_offset = is_decider ? lps.rngOffset : rps[0].rngOffset;
    WARN_LOG(SLIPPI_ONLINE, "Rng Offset: 0x%x", rng_offset);
    WARN_LOG(SLIPPI_ONLINE, "P1 Char: 0x%X, P2 Char: 0x%X", s_online_match_block[0x60],
             s_online_match_block[0x84]);

    // Turn pause on in direct, off in everything else
    u8* game_bit_field_3 = static_cast<u8*>(&s_online_match_block[2]);
    *game_bit_field_3 =
        m_last_search.mode >= direct_mode ? *game_bit_field_3 & 0xF7 : *game_bit_field_3 | 0x8;
    //*game_bit_field_3 = *game_bit_field_3 | 0x8;

    // Group players into left/right side for team splash screen display
    for (int i = 0; i < 4; i++)
    {
      int team_id = s_online_match_block[0x69 + i * 0x24];
      if (team_id == lps.teamId)
        left_team_players.push_back(i);
      else
        right_team_players.push_back(i);
    }
    int left_team_size = static_cast<int>(left_team_players.size());
    int right_team_size = static_cast<int>(right_team_players.size());
    left_team_players.resize(4, 0);
    right_team_players.resize(4, 0);
    left_team_players[3] = static_cast<u8>(left_team_size);
    right_team_players[3] = static_cast<u8>(right_team_size);
  }

  // Add rng offset to output
  AppendWordToBuffer(&m_read_queue, rng_offset);

  // Add delay frames to output
  m_read_queue.push_back((u8)SConfig::GetInstance().m_slippiOnlineDelay);

  // Add chat messages id
  m_read_queue.push_back((u8)sent_chat_message_id);
  m_read_queue.push_back((u8)chat_message_id);
  m_read_queue.push_back((u8)chat_message_player_idx);

  // Add player groupings for VS splash screen
  left_team_players.resize(4, 0);
  right_team_players.resize(4, 0);
  m_read_queue.insert(m_read_queue.end(), left_team_players.begin(), left_team_players.end());
  m_read_queue.insert(m_read_queue.end(), right_team_players.begin(), right_team_players.end());

  // Add names to output
  // Always send static local player name
  local_player_name = ConvertStringForGame(local_player_name, MAX_NAME_LENGTH);
  m_read_queue.insert(m_read_queue.end(), local_player_name.begin(), local_player_name.end());

#ifdef LOCAL_TESTING
  std::string default_names[] = {"Player 1", "Player 2", "Player 3", "Player 4"};
#endif

  for (int i = 0; i < 4; i++)
  {
    std::string name = m_matchmaking->GetPlayerName(i);
#ifdef LOCAL_TESTING
    name = default_names[i];
#endif
    name = ConvertStringForGame(name, MAX_NAME_LENGTH);
    m_read_queue.insert(m_read_queue.end(), name.begin(), name.end());
  }

  // Create the opponent string using the names of all players on opposing teams
  int team_idx = s_online_match_block[0x69 + local_player_index * 0x24];
  std::string opp_text = "";
  for (int i = 0; i < 4; i++)
  {
    if (i == local_player_index)
      continue;

    if (s_online_match_block[0x69 + i * 0x24] != team_idx)
    {
      if (opp_text != "")
        opp_text += "/";

      opp_text += m_matchmaking->GetPlayerName(i);
    }
  }
  if (m_matchmaking->RemotePlayerCount() == 1)
    opp_text = m_matchmaking->GetPlayerName(remote_player_index);
  opp_name = ConvertStringForGame(opp_text, MAX_NAME_LENGTH * 2 + 1);
  m_read_queue.insert(m_read_queue.end(), opp_name.begin(), opp_name.end());

#ifdef LOCAL_TESTING
  std::string default_connect_codes[] = {"PLYR#001", "PLYR#002", "PLYR#003", "PLYR#004"};
#endif

  auto player_info = m_matchmaking->GetPlayerInfo();
  for (int i = 0; i < 4; i++)
  {
    std::string connect_code = i < player_info.size() ? player_info[i].connect_code : "";
#ifdef LOCAL_TESTING
    connect_code = default_connect_codes[i];
#endif
    connect_code = ConvertConnectCodeForGame(connect_code);
    m_read_queue.insert(m_read_queue.end(), connect_code.begin(), connect_code.end());
  }

  // Add error message if there is one
  auto error_str = !m_forced_error.empty() ? m_forced_error : m_matchmaking->GetErrorMessage();
  error_str = ConvertStringForGame(error_str, 120);
  m_read_queue.insert(m_read_queue.end(), error_str.begin(), error_str.end());

  // Add the match struct block to output
  m_read_queue.insert(m_read_queue.end(), s_online_match_block.begin(), s_online_match_block.end());
}

u16 CEXISlippi::GetRandomStage()
{
  static u16 selected_stage;

  static std::vector<u16> stages = {
      0x2,   // FoD
      0x3,   // Pokemon
      0x8,   // Yoshi's Story
      0x1C,  // Dream Land
      0x1F,  // Battlefield
      0x20,  // Final Destination
  };

  // Reset stage pool if it's empty
  if (m_stage_pool.empty())
    m_stage_pool.insert(m_stage_pool.end(), stages.begin(), stages.end());

  // Get random stage
  int rand_idx = generator() % m_stage_pool.size();
  selected_stage = m_stage_pool[rand_idx];

  // Remove last selection from stage pool
  m_stage_pool.erase(m_stage_pool.begin() + rand_idx);

  return selected_stage;
}

void CEXISlippi::SetMatchSelections(u8* payload)
{
  SlippiPlayerSelections s;

  s.teamId = payload[0];
  s.characterId = payload[1];
  s.characterColor = payload[2];
  s.isCharacterSelected = payload[3];

  s.stageId = Common::swap16(&payload[4]);
  u8 stage_select_option = payload[6];

  s.isStageSelected = stage_select_option == 1 || stage_select_option == 3;
  if (stage_select_option == 3)
  {
    // If stage requested is random, select a random stage
    s.stageId = GetRandomStage();
  }

  INFO_LOG(SLIPPI, "LPS set char: %d, iSS: %d, %d, stage: %d, team: %d", s.isCharacterSelected,
           stage_select_option, s.isStageSelected, s.stageId, s.teamId);

  s.rngOffset = generator() % 0xFFFF;

  if (m_matchmaking->LocalPlayerIndex() == 1 && m_first_match)
  {
    m_first_match = false;
    s.stageId = GetRandomStage();
  }

  // Merge these selections
  m_local_selections.Merge(s);

  if (m_slippi_netplay)
  {
    m_slippi_netplay->SetMatchSelections(m_local_selections);
  }
}

void CEXISlippi::PrepareFileLength(u8* payload)
{
  m_read_queue.clear();

  std::string file_name((char*)&payload[0]);

  std::string contents;
  u32 size = m_game_file_loader->LoadFile(file_name, contents);

  INFO_LOG(SLIPPI, "Getting file size for: %s -> %d", file_name.c_str(), size);

  // Write size to output
  AppendWordToBuffer(&m_read_queue, size);
}

void CEXISlippi::PrepareFileLoad(u8* payload)
{
  m_read_queue.clear();

  std::string file_name((char*)&payload[0]);

  std::string contents;
  u32 size = m_game_file_loader->LoadFile(file_name, contents);
  std::vector<u8> buf(contents.begin(), contents.end());

  INFO_LOG(SLIPPI, "Writing file contents: %s -> %d", file_name.c_str(), size);

  // Write the contents to output
  m_read_queue.insert(m_read_queue.end(), buf.begin(), buf.end());
}

void CEXISlippi::PrepareGctLength()
{
  m_read_queue.clear();

  u32 size = Gecko::GetGctLength();

  INFO_LOG(SLIPPI, "Getting gct size: %d", size);

  // Write size to output
  AppendWordToBuffer(&m_read_queue, size);
}

void CEXISlippi::PrepareGctLoad(u8* payload)
{
  m_read_queue.clear();

  auto gct = Gecko::GenerateGct();

  // This is the address where the codes will be written to
  auto address = Common::swap32(&payload[0]);

  INFO_LOG(SLIPPI, "Preparing to write gecko codes at: 0x%X", address);

  m_read_queue.insert(m_read_queue.end(), gct.begin(), gct.end());
}

void CEXISlippi::HandleChatMessage(u8* payload)
{
  int msg_id = payload[0];
  INFO_LOG(SLIPPI, "SLIPPI CHAT INPUT: 0x%x", msg_id);

#ifdef LOCAL_TESTING
  local_chat_msg_id = 11;
#endif

  if (m_slippi_netplay)
  {
    auto user_info = m_user->GetUserInfo();
    auto packet = std::make_unique<sf::Packet>();
    //		OSD::AddMessage("[Me]: "+ msg, OSD::Duration::VERY_LONG, OSD::Color::YELLOW);
    m_slippi_netplay->remoteSentChatMessageId = msg_id;
    m_slippi_netplay->WriteChatMessageToPacket(*packet, msg_id,
                                               m_slippi_netplay->LocalPlayerPort());
    m_slippi_netplay->SendAsync(std::move(packet));
  }
}

void CEXISlippi::LogMessageFromGame(u8* payload)
{
  if (payload[0] == 0)
  {
    // The first byte indicates whether to log the time or not
    GENERIC_LOG(Common::Log::SLIPPI, (Common::Log::LOG_LEVELS)payload[1], "%s", (char*)&payload[2]);
  }
  else
  {
    GENERIC_LOG(Common::Log::SLIPPI, (Common::Log::LOG_LEVELS)payload[1], "%s: %llu",
                (char*)&payload[2], Common::Timer::GetTimeUs());
  }
}

void CEXISlippi::HandleLogInRequest()
{
  bool login_res = m_user->AttemptLogin();
  if (!login_res)
  {
    if (Host_RendererIsFullscreen())
      Host_Fullscreen();
    Host_LowerWindow();
    m_user->OpenLogInPage();
    m_user->ListenForLogIn();
  }
}

void CEXISlippi::HandleLogOutRequest()
{
  m_user->LogOut();
}

void CEXISlippi::HandleUpdateAppRequest()
{
#ifdef __APPLE__
  CriticalAlertT("Automatic updates are not available for macOS, please get the latest update from "
                 "slippi.gg/netplay.");
#else
  Host_LowerWindow();
  m_user->UpdateApp();
  Host_Exit();
#endif
}

void CEXISlippi::PrepareOnlineStatus()
{
  m_read_queue.clear();

  auto logged_in = m_user->IsLoggedIn();
  auto user_info = m_user->GetUserInfo();

  u8 app_state = 0;
  if (logged_in)
  {
    // Check if we have the latest version, and if not, indicate we need to update
    version::Semver200_version latest_version(user_info.latest_version);
    version::Semver200_version current_version(Common::scm_slippi_semver_str);

    app_state = latest_version > current_version ? 2 : 1;
  }

  m_read_queue.push_back(app_state);

  // Write player name (31 bytes)
  std::string player_name = ConvertStringForGame(user_info.display_name, MAX_NAME_LENGTH);
  m_read_queue.insert(m_read_queue.end(), player_name.begin(), player_name.end());

  // Write connect code (10 bytes)
  std::string connect_code = user_info.connect_code;
  char shift_jit_hashtag[] = {'\x81', '\x94', '\x00'};
  connect_code.resize(CONNECT_CODE_LENGTH);
  connect_code = ReplaceAll(connect_code, "#", shift_jit_hashtag);
  auto code_buf = connect_code.c_str();
  m_read_queue.insert(m_read_queue.end(), code_buf, code_buf + CONNECT_CODE_LENGTH + 2);
}

void doConnectionCleanup(std::unique_ptr<SlippiMatchmaking> mm,
                         std::unique_ptr<SlippiNetplayClient> nc)
{
  if (mm)
    mm.reset();

  if (nc)
    nc.reset();
}

void CEXISlippi::HandleConnectionCleanup()
{
  ERROR_LOG(SLIPPI_ONLINE, "Connection cleanup started...");

  // Handle destructors in a separate thread to not block the main thread
  std::thread cleanup(doConnectionCleanup, std::move(m_matchmaking), std::move(m_slippi_netplay));
  cleanup.detach();

  // Reset matchmaking
  m_matchmaking = std::make_unique<SlippiMatchmaking>(m_user.get());

  // Disconnect netplay client
  m_slippi_netplay = nullptr;

  // Clear character selections
  m_local_selections.Reset();

  // Reset random stage pool
  m_stage_pool.clear();

  // Reset any forced errors
  m_forced_error.clear();

  // Reset play session
  m_play_session_active = false;
  m_first_match = true;

#ifdef LOCAL_TESTING
  is_local_connected = false;
#endif

  ERROR_LOG(SLIPPI_ONLINE, "Connection cleanup completed...");
}

void CEXISlippi::PrepareNewSeed()
{
  m_read_queue.clear();

  u32 new_seed = generator() % 0xFFFFFFFF;

  AppendWordToBuffer(&m_read_queue, new_seed);
}

void CEXISlippi::HandleReportGame(u8* payload)
{
#ifndef LOCAL_TESTING
  SlippiGameReporter::GameReport r;
  r.duration_frames = Common::swap32(&payload[0]);

  // ERROR_LOG(SLIPPI_ONLINE, "Frames: %d", r.duration_frames);

  for (auto i = 0; i < 2; ++i)
  {
    SlippiGameReporter::PlayerReport p;
    auto offset = i * 6;
    p.stocks_remaining = payload[5 + offset];

    auto swapped_damage_done = Common::swap32(&payload[6 + offset]);
    p.damage_done = *(float*)&swapped_damage_done;

    // ERROR_LOG(SLIPPI_ONLINE, "Stocks: %d, DamageDone: %f", p.stocks_remaining, p.damage_done);

    r.players.push_back(p);
  }

  m_game_reporter->StartReport(r);
#endif
}

void CEXISlippi::DMAWrite(u32 _uAddr, u32 _uSize)
{
  u8* mem_ptr = Memory::GetPointer(_uAddr);

  u32 buf_loc = 0;

  if (mem_ptr == nullptr)
  {
    NOTICE_LOG(SLIPPI, "DMA Write was passed an invalid address: %x", _uAddr);
    Dolphin_Debugger::PrintCallstack(Common::Log::SLIPPI, Common::Log::LNOTICE);
    m_read_queue.clear();
    return;
  }

  u8 byte = mem_ptr[0];
  if (byte == CMD_RECEIVE_COMMANDS)
  {
    time(&m_game_start_time);  // Store game start time
    u8 receiveCommandsLen = mem_ptr[1];
    ConfigureCommands(&mem_ptr[1], receiveCommandsLen);
    WriteToFileAsync(&mem_ptr[0], receiveCommandsLen + 1, "create");
    buf_loc += receiveCommandsLen + 1;
    g_need_input_for_frame = true;
    SlippiSpectateServer::getInstance().startGame();
    SlippiSpectateServer::getInstance().write(&mem_ptr[0], receiveCommandsLen + 1);
  }

  if (byte == CMD_MENU_FRAME)
  {
    SlippiSpectateServer::getInstance().write(&mem_ptr[0], _uSize);
    g_need_input_for_frame = true;
  }

  INFO_LOG(EXPANSIONINTERFACE,
           "EXI SLIPPI DMAWrite: addr: 0x%08x size: %d, buf_loc:[%02x %02x %02x %02x %02x]", _uAddr,
           _uSize, mem_ptr[buf_loc], mem_ptr[buf_loc + 1], mem_ptr[buf_loc + 2],
           mem_ptr[buf_loc + 3], mem_ptr[buf_loc + 4]);

  while (buf_loc < _uSize)
  {
    byte = mem_ptr[buf_loc];
    if (!payload_sizes.count(byte))
    {
      // This should never happen. Do something else if it does?
      WARN_LOG(EXPANSIONINTERFACE, "EXI SLIPPI: Invalid command byte: 0x%x", byte);
      return;
    }

    u32 payload_len = payload_sizes[byte];
    switch (byte)
    {
    case CMD_RECEIVE_GAME_END:
      WriteToFileAsync(&mem_ptr[buf_loc], payload_len + 1, "close");
      SlippiSpectateServer::getInstance().write(&mem_ptr[buf_loc], payload_len + 1);
      SlippiSpectateServer::getInstance().endGame();
      break;
    case CMD_PREPARE_REPLAY:
      // log.open("log.txt");
      PrepareGameInfo(&mem_ptr[buf_loc + 1]);
      break;
    case CMD_READ_FRAME:
      PrepareFrameData(&mem_ptr[buf_loc + 1]);
      break;
    case CMD_FRAME_BOOKEND:
      g_need_input_for_frame = true;
      WriteToFileAsync(&mem_ptr[buf_loc], payload_len + 1, "");
      SlippiSpectateServer::getInstance().write(&mem_ptr[buf_loc], payload_len + 1);
      break;
    case CMD_IS_STOCK_STEAL:
      PrepareIsStockSteal(&mem_ptr[buf_loc + 1]);
      break;
    case CMD_IS_FILE_READY:
      PrepareIsFileReady();
      break;
    case CMD_GET_GECKO_CODES:
      m_read_queue.clear();
      m_read_queue.insert(m_read_queue.begin(), m_gecko_list.begin(), m_gecko_list.end());
      break;
    case CMD_ONLINE_INPUTS:
      HandleOnlineInputs(&mem_ptr[buf_loc + 1]);
      break;
    case CMD_CAPTURE_SAVESTATE:
      HandleCaptureSavestate(&mem_ptr[buf_loc + 1]);
      break;
    case CMD_LOAD_SAVESTATE:
      HandleLoadSavestate(&mem_ptr[buf_loc + 1]);
      break;
    case CMD_GET_MATCH_STATE:
      PrepareOnlineMatchState();
      break;
    case CMD_FIND_OPPONENT:
      StartFindMatch(&mem_ptr[buf_loc + 1]);
      break;
    case CMD_SET_MATCH_SELECTIONS:
      SetMatchSelections(&mem_ptr[buf_loc + 1]);
      break;
    case CMD_FILE_LENGTH:
      PrepareFileLength(&mem_ptr[buf_loc + 1]);
      break;
    case CMD_FILE_LOAD:
      PrepareFileLoad(&mem_ptr[buf_loc + 1]);
      break;
    case CMD_OPEN_LOGIN:
      HandleLogInRequest();
      break;
    case CMD_LOGOUT:
      HandleLogOutRequest();
      break;
    case CMD_GET_ONLINE_STATUS:
      PrepareOnlineStatus();
      break;
    case CMD_CLEANUP_CONNECTION:
      HandleConnectionCleanup();
      break;
    case CMD_LOG_MESSAGE:
      LogMessageFromGame(&mem_ptr[buf_loc + 1]);
      break;
    case CMD_SEND_CHAT_MESSAGE:
      HandleChatMessage(&mem_ptr[buf_loc + 1]);
      break;
    case CMD_UPDATE:
      HandleUpdateAppRequest();
      break;
    case CMD_GET_NEW_SEED:
      PrepareNewSeed();
      break;
    case CMD_REPORT_GAME:
      HandleReportGame(&mem_ptr[buf_loc + 1]);
      break;
    case CMD_GCT_LENGTH:
      PrepareGctLength();
      break;
    case CMD_GCT_LOAD:
      PrepareGctLoad(&mem_ptr[buf_loc + 1]);
      break;
    default:
      WriteToFileAsync(&mem_ptr[buf_loc], payload_len + 1, "");
      SlippiSpectateServer::getInstance().write(&mem_ptr[buf_loc], payload_len + 1);
      break;
    }

    buf_loc += payload_len + 1;
  }
}

void CEXISlippi::DMARead(u32 addr, u32 size)
{
  if (m_read_queue.empty())
  {
    INFO_LOG(EXPANSIONINTERFACE, "EXI SLIPPI DMARead: Empty");
    return;
  }

  m_read_queue.resize(size, 0);  // Resize response array to make sure it's all full/allocated

  auto queueAddr = &m_read_queue[0];
  INFO_LOG(EXPANSIONINTERFACE,
           "EXI SLIPPI DMARead: addr: 0x%08x size: %d, startResp: [%02x %02x %02x %02x %02x]", addr,
           size, queueAddr[0], queueAddr[1], queueAddr[2], queueAddr[3], queueAddr[4]);

  // Copy buffer data to memory
  Memory::CopyToEmu(addr, queueAddr, size);
}

bool CEXISlippi::IsPresent() const
{
  return true;
}

void CEXISlippi::TransferByte(u8& byte)
{
}
}  // namespace ExpansionInterface
