// gopro-gui — window with live preview, shooting controls, every camera
// setting, and the media on the SD card.
//
// Threads: the UI thread draws; Worker talks to the camera; VideoStream
// decodes the preview. The UI thread never waits for the camera.

#include "platform.hpp"
#include "video_stream.hpp"
#include "worker.hpp"

#include <SDL.h>
#include <SDL_opengl.h>

#include "imgui.h"
#include "imgui_impl_opengl3.h"
#include "imgui_impl_sdl2.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

// Status ids (names come from the camera's schema; see `gopro status --all`).
constexpr int kBatteryPercent = 70;
constexpr int kBusy = 8;
constexpr int kEncoding = 10;
constexpr int kVideoProgress = 13;
constexpr int kSdStatus = 33;
constexpr int kRemainingPhotos = 34;
constexpr int kRemainingVideoSeconds = 35;
constexpr int kMode = 43;
constexpr int kSubMode = 44;
constexpr int kRemainingSpaceKb = 54;

constexpr long kVideoMode = 0;
constexpr long kPhotoMode = 1;

const ImVec4 kRed(0.80f, 0.16f, 0.16f, 1.0f);
const ImVec4 kRedHover(0.92f, 0.24f, 0.24f, 1.0f);
const ImVec4 kAccent(0.16f, 0.47f, 0.80f, 1.0f);
const ImVec4 kAccentHover(0.24f, 0.56f, 0.90f, 1.0f);
const ImVec4 kMuted(0.62f, 0.64f, 0.68f, 1.0f);
const ImVec4 kWarning(0.95f, 0.70f, 0.25f, 1.0f);

struct Options {
    gopro::Endpoint endpoint;
    float scale = 1.0f;
    std::string screenshotPath;  // testing aid: save the window to a PPM and exit
    double screenshotAfter = 4.0;
    std::string initialTab;
};

struct Ui {
    GLuint videoTexture = 0;
    int videoWidth = 0, videoHeight = 0;
    std::vector<std::uint8_t> frame;

    std::string downloadDir;
    bool mediaRequested = false;
    gopro::MediaFile pendingDelete;
    bool openDeletePopup = false;
    char filterBuffer[64] = "";
};

std::string humanSize(double bytes) {
    const char* units[] = {"B", "kB", "MB", "GB", "TB"};
    int unit = 0;
    while (bytes >= 1024.0 && unit < 4) {
        bytes /= 1024.0;
        ++unit;
    }
    char text[32];
    std::snprintf(text, sizeof text, unit == 0 ? "%.0f %s" : "%.1f %s", bytes, units[unit]);
    return text;
}

std::string clockText(long seconds) {
    char text[32];
    if (seconds >= 3600) {
        std::snprintf(text, sizeof text, "%ld:%02ld:%02ld", seconds / 3600, seconds % 3600 / 60,
                      seconds % 60);
    } else {
        std::snprintf(text, sizeof text, "%02ld:%02ld", seconds / 60, seconds % 60);
    }
    return text;
}

std::string dateText(std::time_t when) {
    const std::tm local = gopro::platform::localTime(when);
    char text[32];
    std::strftime(text, sizeof text, "%Y-%m-%d %H:%M", &local);
    return text;
}

std::string lower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

const gopro::Setting* findSetting(const Snapshot& snap, const std::string& mode,
                                  const std::string& segment) {
    for (const gopro::Setting& setting : snap.settings) {
        if (setting.mode == mode && setting.segment == segment) return &setting;
    }
    return nullptr;
}

// --- video ------------------------------------------------------------------

void uploadFrame(Ui& ui, VideoStream& video) {
    int width = 0, height = 0;
    if (!video.takeFrame(ui.frame, width, height)) return;
    if (ui.videoTexture == 0) {
        glGenTextures(1, &ui.videoTexture);
        glBindTexture(GL_TEXTURE_2D, ui.videoTexture);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }
    glBindTexture(GL_TEXTURE_2D, ui.videoTexture);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    if (width != ui.videoWidth || height != ui.videoHeight) {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                     ui.frame.data());
        ui.videoWidth = width;
        ui.videoHeight = height;
    } else {
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE,
                        ui.frame.data());
    }
}

