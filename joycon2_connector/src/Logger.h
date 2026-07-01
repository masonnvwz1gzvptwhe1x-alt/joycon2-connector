#pragma once
// Logger.h — 线程安全的运行时日志系统
// 日志级别：INFO / WARN / ERROR / DEBUG / MOTION
// 支持弹窗查看、一键导出到桌面

#include <string>
#include <vector>
#include <mutex>
#include <chrono>
#include <sstream>
#include <iomanip>
#include <fstream>
#include <atomic>
#include <Windows.h>
#include <ShlObj.h>    // SHGetFolderPath
#include "imgui/imgui.h"
#include "UI_Theme.h"

// ─── 日志级别 ────────────────────────────────────────────────────────────────
enum class LogLevel {
    INFO,   // 常规状态信息（连接、断开、初始化等）
    WARN,   // 警告（特征值缺失、重试等）
    ERR,    // 错误（连接失败、命令发送失败等）— 注意：不能叫 ERROR，
            // <Windows.h>(wingdi.h) 会把 ERROR 宏替换成 0，导致编译错误
    DEBUG,  // 调试细节（GATT UUID、命令字节、报告包头等）
    MOTION, // 体感数据采样（高频，默认折叠）
};

// ─── 单条日志条目 ─────────────────────────────────────────────────────────────
struct LogEntry {
    LogLevel    level;
    std::string timestamp;  // HH:MM:SS.mmm
    std::string category;   // "BLE" / "IMU" / "DSU" / "UI" / "VIGEM"
    std::string message;
};

// ─── Logger 单例 ──────────────────────────────────────────────────────────────
class Logger {
public:
    static Logger& Instance() {
        static Logger inst;
        return inst;
    }

    // 最大保留条数（超过后丢弃最旧的）
    static constexpr size_t MAX_ENTRIES = 2000;

    void Log(LogLevel level, const char* category, const char* fmt, ...) {
        // 格式化消息
        char buf[1024];
        va_list args;
        va_start(args, fmt);
        vsnprintf(buf, sizeof(buf), fmt, args);
        va_end(args);

        LogEntry entry;
        entry.level     = level;
        entry.timestamp = GetTimestamp();
        entry.category  = category;
        entry.message   = buf;

        std::lock_guard<std::mutex> lock(mutex_);
        entries_.push_back(std::move(entry));
        if (entries_.size() > MAX_ENTRIES)
            entries_.erase(entries_.begin(), entries_.begin() + (MAX_ENTRIES / 4));

        scrollToBottom_.store(true);
    }

    // 快捷宏风格的函数
    void Info  (const char* cat, const char* fmt, ...) { va_list a; va_start(a,fmt); LogV(LogLevel::INFO,   cat,fmt,a); va_end(a); }
    void Warn  (const char* cat, const char* fmt, ...) { va_list a; va_start(a,fmt); LogV(LogLevel::WARN,   cat,fmt,a); va_end(a); }
    void Error (const char* cat, const char* fmt, ...) { va_list a; va_start(a,fmt); LogV(LogLevel::ERR,  cat,fmt,a); va_end(a); }
    void Debug (const char* cat, const char* fmt, ...) { va_list a; va_start(a,fmt); LogV(LogLevel::DEBUG,  cat,fmt,a); va_end(a); }
    void Motion(const char* cat, const char* fmt, ...) { va_list a; va_start(a,fmt); LogV(LogLevel::MOTION, cat,fmt,a); va_end(a); }

