// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <iostream>
#include <memory>
#include <regex>
#include <string>
#include <thread>

// This needs to be included before getopt.h because the latter #defines symbols used by it
#include "common/microprofile.h"

#include "citra_sdl/config.h"
#include "citra_sdl/emu_window/emu_window_sdl2.h"
#ifdef ENABLE_OPENGL
#include "citra_sdl/emu_window/emu_window_sdl2_gl.h"
#endif
#ifdef ENABLE_SOFTWARE_RENDERER
#include "citra_sdl/emu_window/emu_window_sdl2_sw.h"
#endif
#ifdef ENABLE_VULKAN
#include "citra_sdl/emu_window/emu_window_sdl2_vk.h"
#endif
#include "SDL_messagebox.h"
#include "citra_meta/common_strings.h"
#include "common/common_paths.h"
#include "common/detached_tasks.h"
#include "common/file_util.h"
#include "common/logging/backend.h"
#include "common/logging/log.h"
#include "common/scm_rev.h"
#include "common/scope_exit.h"
#include "common/settings.h"
#include "common/string_util.h"
#include "core/core.h"
#include "core/dumping/backend.h"
#include "core/dumping/ffmpeg_backend.h"
#include "core/frontend/applets/default_applets.h"
#include "core/frontend/framebuffer_layout.h"
#include "core/hle/service/am/am.h"
#include "core/hle/service/cfg/cfg.h"
#include "core/movie.h"
#include "input_common/main.h"
#include "network/network.h"
#include "video_core/gpu.h"
#include "video_core/renderer_base.h"

#ifdef __unix__
#include "common/linux/gamemode.h"
#endif

#undef _UNICODE
#include <getopt.h>
#ifndef _MSC_VER
#include <unistd.h>
#endif

#ifdef _WIN32
// windows.h needs to be included before shellapi.h
#include <windows.h>

#include <shellapi.h>
#endif

static void ShowCommandOutput(std::string title, std::string message) {
#ifdef _WIN32
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_INFORMATION, title.c_str(), message.c_str(), NULL);
#else
    std::cout << message << std::endl;
#endif
}

static void PrintHelp(const char* argv0) {
    ShowCommandOutput("Help", fmt::format(Common::help_string, argv0));
}

static void OnStateChanged(const Network::RoomMember::State& state) {
    switch (state) {
    case Network::RoomMember::State::Idle:
        LOG_DEBUG(Network, "Network is idle");
        break;
    case Network::RoomMember::State::Joining:
        LOG_DEBUG(Network, "Connection sequence to room started");
        break;
    case Network::RoomMember::State::Joined:
        LOG_DEBUG(Network, "Successfully joined to the room");
        break;
    case Network::RoomMember::State::Moderator:
        LOG_DEBUG(Network, "Successfully joined the room as a moderator");
        break;
    default:
        break;
    }
}

static void OnNetworkError(const Network::RoomMember::Error& error) {
    switch (error) {
    case Network::RoomMember::Error::LostConnection:
        LOG_DEBUG(Network, "Lost connection to the room");
        break;
    case Network::RoomMember::Error::CouldNotConnect:
        LOG_ERROR(Network, "Error: Could not connect");
        std::exit(1);
        break;
    case Network::RoomMember::Error::NameCollision:
        LOG_ERROR(
            Network,
            "You tried to use the same nickname as another user that is connected to the Room");
        std::exit(1);
        break;
    case Network::RoomMember::Error::MacCollision:
        LOG_ERROR(Network, "You tried to use the same MAC-Address as another user that is "
                           "connected to the Room");
        std::exit(1);
        break;
    case Network::RoomMember::Error::ConsoleIdCollision:
        LOG_ERROR(Network, "Your Console ID conflicted with someone else in the Room");
        std::exit(1);
        break;
    case Network::RoomMember::Error::WrongPassword:
        LOG_ERROR(Network, "Room replied with: Wrong password");
        std::exit(1);
        break;
    case Network::RoomMember::Error::WrongVersion:
        LOG_ERROR(Network,
                  "You are using a different version than the room you are trying to connect to");
        std::exit(1);
        break;
    case Network::RoomMember::Error::RoomIsFull:
        LOG_ERROR(Network, "The room is full");
        std::exit(1);
        break;
    case Network::RoomMember::Error::HostKicked:
        LOG_ERROR(Network, "You have been kicked by the host");
        break;
    case Network::RoomMember::Error::HostBanned:
        LOG_ERROR(Network, "You have been banned by the host");
        break;
    default:
        LOG_ERROR(Network, "Unknown network error: {}", error);
        break;
    }
}