void drawPreview(Ui& ui, const Snapshot& snap, VideoStream& video, const ImVec2& area) {
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(origin, ImVec2(origin.x + area.x, origin.y + area.y),
                        IM_COL32(12, 12, 14, 255), 6.0f);

    const bool live = ui.videoTexture != 0 && video.secondsSinceFrame() < 3.0;
    if (ui.videoTexture != 0) {
        // Fit the picture inside the area, keeping its aspect ratio.
        const float aspect = static_cast<float>(ui.videoWidth) / static_cast<float>(ui.videoHeight);
        float width = area.x, height = area.x / aspect;
        if (height > area.y) {
            height = area.y;
            width = area.y * aspect;
        }
        const ImVec2 topLeft(origin.x + (area.x - width) / 2, origin.y + (area.y - height) / 2);
        const ImU32 tint = live ? IM_COL32_WHITE : IM_COL32(255, 255, 255, 90);
        draw->AddImage(static_cast<ImTextureID>(static_cast<intptr_t>(ui.videoTexture)), topLeft,
                       ImVec2(topLeft.x + width, topLeft.y + height), ImVec2(0, 0), ImVec2(1, 1),
                       tint);
    }
    if (!live) {
        std::string text = "Waiting for the preview stream...";
        if (!snap.connected) text = "Camera not connected";
        else if (!video.error().empty()) text = video.error();
        else if (snap.state.statusInt(kEncoding).value_or(0) != 0) text = "No preview while recording";
        const ImVec2 size = ImGui::CalcTextSize(text.c_str());
        draw->AddText(ImVec2(origin.x + (area.x - size.x) / 2, origin.y + (area.y - size.y) / 2),
                      IM_COL32(190, 190, 195, 255), text.c_str());
    }
    if (snap.state.statusInt(kEncoding).value_or(0) != 0) {
        const std::string rec = "REC " + clockText(snap.state.statusInt(kVideoProgress).value_or(0));
        const ImVec2 size = ImGui::CalcTextSize(rec.c_str());
        const ImVec2 pos(origin.x + 14, origin.y + 12);
        draw->AddRectFilled(ImVec2(pos.x - 8, pos.y - 5), ImVec2(pos.x + size.x + 8, pos.y + size.y + 5),
                            IM_COL32(200, 36, 36, 235), 4.0f);
        draw->AddText(pos, IM_COL32_WHITE, rec.c_str());
    }
    ImGui::Dummy(area);
}

// --- controls ---------------------------------------------------------------

void drawControls(const Snapshot& snap, Worker& worker, float scale) {
    const long mode = snap.state.statusInt(kMode).value_or(-1);
    const long subMode = snap.state.statusInt(kSubMode).value_or(-1);
    const bool encoding = snap.state.statusInt(kEncoding).value_or(0) != 0;
    const bool busy = snap.state.statusInt(kBusy).value_or(0) != 0;
    const ImVec2 bigButton(190 * scale, 46 * scale);

    ImGui::BeginDisabled(!snap.connected);

    // Shutter: one button whose meaning follows the camera's state.
    if (encoding) {
        ImGui::PushStyleColor(ImGuiCol_Button, kRed);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, kRedHover);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, kRed);
        if (ImGui::Button("Stop", bigButton)) {
            worker.post("Stopped", [](gopro::Camera& camera) {
                camera.shutter(false);
                camera.waitUntilIdle(10000);
            });
        }
        ImGui::PopStyleColor(3);
        ImGui::SameLine();
        if (ImGui::Button("HiLight tag", ImVec2(0, bigButton.y))) {
            worker.post("HiLight tag added", [](gopro::Camera& camera) { camera.tagMoment(); });
        }
    } else {
        const char* label = mode == kVideoMode ? "Record" : mode == kPhotoMode ? "Take photo" : "Start";
        ImGui::BeginDisabled(busy);
        ImGui::PushStyleColor(ImGuiCol_Button, mode == kVideoMode ? kRed : kAccent);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, mode == kVideoMode ? kRedHover : kAccentHover);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, mode == kVideoMode ? kRed : kAccent);
        if (ImGui::Button(label, bigButton)) {
            worker.post(mode == kVideoMode ? "Recording" : "Shutter pressed",
                        [](gopro::Camera& camera) { camera.shutter(true); });
        }
        ImGui::PopStyleColor(3);
        ImGui::EndDisabled();
    }

    // Mode buttons and the sub-mode list.
    ImGui::SameLine(0, 28 * scale);
    ImGui::BeginGroup();
    ImGui::BeginDisabled(encoding);
    for (const gopro::Mode& entry : snap.modes) {
        if (entry.value < 0 || entry.value > 2) continue;  // video, photo, multi-shot
        const bool active = entry.value == mode;
        if (active) ImGui::PushStyleColor(ImGuiCol_Button, kAccent);
        if (ImGui::Button(entry.name.c_str(), ImVec2(104 * scale, 0)) && !active) {
            const long target = entry.value;
            worker.post(entry.name + " mode", [target](gopro::Camera& camera) {
                camera.setMode(target);
                camera.waitUntilIdle();
            });
            worker.restartPreview();
        }
        if (active) ImGui::PopStyleColor();
        ImGui::SameLine();
    }
    for (const gopro::Mode& entry : snap.modes) {
        if (entry.value != mode) continue;
        const gopro::Setting* subModes = findSetting(snap, entry.segment, "current_sub_mode");
        if (!subModes) break;
        const gopro::Option* current = subModes->optionByValue(subMode);
        ImGui::SetNextItemWidth(190 * scale);
        if (ImGui::BeginCombo("##submode", current ? current->name.c_str() : "?")) {
            for (const gopro::Option& option : subModes->options) {
                if (ImGui::Selectable(option.name.c_str(), option.value == subMode)) {
                    const long target = option.value;
                    worker.post(option.name, [mode, target](gopro::Camera& camera) {
                        camera.setSubMode(mode, target);
                        camera.waitUntilIdle();
                    });
                    worker.restartPreview();
                }
            }
            ImGui::EndCombo();
        }
    }
    ImGui::EndDisabled();
    ImGui::NewLine();
    ImGui::EndGroup();

    ImGui::EndDisabled();
}