    // 渲染日志弹窗（在主循环里调用）
    void RenderWindow(bool* p_open) {
        if (!p_open || !*p_open) return;

        float s = UITheme::DpiScale;
        ImGui::SetNextWindowSize(ImVec2(820 * s, 520 * s), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowPos(
            ImVec2(ImGui::GetIO().DisplaySize.x * 0.5f, ImGui::GetIO().DisplaySize.y * 0.5f),
            ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.5f));

        ImGuiWindowFlags flags =
            ImGuiWindowFlags_NoCollapse |
            ImGuiWindowFlags_NoSavedSettings;

        ImGui::PushStyleColor(ImGuiCol_WindowBg, UITheme::SurfaceCard);
        ImGui::PushStyleColor(ImGuiCol_TitleBg,       UITheme::Sidebar);
        ImGui::PushStyleColor(ImGuiCol_TitleBgActive,  UITheme::Sidebar);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16*s, 12*s));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 12*s);

        if (!ImGui::Begin("JoyCon2 Connector — 运行日志", p_open, flags)) {
            ImGui::End();
            ImGui::PopStyleVar(2);
            ImGui::PopStyleColor(3);
            return;
        }

        // ── 顶部工具栏 ───────────────────────────────────────────────────────
        // 过滤器复选框
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(6*s, 4*s));

        ImGui::Checkbox("INFO",   &showInfo_);   ImGui::SameLine();
        ImGui::Checkbox("WARN",   &showWarn_);   ImGui::SameLine();
        ImGui::Checkbox("ERROR",  &showError_);  ImGui::SameLine();
        ImGui::Checkbox("DEBUG",  &showDebug_);  ImGui::SameLine();
        ImGui::Checkbox("MOTION", &showMotion_);

        ImGui::SameLine(0, 20*s);

        // 搜索框
        ImGui::SetNextItemWidth(160*s);
        ImGui::InputText("##search", searchBuf_, sizeof(searchBuf_));
        ImGui::SameLine();
        ImGui::TextColored(UITheme::TextTertiary, "搜索");

        ImGui::SameLine(0, 20*s);

        // 清空按钮
        ImGui::PushStyleColor(ImGuiCol_Button,        UITheme::ButtonDanger);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered,  UITheme::ButtonDangerHov);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,   UITheme::ButtonDangerHov);
        ImGui::PushStyleColor(ImGuiCol_Text,           UITheme::Error);
        if (ImGui::Button("清空")) {
            std::lock_guard<std::mutex> lock(mutex_);
            entries_.clear();
        }
        ImGui::PopStyleColor(4);

        ImGui::SameLine();

        // 导出按钮
        if (ImGui::Button("导出日志")) {
            ExportToDesktop();
        }

        ImGui::PopStyleVar(); // FramePadding

        ImGui::Separator();

        // ── 日志列表 ─────────────────────────────────────────────────────────
        ImGui::PushStyleColor(ImGuiCol_ChildBg, UITheme::Surface);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8*s, 6*s));
        ImGui::BeginChild("LogScrollArea",
            ImVec2(0, ImGui::GetContentRegionAvail().y - 28*s),
            ImGuiChildFlags_Borders);

        std::string searchStr(searchBuf_);

        {
            std::lock_guard<std::mutex> lock(mutex_);
            for (const auto& e : entries_) {
                if (!ShouldShow(e, searchStr)) continue;

                // 时间戳 + 分类（灰色）
                ImGui::TextColored(UITheme::TextTertiary, "[%s][%-5s]",
                    e.timestamp.c_str(), e.category.c_str());
                ImGui::SameLine();

                // 级别标记（带颜色）
                ImVec4 color = LevelColor(e.level);
                const char* levelStr = LevelStr(e.level);
                ImGui::TextColored(color, "[%s]", levelStr);
                ImGui::SameLine();

                // 消息正文
                ImGui::TextUnformatted(e.message.c_str());
            }
        }

        // 自动滚动到底部
        if (scrollToBottom_.exchange(false))
            ImGui::SetScrollHereY(1.0f);

        ImGui::EndChild();
        ImGui::PopStyleVar();
        ImGui::PopStyleColor();

        // ── 底部条目数 ────────────────────────────────────────────────────────
        {
            std::lock_guard<std::mutex> lock(mutex_);
            ImGui::TextColored(UITheme::TextTertiary,
                "共 %zu 条记录（最多保留 %zu 条）", entries_.size(), MAX_ENTRIES);
        }

        ImGui::End();
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor(3);
    }

