// Copyright 2008 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#pragma once

// Directory separators, do we need this?
#define DIR_SEP "/"
#define DIR_SEP_CHR '/'

// The user data dir
#define ROOT_DIR "."
#ifdef _WIN32
#define USERDATA_DIR "User"
#define DOLPHIN_DATA_DIR "Dolphin"
#elif defined __APPLE__
// On OS X, USERDATA_DIR exists within the .app, but *always* reference
// the copy in Application Support instead! (Copied on first run)
// You can use the File::GetUserPath() util for this
#define USERDATA_DIR "Contents/Resources/User"
#define DOLPHIN_DATA_DIR "Library/Application Support/Dolphin"
#elif defined ANDROID
#define USERDATA_DIR "user"
#define DOLPHIN_DATA_DIR "/sdcard/dolphin-emu"
#define NOMEDIA_FILE ".nomedia"
#else
#define USERDATA_DIR "user"
#define DOLPHIN_DATA_DIR "SlippiOnline"
#define PLAYBACK_DATA_DIR "SlippiPlayback"
#endif

// Dirs in both User and Sys
#define EUR_DIR "EUR"
#define USA_DIR "USA"
#define JAP_DIR "JAP"

// Subdirs in the User dir returned by GetUserPath(D_USER_IDX)
#define GC_USER_DIR "GC"
#define WII_USER_DIR "Wii"
#define CONFIG_DIR "Config"
#define GAMESETTINGS_DIR "GameSettings"
#define MAPS_DIR "Maps"
#define CACHE_DIR "Cache"
#define COVERCACHE_DIR "GameCovers"
#define REDUMPCACHE_DIR "Redump"
#define SHADERCACHE_DIR "Shaders"
#define STATESAVES_DIR "StateSaves"
#define SCREENSHOTS_DIR "ScreenShots"
#define LOAD_DIR "Load"
#define HIRES_TEXTURES_DIR "Textures"
#define DUMP_DIR "Dump"
#define DUMP_TEXTURES_DIR "Textures"
#define DUMP_FRAMES_DIR "Frames"
#define DUMP_OBJECTS_DIR "Objects"
#define DUMP_AUDIO_DIR "Audio"
#define DUMP_DSP_DIR "DSP"
#define DUMP_SSL_DIR "SSL"
#define LOGS_DIR "Logs"
#define MAIL_LOGS_DIR "Mail"
#define SHADERS_DIR "Shaders"
#define WII_SYSCONF_DIR "shared2" DIR_SEP "sys"
#define WII_WC24CONF_DIR "shared2" DIR_SEP "wc24"
#define RESOURCES_DIR "Resources"
#define THEMES_DIR "Themes"
#define STYLES_DIR "Styles"
#define ANAGLYPH_DIR "Anaglyph"
#define PASSIVE_DIR "Passive"
#define PIPES_DIR "Pipes"
#define MEMORYWATCHER_DIR "MemoryWatcher"
#define WFSROOT_DIR "WFS"
#define BACKUP_DIR "Backup"
#define RESOURCEPACK_DIR "ResourcePacks"
#define DYNAMICINPUT_DIR "DynamicInputTextures"

// This one is only used to remove it if it was present
#define SHADERCACHE_LEGACY_DIR "ShaderCache"

// The theme directory used by default
#define DEFAULT_THEME_DIR "Clean"

// Filenames
// Files in the directory returned by GetUserPath(D_CONFIG_IDX)
#define DOLPHIN_CONFIG "Dolphin.ini"
#define GCPAD_CONFIG "GCPadNew.ini"
#define WIIPAD_CONFIG "WiimoteNew.ini"
#define GCKEYBOARD_CONFIG "GCKeyNew.ini"
#define GFX_CONFIG "GFX.ini"
#define DEBUGGER_CONFIG "Debugger.ini"
#define LOGGER_CONFIG "Logger.ini"
#define DUALSHOCKUDPCLIENT_CONFIG "DSUClient.ini"

// Files in the directory returned by GetUserPath(D_LOGS_IDX)
#define MAIN_LOG "dolphin.log"

// Files in the directory returned by GetUserPath(D_WIISYSCONF_IDX)
#define WII_SYSCONF "SYSCONF"

// Files in the directory returned by GetUserPath(D_DUMP_IDX)
#define MEM1_DUMP "mem1.raw"
#define MEM2_DUMP "mem2.raw"
#define ARAM_DUMP "aram.raw"
#define FAKEVMEM_DUMP "fakevmem.raw"

// Files in the directory returned by GetUserPath(D_MEMORYWATCHER_IDX)
#define MEMORYWATCHER_LOCATIONS "Locations.txt"
#define MEMORYWATCHER_SOCKET "MemoryWatcher"

// Sys files
#define TOTALDB "totaldb.dsy"

#define FONT_WINDOWS_1252 "font_western.bin"
#define FONT_SHIFT_JIS "font_japanese.bin"

#define DSP_IROM "dsp_rom.bin"
#define DSP_COEF "dsp_coef.bin"

#define GC_IPL "IPL.bin"
#define GC_SRAM "SRAM.raw"
#define GC_MEMCARDA "MemoryCardA"
#define GC_MEMCARDB "MemoryCardB"
#define GC_MEMCARD_NETPLAY "NetPlayTemp"

#define WII_STATE "state.dat"

#define WII_SDCARD "sd.raw"
#define WII_BTDINF_BACKUP "btdinf.bak"

#define WII_SETTING "setting.txt"

#define GECKO_CODE_HANDLER "codehandler.bin"

// Subdirs in Sys
#define GC_SYS_DIR "GC"
#define WII_SYS_DIR "Wii"