void drawStatusLine(const Snapshot& snap) {
    if (!snap.connected) {
        ImGui::TextColored(kWarning, "Not connected.");
        ImGui::SameLine();
        ImGui::TextColored(kMuted, "Join the camera's Wi-Fi; the app reconnects by itself.");
        return;
    }
    ImGui::TextUnformatted(snap.info.model.c_str());
    ImGui::SameLine(0, 24);
    if (const auto battery = snap.state.statusInt(kBatteryPercent)) {
        ImGui::TextColored(*battery <= 15 ? kWarning : ImGui::GetStyleColorVec4(ImGuiCol_Text),
                           "Battery %ld%%", *battery);
        ImGui::SameLine(0, 24);
    }
    if (snap.state.statusInt(kSdStatus).value_or(0) != 0) {
        ImGui::TextColored(kWarning, "No SD card");
    } else {
        const double freeBytes = 1024.0 * snap.state.statusInt(kRemainingSpaceKb).value_or(0);
        ImGui::Text("SD %s free", humanSize(freeBytes).c_str());
        ImGui::SameLine(0, 24);
        ImGui::TextColored(kMuted, "%s video or %ld photos left",
                           clockText(snap.state.statusInt(kRemainingVideoSeconds).value_or(0)).c_str(),
                           snap.state.statusInt(kRemainingPhotos).value_or(0));
    }
}

// --- settings ---------------------------------------------------------------

void drawSetting(const gopro::Setting& setting, const Snapshot& snap, Worker& worker, float scale) {
    const auto value = snap.state.settingValue(setting.id);
    const gopro::Option* current = value ? setting.optionByValue(*value) : nullptr;
    const std::string preview =
        current ? current->name : value ? "value " + std::to_string(*value) : "-";

    ImGui::PushID(&setting);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(setting.name.c_str());
    ImGui::SameLine(170 * scale);
    ImGui::SetNextItemWidth(-1);
    if (ImGui::BeginCombo("##value", preview.c_str())) {
        for (const gopro::Option& option : setting.options) {
            const bool selected = value && option.value == *value;
            if (ImGui::Selectable(option.name.c_str(), selected) && !selected) {
                const int id = setting.id;
                const long target = option.value;
                const std::string label = setting.name + ": " + option.name;
                worker.post(label, [id, target, label](gopro::Camera& camera) {
                    camera.applySetting(id, target);
                    camera.waitUntilIdle(2000);
                    // The camera refuses combinations it does not support.
                    if (camera.state().settingValue(id).value_or(target - 1) != target) {
                        throw std::runtime_error("camera did not accept " + label);
                    }
                });
            }
        }
        ImGui::EndCombo();
    }
    ImGui::PopID();
}