private:
    Logger() = default;

    std::vector<LogEntry> entries_;
    std::mutex            mutex_;
    std::atomic<bool>     scrollToBottom_{ false };

    // 过滤状态
    bool showInfo_   = true;
    bool showWarn_   = true;
    bool showError_  = true;
    bool showDebug_  = false;   // 默认不展示 DEBUG，避免刷屏
    bool showMotion_ = false;   // 默认不展示 MOTION，避免刷屏
    char searchBuf_[128]  = {};

    void LogV(LogLevel level, const char* category, const char* fmt, va_list args) {
        char buf[1024];
        vsnprintf(buf, sizeof(buf), fmt, args);
        Log(level, category, "%s", buf);
    }

    static std::string GetTimestamp() {
        auto now = std::chrono::system_clock::now();
        auto ms  = std::chrono::duration_cast<std::chrono::milliseconds>(
                       now.time_since_epoch()) % 1000;
        auto t   = std::chrono::system_clock::to_time_t(now);
        struct tm tm_info{};
        localtime_s(&tm_info, &t);
        char buf[32];
        snprintf(buf, sizeof(buf), "%02d:%02d:%02d.%03d",
            tm_info.tm_hour, tm_info.tm_min, tm_info.tm_sec,
            (int)ms.count());
        return buf;
    }

    bool ShouldShow(const LogEntry& e, const std::string& search) const {
        switch (e.level) {
            case LogLevel::INFO:   if (!showInfo_)   return false; break;
            case LogLevel::WARN:   if (!showWarn_)   return false; break;
            case LogLevel::ERR:  if (!showError_)  return false; break;
            case LogLevel::DEBUG:  if (!showDebug_)  return false; break;
            case LogLevel::MOTION: if (!showMotion_) return false; break;
        }
        if (!search.empty()) {
            bool inMsg = e.message.find(search)  != std::string::npos;
            bool inCat = e.category.find(search) != std::string::npos;
            if (!inMsg && !inCat) return false;
        }
        return true;
    }

    static ImVec4 LevelColor(LogLevel l) {
        switch (l) {
            case LogLevel::INFO:   return UITheme::TextSecondary;
            case LogLevel::WARN:   return UITheme::Warning;
            case LogLevel::ERR:  return UITheme::Error;
            case LogLevel::DEBUG:  return UITheme::TextTertiary;
            case LogLevel::MOTION: return UITheme::PrimaryHover;
        }
        return UITheme::TextSecondary;
    }

    static const char* LevelStr(LogLevel l) {
        switch (l) {
            case LogLevel::INFO:   return "INFO ";
            case LogLevel::WARN:   return "WARN ";
            case LogLevel::ERR:  return "ERROR";
            case LogLevel::DEBUG:  return "DEBUG";
            case LogLevel::MOTION: return "MOTN ";
        }
        return "?????";
    }

    void ExportToDesktop() {
        // 找桌面路径
        char desktopPath[MAX_PATH];
        if (FAILED(SHGetFolderPathA(nullptr, CSIDL_DESKTOP, nullptr, 0, desktopPath)))
            return;

        // 用时间戳命名文件
        auto now = std::chrono::system_clock::now();
        auto t   = std::chrono::system_clock::to_time_t(now);
        struct tm tm_info{};
        localtime_s(&tm_info, &t);
        char filename[MAX_PATH];
        snprintf(filename, sizeof(filename),
            "%s\\joycon2_log_%04d%02d%02d_%02d%02d%02d.txt",
            desktopPath,
            tm_info.tm_year + 1900, tm_info.tm_mon + 1, tm_info.tm_mday,
            tm_info.tm_hour, tm_info.tm_min, tm_info.tm_sec);

        std::ofstream f(filename, std::ios::out | std::ios::trunc);
        if (!f.is_open()) return;

        f << "JoyCon2 Connector — 运行日志导出\n";
        f << "导出时间: ";
        char timebuf[64];
        snprintf(timebuf, sizeof(timebuf), "%04d-%02d-%02d %02d:%02d:%02d",
            tm_info.tm_year + 1900, tm_info.tm_mon + 1, tm_info.tm_mday,
            tm_info.tm_hour, tm_info.tm_min, tm_info.tm_sec);
        f << timebuf << "\n";
        f << std::string(80, '-') << "\n";

        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& e : entries_) {
            const char* lvl = LevelStr(e.level);
            f << "[" << e.timestamp << "][" << e.category << "][" << lvl << "] "
              << e.message << "\n";
        }

        f.close();

        // 用资源管理器打开桌面（让用户看到文件）
        ShellExecuteA(nullptr, "open", desktopPath, nullptr, nullptr, SW_SHOW);
    }
};

// ─── 全局快捷宏 ───────────────────────────────────────────────────────────────
#define LOG_INFO(cat, ...)   Logger::Instance().Info  (cat, __VA_ARGS__)
#define LOG_WARN(cat, ...)   Logger::Instance().Warn  (cat, __VA_ARGS__)
#define LOG_ERROR(cat, ...)  Logger::Instance().Error (cat, __VA_ARGS__)
#define LOG_DEBUG(cat, ...)  Logger::Instance().Debug (cat, __VA_ARGS__)
#define LOG_MOTION(cat, ...) Logger::Instance().Motion(cat, __VA_ARGS__)