static void OnMessageReceived(const Network::ChatEntry& msg) {
    std::cout << std::endl << msg.nickname << ": " << msg.message << std::endl << std::endl;
}

static void OnStatusMessageReceived(const Network::StatusMessageEntry& msg) {
    std::string message;
    switch (msg.type) {
    case Network::IdMemberJoin:
        message = fmt::format("{} has joined", msg.nickname);
        break;
    case Network::IdMemberLeave:
        message = fmt::format("{} has left", msg.nickname);
        break;
    case Network::IdMemberKicked:
        message = fmt::format("{} has been kicked", msg.nickname);
        break;
    case Network::IdMemberBanned:
        message = fmt::format("{} has been banned", msg.nickname);
        break;
    case Network::IdAddressUnbanned:
        message = fmt::format("{} has been unbanned", msg.nickname);
        break;
    }
    if (!message.empty())
        std::cout << std::endl << "* " << message << std::endl << std::endl;
}

/// Application entry point
void LaunchSdlFrontend(int argc, char** argv) {
    Common::Log::Initialize();
    Common::Log::SetColorConsoleBackendEnabled(true);
    Common::Log::Start();
    Common::DetachedTasks detached_tasks;
    SdlConfig config;
    int option_index = 0;
    bool use_gdbstub = Settings::values.use_gdbstub.GetValue();
    u32 gdb_port = static_cast<u32>(Settings::values.gdbstub_port.GetValue());
    std::string movie_record;
    std::string movie_record_author;
    std::string movie_play;
    std::string dump_video;

    char* endarg;
#ifdef _WIN32
    int argc_w;
    auto argv_w = CommandLineToArgvW(GetCommandLineW(), &argc_w);

    if (argv_w == nullptr) {
        LOG_CRITICAL(Frontend, "Failed to get command line arguments");
        exit(-1);
    }
#endif
    std::string filepath;

    bool use_multiplayer = false;
    bool fullscreen = false;
    std::string nickname{};
    std::string password{};
    std::string address{};
    u16 port = Network::DefaultRoomPort;

    static struct option long_options[] = {
        {"dump-video", required_argument, 0, 'd'},
        {"fullscreen", no_argument, 0, 'f'},
        {"gdbport", required_argument, 0, 'g'},
        {"help", no_argument, 0, 'h'},
        {"install", required_argument, 0, 'i'},
        {"movie-play", required_argument, 0, 'p'},
        {"movie-record", required_argument, 0, 'r'},
        {"movie-record-author", required_argument, 0, 'a'},
        {"multiplayer", required_argument, 0, 'm'},
        {"version", no_argument, 0, 'v'},
        {"windowed", no_argument, 0, 'w'},
        {0, 0, 0, 0},
    };

    while (optind < argc) {
        int arg = getopt_long(argc, argv, "d:fg:hi:p:r:a:m:nvw", long_options, &option_index);
        if (arg != -1) {
            switch (static_cast<char>(arg)) {
            case 'd':
                dump_video = optarg;
                break;
            case 'f':
                fullscreen = true;
                LOG_INFO(Frontend, "Starting in fullscreen mode...");
                break;
            case 'g':
                errno = 0;
                gdb_port = strtoul(optarg, &endarg, 0);
                use_gdbstub = true;
                if (endarg == optarg)
                    errno = EINVAL;
                if (errno != 0) {
                    perror("--gdbport");
                    exit(1);
                }
                break;
            case 'h':
                PrintHelp(argv[0]);
                exit(0);
            case 'i': {
                const auto cia_progress = [](std::size_t written, std::size_t total) {
                    LOG_INFO(Frontend, "{:02d}%", (written * 100 / total));
                };
                if (Service::AM::InstallCIA(std::string(optarg), cia_progress) !=
                    Service::AM::InstallStatus::Success)
                    errno = EINVAL;
                if (errno != 0)
                    exit(1);
                break;
            }
            case 'p':
                movie_play = optarg;
                break;
            case 'r':
                movie_record = optarg;
                break;
            case 'a':
                movie_record_author = optarg;
                break;
            case 'm': {
                use_multiplayer = true;
                const std::string str_arg(optarg);
                // regex to check if the format is nickname:password@ip:port
                // with optional :password
                const std::regex re("^([^:]+)(?::(.+))?@([^:]+)(?::([0-9]+))?$");
                if (!std::regex_match(str_arg, re)) {
                    std::cout << "Wrong format for option --multiplayer\n";
                    PrintHelp(argv[0]);
                    exit(0);
                }

                std::smatch match;
                std::regex_search(str_arg, match, re);
                ASSERT(match.size() == 5);
                nickname = match[1];
                password = match[2];
                address = match[3];
                if (!match[4].str().empty())
                    port = std::stoi(match[4]);
                std::regex nickname_re("^[a-zA-Z0-9._\\- ]+$");
                if (!std::regex_match(nickname, nickname_re)) {
                    std::cout
                        << "Nickname is not valid. Must be 4 to 20 alphanumeric characters.\n";
                    exit(0);
                }
                if (address.empty()) {
                    std::cout << "Address to room must not be empty.\n";
                    exit(0);
                }
                break;
            }
            case 'v':
                const std::string version_string =
                    std::string("Azahar ") + Common::g_build_fullname;
                ShowCommandOutput("Version", version_string);
                exit(0);
            }
        } else {
#ifdef _WIN32
            filepath = Common::UTF16ToUTF8(argv_w[optind]);
#else
            filepath = argv[optind];
#endif
            optind++;
        }
    }

#ifdef _WIN32
    LocalFree(argv_w);
#endif

    MicroProfileOnThreadCreate("EmuThread");
    SCOPE_EXIT({ MicroProfileShutdown(); });

    if (filepath.empty()) {
        LOG_CRITICAL(Frontend, "Failed to load ROM: No ROM specified");
        exit(-1);
    }

    if (!movie_record.empty() && !movie_play.empty()) {
        LOG_CRITICAL(Frontend, "Cannot both play and record a movie");
        exit(-1);
    }

    auto& system = Core::System::GetInstance();
    auto& movie = system.Movie();

    if (!movie_record.empty()) {
        movie.PrepareForRecording();
    }
    if (!movie_play.empty()) {
        movie.PrepareForPlayback(movie_play);
    }

    // Apply the command line arguments
    Settings::values.gdbstub_port = gdb_port;
    Settings::values.use_gdbstub = use_gdbstub;
    system.ApplySettings();

    // Register frontend applets
    Frontend::RegisterDefaultApplets(system);

    EmuWindow_SDL2::InitializeSDL2();

    const auto create_emu_window = [&](bool fullscreen,
                                       bool is_secondary) -> std::unique_ptr<EmuWindow_SDL2> {
        const auto graphics_api = Settings::values.graphics_api.GetValue();
        switch (graphics_api) {
#ifdef ENABLE_OPENGL
        case Settings::GraphicsAPI::OpenGL:
            return std::make_unique<EmuWindow_SDL2_GL>(system, fullscreen, is_secondary);
#endif
#ifdef ENABLE_VULKAN
        case Settings::GraphicsAPI::Vulkan:
            return std::make_unique<EmuWindow_SDL2_VK>(system, fullscreen, is_secondary);
#endif
#ifdef ENABLE_SOFTWARE_RENDERER
        case Settings::GraphicsAPI::Software:
            return std::make_unique<EmuWindow_SDL2_SW>(system, fullscreen, is_secondary);
#endif
        default:
            LOG_CRITICAL(
                Frontend,
                "Unknown or unsupported graphics API {}, falling back to available default",
                graphics_api);
#ifdef ENABLE_OPENGL
            return std::make_unique<EmuWindow_SDL2_GL>(system, fullscreen, is_secondary);
#elif ENABLE_VULKAN
            return std::make_unique<EmuWindow_SDL2_VK>(system, fullscreen, is_secondary);
#elif ENABLE_SOFTWARE_RENDERER
            return std::make_unique<EmuWindow_SDL2_SW>(system, fullscreen, is_secondary);
#else
            // TODO: Add a null renderer backend for this, perhaps.
#error "At least one renderer must be enabled."
#endif
        }
    };

    const auto emu_window{create_emu_window(fullscreen, false)};
    const bool use_secondary_window{
        Settings::values.layout_option.GetValue() == Settings::LayoutOption::SeparateWindows &&
        Settings::values.graphics_api.GetValue() != Settings::GraphicsAPI::Software};
    const auto secondary_window = use_secondary_window ? create_emu_window(false, true) : nullptr;

    const auto scope = emu_window->Acquire();

    LOG_INFO(Frontend, "Azahar Version: {} | {}-{}", Common::g_build_fullname, Common::g_scm_branch,
             Common::g_scm_desc);
    Settings::LogSettings();

    const Core::System::ResultStatus load_result{
        system.Load(*emu_window, filepath, secondary_window.get())};

    switch (load_result) {
    case Core::System::ResultStatus::ErrorGetLoader:
        LOG_CRITICAL(Frontend, "Failed to obtain loader for {}!", filepath);
        exit(-1);
    case Core::System::ResultStatus::ErrorLoader:
        LOG_CRITICAL(Frontend, "Failed to load ROM!");
        exit(-1);
    case Core::System::ResultStatus::ErrorLoader_ErrorEncrypted:
        LOG_CRITICAL(Frontend,
                     "The application that you are trying to load must be decrypted before "
                     "being used with Azahar. \n\n For more information on dumping and "
                     "decrypting applications, please refer to: "
                     "https://web.archive.org/web/20240304210021/https://citra-emu.org/"
                     "wiki/dumping-game-cartridges/");
        exit(-1);
    case Core::System::ResultStatus::ErrorLoader_ErrorInvalidFormat:
        LOG_CRITICAL(Frontend, "Error while loading ROM: The ROM format is not supported.");
        exit(-1);
    case Core::System::ResultStatus::ErrorNotInitialized:
        LOG_CRITICAL(Frontend, "CPUCore not initialized");
        exit(-1);
    case Core::System::ResultStatus::ErrorSystemMode:
        LOG_CRITICAL(Frontend, "Failed to determine system mode!");
        exit(-1);
    case Core::System::ResultStatus::Success:
        break; // Expected case
    default:
        LOG_ERROR(Frontend, "Error while loading ROM: {}", system.GetStatusDetails());
        break;
    }

    if (use_multiplayer) {
        if (auto member = Network::GetRoomMember().lock()) {
            member->BindOnChatMessageRecieved(OnMessageReceived);
            member->BindOnStatusMessageReceived(OnStatusMessageReceived);
            member->BindOnStateChanged(OnStateChanged);
            member->BindOnError(OnNetworkError);
            LOG_DEBUG(Network, "Start connection to {}:{} with nickname {}", address, port,
                      nickname);
            member->Join(nickname, Service::CFG::GetConsoleIdHash(system), address.c_str(), port, 0,
                         Network::NoPreferredMac, password);
        } else {
            LOG_ERROR(Network, "Could not access RoomMember");
            exit(0);
        }
    }

    if (!movie_play.empty()) {
        auto metadata = movie.GetMovieMetadata(movie_play);
        LOG_INFO(Movie, "Author: {}", metadata.author);
        LOG_INFO(Movie, "Rerecord count: {}", metadata.rerecord_count);
        LOG_INFO(Movie, "Input count: {}", metadata.input_count);
        movie.StartPlayback(movie_play);
    }
    if (!movie_record.empty()) {
        movie.StartRecording(movie_record, movie_record_author);
    }
    if (!dump_video.empty() && DynamicLibrary::FFmpeg::LoadFFmpeg()) {
        auto& renderer = system.GPU().Renderer();
        const auto layout{
            Layout::FrameLayoutFromResolutionScale(renderer.GetResolutionScaleFactor())};
        auto dumper = std::make_shared<VideoDumper::FFmpegBackend>(renderer);
        if (dumper->StartDumping(dump_video, layout)) {
            system.RegisterVideoDumper(dumper);
        }
    }

#ifdef __unix__
    Common::Linux::StartGamemode();
#endif

    std::thread main_render_thread([&emu_window] { emu_window->Present(); });
    std::thread secondary_render_thread([&secondary_window] {
        if (secondary_window) {
            secondary_window->Present();
        }
    });

    std::atomic_bool stop_run;
    system.GPU().Renderer().Rasterizer()->LoadDefaultDiskResources(
        stop_run, [](VideoCore::LoadCallbackStage stage, std::size_t value, std::size_t total) {
            LOG_DEBUG(Frontend, "Loading stage {} progress {} {}", static_cast<u32>(stage), value,
                      total);
        });

    const auto secondary_is_open = [&secondary_window] {
        // if the secondary window isn't created, it shouldn't affect the main loop
        return secondary_window ? secondary_window->IsOpen() : true;
    };
    while (emu_window->IsOpen() && secondary_is_open()) {
        const auto result = system.RunLoop();

        switch (result) {
        case Core::System::ResultStatus::ShutdownRequested:
            emu_window->RequestClose();
            break;
        case Core::System::ResultStatus::Success:
            break;
        default:
            LOG_ERROR(Frontend, "Error in main run loop: {}", result, system.GetStatusDetails());
            break;
        }
    }
    emu_window->RequestClose();
    if (secondary_window) {
        secondary_window->RequestClose();
    }
    main_render_thread.join();
    secondary_render_thread.join();

    movie.Shutdown();

    auto video_dumper = system.GetVideoDumper();
    if (video_dumper && video_dumper->IsDumping()) {
        video_dumper->StopDumping();
    }

    Network::Shutdown();
    InputCommon::Shutdown();

    system.Shutdown();

#ifdef __unix__
    Common::Linux::StopGamemode();
#endif

    detached_tasks.WaitForAllTasks();
    exit(0);
}