void drawSettings(Ui& ui, const Snapshot& snap, Worker& worker, float scale) {
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##filter", "Filter settings", ui.filterBuffer, sizeof ui.filterBuffer);
    const std::string filter = lower(ui.filterBuffer);
    const long mode = snap.state.statusInt(kMode).value_or(-1);

    ImGui::BeginChild("settings-list");
    for (const gopro::Mode& entry : snap.modes) {
        // Count what this group would show, so empty groups are skipped.
        std::vector<const gopro::Setting*> shown;
        for (const gopro::Setting& setting : snap.settings) {
            if (setting.mode != entry.segment) continue;
            if (setting.segment == "current_sub_mode") continue;  // has its own control
            if (!filter.empty() && lower(setting.name + " " + setting.key()).find(filter) ==
                                       std::string::npos) continue;
            shown.push_back(&setting);
        }
        if (shown.empty()) continue;

        ImGuiTreeNodeFlags flags = 0;
        if (entry.value == mode || !filter.empty()) flags |= ImGuiTreeNodeFlags_DefaultOpen;
        // Re-evaluate "open by default" when the camera mode changes.
        ImGui::PushID(static_cast<int>(entry.value == mode));
        if (ImGui::CollapsingHeader(entry.name.c_str(), flags)) {
            for (const gopro::Setting* setting : shown) drawSetting(*setting, snap, worker, scale);
            ImGui::Spacing();
        }
        ImGui::PopID();
    }
    ImGui::EndChild();
}

// --- media ------------------------------------------------------------------

void drawMedia(Ui& ui, const Snapshot& snap, Worker& worker, float scale) {
    if (!ui.mediaRequested && snap.connected) {
        worker.refreshMedia();
        ui.mediaRequested = true;
    }
    if (ImGui::Button(snap.mediaLoading ? "Loading..." : "Refresh")) worker.refreshMedia();
    ImGui::SameLine();
    ImGui::TextColored(kMuted, "%zu files, saved to %s", snap.media.size(), ui.downloadDir.c_str());

    if (snap.downloading) {
        const float fraction = snap.downloadTotal == 0 ? 0.0f
            : static_cast<float>(static_cast<double>(snap.downloadNow) /
                                 static_cast<double>(snap.downloadTotal));
        const std::string label = snap.downloadName + "  " +
                                  humanSize(static_cast<double>(snap.downloadNow)) + " / " +
                                  humanSize(static_cast<double>(snap.downloadTotal));
        ImGui::ProgressBar(fraction, ImVec2(-1, 0), label.c_str());
    }

    const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
                                  ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingStretchProp;
    if (ImGui::BeginTable("media", 4, flags)) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("File", ImGuiTableColumnFlags_WidthStretch, 3.0f);
        ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthFixed, 74 * scale);
        ImGui::TableSetupColumn("Date", ImGuiTableColumnFlags_WidthFixed, 118 * scale);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 132 * scale);
        ImGui::TableHeadersRow();
        // Newest first: that is almost always the file you want.
        for (auto it = snap.media.rbegin(); it != snap.media.rend(); ++it) {
            const gopro::MediaFile& file = *it;
            ImGui::TableNextRow();
            ImGui::PushID(file.path().c_str());
            ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(file.name.c_str());
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(humanSize(static_cast<double>(file.size)).c_str());
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(dateText(file.modified).c_str());
            ImGui::TableNextColumn();
            ImGui::BeginDisabled(snap.downloading);
            if (ImGui::SmallButton("Save")) worker.download(file, ui.downloadDir);
            ImGui::EndDisabled();
            ImGui::SameLine();
            if (ImGui::SmallButton("Delete")) {
                ui.pendingDelete = file;
                ui.openDeletePopup = true;
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }

    if (ui.openDeletePopup) {
        ImGui::OpenPopup("Delete file?");
        ui.openDeletePopup = false;
    }
    const ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal("Delete file?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Delete %s from the SD card?", ui.pendingDelete.path().c_str());
        ImGui::TextColored(kMuted, "This cannot be undone.");
        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_Button, kRed);
        if (ImGui::Button("Delete", ImVec2(120 * scale, 0))) {
            const gopro::MediaFile file = ui.pendingDelete;
            worker.post("Deleted " + file.name,
                        [file](gopro::Camera& camera) { camera.deleteFile(file); });
            worker.refreshMedia();
            ImGui::CloseCurrentPopup();
        }
        ImGui::PopStyleColor();
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(120 * scale, 0))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}

// --- frame ------------------------------------------------------------------

void drawFrame(Ui& ui, const Snapshot& snap, Worker& worker, VideoStream& video,
               const Options& options) {
    const float scale = options.scale;
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);
    ImGui::Begin("main", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus);

    const float sideWidth = 470 * scale;
    const float spacing = ImGui::GetStyle().ItemSpacing.x;
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const float footer = ImGui::GetFrameHeightWithSpacing();

    // Left: status, preview, controls.
    ImGui::BeginChild("left", ImVec2(avail.x - sideWidth - spacing, avail.y - footer));
    drawStatusLine(snap);
    const float controlsHeight = 46 * scale + ImGui::GetStyle().ItemSpacing.y * 3;
    const ImVec2 area(ImGui::GetContentRegionAvail().x,
                      std::max(120.0f, ImGui::GetContentRegionAvail().y - controlsHeight));
    drawPreview(ui, snap, video, area);
    ImGui::Spacing();
    drawControls(snap, worker, scale);
    ImGui::EndChild();

    // Right: settings and media.
    ImGui::SameLine();
    ImGui::BeginChild("right", ImVec2(sideWidth, avail.y - footer));
    if (ImGui::BeginTabBar("tabs")) {
        const bool forceMedia = options.initialTab == "media" && ImGui::GetFrameCount() < 3;
        if (ImGui::BeginTabItem("Settings")) {
            if (snap.haveSchema) drawSettings(ui, snap, worker, scale);
            else ImGui::TextColored(kMuted, "Settings appear once the camera is connected.");
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Media", nullptr, forceMedia ? ImGuiTabItemFlags_SetSelected : 0)) {
            drawMedia(ui, snap, worker, scale);
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::EndChild();

    // Footer: result of the last action, or why the camera is unreachable.
    ImGui::Separator();
    if (!snap.connected && !snap.error.empty()) {
        ImGui::TextColored(kWarning, "%s", snap.error.c_str());
    } else {
        const bool failed = snap.message.rfind("Failed", 0) == 0 ||
                            snap.message.rfind("Download failed", 0) == 0;
        ImGui::TextColored(failed ? kWarning : kMuted, "%s", snap.message.c_str());
    }
    ImGui::End();
}

void applyStyle(float scale) {
    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 0;
    style.FrameRounding = 5;
    style.GrabRounding = 5;
    style.TabRounding = 5;
    style.PopupRounding = 5;
    style.FramePadding = ImVec2(9, 6);
    style.ItemSpacing = ImVec2(9, 8);
    style.WindowPadding = ImVec2(12, 12);
    style.Colors[ImGuiCol_WindowBg] = ImVec4(0.09f, 0.09f, 0.10f, 1.0f);
    style.Colors[ImGuiCol_Header] = ImVec4(0.17f, 0.18f, 0.20f, 1.0f);
    style.ScaleAllSizes(scale);

    // Use a system font with Cyrillic glyphs (file names and folders may need
    // them); fall back to ImGui's built-in font if none of these exist.
    ImGuiIO& io = ImGui::GetIO();
    for (const char* path : {
#if defined(_WIN32)
             "C:\\Windows\\Fonts\\segoeui.ttf", "C:\\Windows\\Fonts\\arial.ttf",
#elif defined(__APPLE__)
             "/System/Library/Fonts/Supplemental/Arial.ttf", "/Library/Fonts/Arial.ttf",
             "/System/Library/Fonts/Supplemental/Tahoma.ttf",
#else
             "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", "/usr/share/fonts/TTF/DejaVuSans.ttf",
             "/usr/share/fonts/dejavu-sans-fonts/DejaVuSans.ttf",
             "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
#endif
         }) {
        std::error_code error;
        if (std::filesystem::exists(path, error)) {
            io.Fonts->AddFontFromFileTTF(path, 16.0f * scale, nullptr,
                                         io.Fonts->GetGlyphRangesCyrillic());
            return;
        }
    }
    io.FontGlobalScale = scale;
}

void saveScreenshot(const std::string& path, int width, int height) {
    std::vector<unsigned char> pixels(static_cast<std::size_t>(width) * height * 3);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, width, height, GL_RGB, GL_UNSIGNED_BYTE, pixels.data());
    std::ofstream out(path, std::ios::binary);
    out << "P6\n" << width << " " << height << "\n255\n";
    for (int row = height - 1; row >= 0; --row) {  // OpenGL rows are bottom-up
        out.write(reinterpret_cast<const char*>(pixels.data()) + static_cast<std::size_t>(row) * width * 3,
                  static_cast<std::streamsize>(width) * 3);
    }
}

Options parseOptions(int argc, char** argv) {
    Options options;
    if (const char* host = std::getenv("GOPRO_HOST"); host && *host) options.endpoint.host = host;
    if (const char* port = std::getenv("GOPRO_PORT"); port && *port) options.endpoint.port = std::atoi(port);
    if (const char* port = std::getenv("GOPRO_MEDIA_PORT"); port && *port) {
        options.endpoint.mediaPort = std::atoi(port);
    }
    if (const char* scale = std::getenv("GOPRO_GUI_SCALE"); scale && *scale) {
        options.scale = static_cast<float>(std::atof(scale));
    }
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        const bool hasValue = i + 1 < argc;
        if (arg == "--host" && hasValue) options.endpoint.host = argv[++i];
        else if (arg == "--port" && hasValue) options.endpoint.port = std::atoi(argv[++i]);
        else if (arg == "--media-port" && hasValue) options.endpoint.mediaPort = std::atoi(argv[++i]);
        else if (arg == "--scale" && hasValue) options.scale = static_cast<float>(std::atof(argv[++i]));
        else if (arg == "--screenshot" && hasValue) options.screenshotPath = argv[++i];
        else if (arg == "--after" && hasValue) options.screenshotAfter = std::atof(argv[++i]);
        else if (arg == "--tab" && hasValue) options.initialTab = argv[++i];
        else {
            std::printf("Usage: gopro-gui [--host ip] [--port n] [--media-port n] [--scale 1.5]\n"
                        "Live preview, shooting controls, settings and media of a GoPro over Wi-Fi.\n");
            std::exit(arg == "-h" || arg == "--help" ? 0 : 2);
        }
    }
    options.scale = std::clamp(options.scale, 0.75f, 4.0f);
    return options;
}

}  // namespace

int main(int argc, char** argv) {
    const Options options = parseOptions(argc, argv);

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) != 0) {
        std::fprintf(stderr, "gopro-gui: cannot start SDL: %s\n", SDL_GetError());
        return 1;
    }
    // Same choices as the Dear ImGui SDL2 example: macOS only offers a
    // forward-compatible core profile; elsewhere OpenGL 3.0 is enough.
#ifdef __APPLE__
    const char* glslVersion = "#version 150";
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 2);
#else
    const char* glslVersion = "#version 130";
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, 0);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
#endif
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_Window* window = SDL_CreateWindow(
        "GoPro", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        static_cast<int>(1280 * options.scale), static_cast<int>(720 * options.scale),
        SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
    SDL_GLContext gl = window ? SDL_GL_CreateContext(window) : nullptr;
    if (!window || !gl) {
        std::fprintf(stderr, "gopro-gui: cannot create an OpenGL 3 window: %s\n", SDL_GetError());
        return 1;
    }
    SDL_GL_MakeCurrent(window, gl);
    SDL_GL_SetSwapInterval(1);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;  // the layout is fixed; nothing to remember
    applyStyle(options.scale);
    ImGui_ImplSDL2_InitForOpenGL(window, gl);
    ImGui_ImplOpenGL3_Init(glslVersion);

    Ui ui;
    ui.downloadDir = (std::filesystem::path(gopro::platform::homeDirectory()) / "GoPro").string();

    {
        // Listen for video before asking the camera to send it.
        VideoStream video(gopro::Camera::kStreamPort);
        video.start();
        Worker worker(options.endpoint);

        const Uint64 started = SDL_GetTicks64();
        bool running = true;
        while (running) {
            SDL_Event event;
            while (SDL_PollEvent(&event)) {
                ImGui_ImplSDL2_ProcessEvent(&event);
                if (event.type == SDL_QUIT) running = false;
                if (event.type == SDL_WINDOWEVENT && event.window.event == SDL_WINDOWEVENT_CLOSE &&
                    event.window.windowID == SDL_GetWindowID(window)) {
                    running = false;
                }
            }

            uploadFrame(ui, video);
            const Snapshot snap = worker.snapshot();

            ImGui_ImplOpenGL3_NewFrame();
            ImGui_ImplSDL2_NewFrame();
            ImGui::NewFrame();
            drawFrame(ui, snap, worker, video, options);
            ImGui::Render();

            int width = 0, height = 0;
            SDL_GL_GetDrawableSize(window, &width, &height);
            glViewport(0, 0, width, height);
            glClearColor(0.09f, 0.09f, 0.10f, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT);
            ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

            if (!options.screenshotPath.empty() &&
                static_cast<double>(SDL_GetTicks64() - started) / 1000.0 >= options.screenshotAfter) {
                saveScreenshot(options.screenshotPath, width, height);
                running = false;
            }
            SDL_GL_SwapWindow(window);
        }
    }  // worker and video threads stop here, before the GL context goes away

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();
    SDL_GL_DeleteContext(gl);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
