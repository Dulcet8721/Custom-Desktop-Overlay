#include <windows.h>
#include <string>
#include <vector>
#include <shellapi.h>
#include <commctrl.h>
#include <gdiplus.h>
#include <wincodec.h>
#include <wincodecsdk.h>
#include <cwctype>
#include <memory>
#include <algorithm>
#include <sstream>
#include <iomanip>
#include <thread>
#include <mutex>
#include <atomic>
#include <map>
#ifndef GET_X_LPARAM
#define GET_X_LPARAM(lParam) ((int)(short)LOWORD(lParam))
#endif
#ifndef GET_Y_LPARAM
#define GET_Y_LPARAM(lParam) ((int)(short)HIWORD(lParam))
#endif
#define IDI_MYICON 101
#define ID_TRAY_APP_ICON 1001
#define ID_TRAY_EXIT 1002
#define ID_TRAY_SHOW 1003
#define ID_TRAY_HIDE_OVERLAY 1004
#define WM_TRAYICON (WM_USER + 1)
#define ID_IMAGE_LIST 2002
#define ID_BROWSE_BUTTON 2003
#define ID_CONTEXT_OPEN 2004
#define ID_CONTEXT_REMOVE 2005
#define ID_HIDE_OVERLAY_CHECKBOX 2006
#define ID_MONITOR_LIST 2007
#define ID_SIZE_SLIDER 2010
#define ID_THEME_BUTTON 2014
#define ID_TAB_BUTTON1 2016
#define ID_TAB_BUTTON2 2017
#define ID_TAB_BUTTON3 2018
#define ID_OFFSET_X_DISPLAY 2020
#define ID_OFFSET_Y_DISPLAY 2021
#define ID_SIZE_DISPLAY 2022
#define ID_HOTKEY_TOGGLE_DISPLAY 2023
#define ID_HOTKEY_QUIT_DISPLAY 2024
#define ID_HOTKEY_SETTINGS_DISPLAY 2025
#define ID_RESET_TOGGLE 2026
#define ID_RESET_SETTINGS 2027
#define ID_RESET_QUIT 2028
#define ID_ANIMATION_TIMER 2029
#define WM_UPDATE_MONITOR_NAMES (WM_APP + 1)
#define WM_SHOW_AND_MESSAGE (WM_APP + 2)
using namespace Gdiplus;
struct ImageFile {
    std::wstring path;
    std::wstring filename;
};
struct MonitorInfo {
    int index;
    std::wstring name;
    std::wstring friendlyName;
    std::wstring deviceID;
    HMONITOR handle;
    RECT rect;
};
struct EditField {
    std::wstring text;
    bool hasFocus;
    int cursorPos;
    UINT id;
    HWND hwnd;
};
struct GdiObjectDeleter { void operator()(HGDIOBJ obj) { if(obj) DeleteObject(obj); } };
struct IconDeleter { void operator()(HICON icon) { if(icon) DestroyIcon(icon); } };
using UniqueHBrush = std::unique_ptr<std::remove_pointer<HBRUSH>::type, GdiObjectDeleter>;
using UniqueHIcon = std::unique_ptr<std::remove_pointer<HICON>::type, IconDeleter>;
using UniqueBitmap = std::unique_ptr<Bitmap>;
class AnimatedGIF {
private:
    IWICImagingFactory* m_pFactory;
    IWICBitmapDecoder* m_pDecoder;
    struct GIFrame {
        std::unique_ptr<Gdiplus::Bitmap> bitmap;
        UINT delay;
        UINT left, top, width, height;
        BYTE disposal;
    };
    std::vector<GIFrame> m_frames;
    UINT m_currentFrame;
    UINT m_width;
    UINT m_height;
    UINT_PTR m_timerId;
    HWND m_hwndOverlay;
    bool m_isInitialized;
    void Cleanup() {
        m_frames.clear();
        if (m_pDecoder) {
            m_pDecoder->Release();
            m_pDecoder = NULL;
        }
        m_currentFrame = 0;
        m_width = m_height = 0;
        m_isInitialized = false;
    }
    std::unique_ptr<Gdiplus::Bitmap> CreateBitmapFromConverter(IWICFormatConverter* pConverter, UINT width, UINT height) {
        auto bitmap = std::make_unique<Gdiplus::Bitmap>(width, height, PixelFormat32bppPARGB);
        Gdiplus::BitmapData bmpData;
        Gdiplus::Rect rect(0, 0, width, height);
        if (bitmap->LockBits(&rect, Gdiplus::ImageLockModeWrite, PixelFormat32bppPARGB, &bmpData) == Gdiplus::Ok) {
            WICRect rc = {0, 0, (INT)width, (INT)height};
            HRESULT hr = pConverter->CopyPixels(&rc, bmpData.Stride, bmpData.Stride * height, (BYTE*)bmpData.Scan0);
            bitmap->UnlockBits(&bmpData);
            if (SUCCEEDED(hr)) {
                return bitmap;
            }
        }
        return nullptr;
    }
public:
    AnimatedGIF() : m_pFactory(NULL), m_pDecoder(NULL), m_currentFrame(0), m_width(0), m_height(0), m_timerId(0), m_hwndOverlay(NULL), m_isInitialized(false) {
        CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
        CoCreateInstance(CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&m_pFactory));
    }
    ~AnimatedGIF() {
        StopAnimation();
        Cleanup();
        if (m_pFactory) m_pFactory->Release();
        CoUninitialize();
    }
    bool Load(const std::wstring& filename) {
        Cleanup();
        HRESULT hr = m_pFactory->CreateDecoderFromFilename(filename.c_str(), NULL, GENERIC_READ, WICDecodeMetadataCacheOnLoad, &m_pDecoder);
        if (FAILED(hr)) return false;
        UINT frameCount = 0;
        m_pDecoder->GetFrameCount(&frameCount);
        if (frameCount == 0) return false;
        IWICBitmapFrameDecode* pFirstFrame = NULL;
        hr = m_pDecoder->GetFrame(0, &pFirstFrame);
        if (FAILED(hr)) return false;
        pFirstFrame->GetSize(&m_width, &m_height);
        pFirstFrame->Release();
        auto composite = std::make_unique<Gdiplus::Bitmap>(m_width, m_height, PixelFormat32bppPARGB);
        {
            Gdiplus::Graphics g(composite.get());
            Gdiplus::SolidBrush clearBrush(Gdiplus::Color(0, 0, 0, 0));
            g.FillRectangle(&clearBrush, 0, 0, (INT)m_width, (INT)m_height);
        }
        m_frames.reserve(frameCount);
        for (UINT i = 0; i < frameCount; ++i) {
            IWICBitmapFrameDecode* pFrame = NULL;
            hr = m_pDecoder->GetFrame(i, &pFrame);
            if (FAILED(hr)) continue;
            UINT delay = 100;
            BYTE disposal = 0;
            UINT left = 0, top = 0, width = m_width, height = m_height;
            IWICMetadataQueryReader* pMeta = NULL;
            if (SUCCEEDED(pFrame->GetMetadataQueryReader(&pMeta))) {
                PROPVARIANT prop;
                PropVariantInit(&prop);
                if (SUCCEEDED(pMeta->GetMetadataByName(L"/grctlext/Delay", &prop)) && prop.vt == VT_UI2)
                    delay = prop.uiVal * 10;
                PropVariantClear(&prop);
                if (SUCCEEDED(pMeta->GetMetadataByName(L"/grctlext/Disposal", &prop)) && prop.vt == VT_UI1)
                    disposal = prop.bVal;
                PropVariantClear(&prop);
                if (SUCCEEDED(pMeta->GetMetadataByName(L"/imgdesc/Left", &prop)) && prop.vt == VT_UI2)
                    left = prop.uiVal;
                PropVariantClear(&prop);
                if (SUCCEEDED(pMeta->GetMetadataByName(L"/imgdesc/Top", &prop)) && prop.vt == VT_UI2)
                    top = prop.uiVal;
                PropVariantClear(&prop);
                if (SUCCEEDED(pMeta->GetMetadataByName(L"/imgdesc/Width", &prop)) && prop.vt == VT_UI2)
                    width = prop.uiVal;
                PropVariantClear(&prop);
                if (SUCCEEDED(pMeta->GetMetadataByName(L"/imgdesc/Height", &prop)) && prop.vt == VT_UI2)
                    height = prop.uiVal;
                PropVariantClear(&prop);
                pMeta->Release();
            }
            IWICFormatConverter* pConverter = NULL;
            hr = m_pFactory->CreateFormatConverter(&pConverter);
            std::unique_ptr<Gdiplus::Bitmap> frameBitmap;
            if (SUCCEEDED(hr)) {
                hr = pConverter->Initialize(pFrame, GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, NULL, 0.0, WICBitmapPaletteTypeCustom);
                if (SUCCEEDED(hr)) {
                    UINT frameW, frameH;
                    pConverter->GetSize(&frameW, &frameH);
                    if (frameW > 0 && frameH > 0) {
                        frameBitmap = CreateBitmapFromConverter(pConverter, frameW, frameH);
                    }
                }
                pConverter->Release();
            }
            pFrame->Release();
            if (!frameBitmap) {
                frameBitmap = std::make_unique<Gdiplus::Bitmap>(m_width, m_height, PixelFormat32bppPARGB);
                Gdiplus::Graphics g(frameBitmap.get());
                Gdiplus::SolidBrush clear(Gdiplus::Color(0,0,0,0));
                g.FillRectangle(&clear, 0, 0, (INT)m_width, (INT)m_height);
            }
            auto fullFrame = std::make_unique<Gdiplus::Bitmap>(m_width, m_height, PixelFormat32bppPARGB);
            {
                Gdiplus::Graphics g(fullFrame.get());
                g.SetCompositingMode(Gdiplus::CompositingModeSourceOver);
                g.DrawImage(composite.get(), 0, 0, (INT)m_width, (INT)m_height);
                g.DrawImage(frameBitmap.get(), (INT)left, (INT)top, (INT)width, (INT)height);
            }
            GIFrame gf;
            gf.bitmap = std::move(fullFrame);
            gf.delay = delay;
            gf.left = left;
            gf.top = top;
            gf.width = width;
            gf.height = height;
            gf.disposal = disposal;
            m_frames.push_back(std::move(gf));
            switch (disposal) {
                case 2:
                {
                    Gdiplus::Graphics g(composite.get());
                    g.SetCompositingMode(Gdiplus::CompositingModeSourceCopy);
                    Gdiplus::SolidBrush clear(Gdiplus::Color(0,0,0,0));
                    g.FillRectangle(&clear, (INT)left, (INT)top, (INT)width, (INT)height);
                    break;
                }
                case 3:
                    break;
                default:
                {
                    Gdiplus::Graphics g(composite.get());
                    g.SetCompositingMode(Gdiplus::CompositingModeSourceCopy);
                    g.DrawImage(m_frames.back().bitmap.get(), 0, 0, (INT)m_width, (INT)m_height);
                    break;
                }
            }
        }
        m_currentFrame = 0;
        m_isInitialized = true;
        return true;
    }
    void StartAnimation(HWND hwndOverlay) {
        if (!m_isInitialized || m_frames.empty()) return;
        m_hwndOverlay = hwndOverlay;
        if (m_frames.size() > 1 && m_hwndOverlay) {
            if (m_timerId) KillTimer(m_hwndOverlay, m_timerId);
            m_timerId = SetTimer(m_hwndOverlay, ID_ANIMATION_TIMER, m_frames[m_currentFrame].delay, NULL);
        }
    }
    void StopAnimation() {
        if (m_timerId && m_hwndOverlay) {
            KillTimer(m_hwndOverlay, m_timerId);
            m_timerId = 0;
        }
    }
    void NextFrame() {
        if (!m_isInitialized || m_frames.empty()) return;
        m_currentFrame = (m_currentFrame + 1) % m_frames.size();
        if (m_timerId && m_hwndOverlay) {
            KillTimer(m_hwndOverlay, m_timerId);
            m_timerId = SetTimer(m_hwndOverlay, ID_ANIMATION_TIMER, m_frames[m_currentFrame].delay, NULL);
        }
    }
    bool GetCurrentFrame(Gdiplus::Graphics* pGraphics, int drawX, int drawY, int drawWidth, int drawHeight) {
        if (!m_isInitialized || m_currentFrame >= m_frames.size()) return false;
        Gdiplus::Bitmap* pBitmap = m_frames[m_currentFrame].bitmap.get();
        if (!pBitmap) return false;
        pGraphics->DrawImage(pBitmap, drawX, drawY, drawWidth, drawHeight);
        return true;
    }
    int GetWidth() const { return static_cast<int>(m_width); }
    int GetHeight() const { return static_cast<int>(m_height); }
    bool IsAnimated() const { return m_frames.size() > 1; }
    bool IsInitialized() const { return m_isInitialized; }
};
void DrawOwnerDrawButton(HDC hdc, HWND hwnd, bool isPressed, const std::wstring& text, bool isActiveTab = false);
void DrawOwnerDrawCheckbox(HDC hdc, HWND hwnd, bool isChecked, const std::wstring& text);
void DrawOwnerDrawListBox(HDC hdc, HWND hwnd, int type);
void DrawOwnerDrawEdit(HDC hdc, HWND hwnd, EditField* field, bool darkTheme);
LRESULT HandleOwnerDrawButton(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam, int buttonId);
LRESULT HandleOwnerDrawCheckbox(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
LRESULT HandleOwnerDrawListBox(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam, int listType);
LRESULT HandleOwnerDrawEdit(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam, EditField* field);
void UpdateAllButtons();
void AddImageToList(const std::wstring& filePath, bool saveToIni);
void SaveAllSettingsToIni();
void LoadAllSettingsFromIni();
void CreateOverlayWindow(const std::wstring& imagePath);
void UpdateScaleControls();
void UpdateOffsetEditControls();
void UpdateImageSelection();
void UpdateMonitorSelection();
void UpdateHideOverlayCheckbox();
void ToggleHideOverlay();
void CorrectOffsetValues();
void CorrectScaleValue();
void ApplyTheme();
void ToggleTheme();
void SafeGdiplusShutdown();
void UpdateHotkeyEdits();
bool RegisterHotkeys();
void UnregisterHotkeys();
bool IsValidHotkeyChar(wchar_t c);
bool CheckHotkeyConflict(wchar_t newKey, int excludeId, std::wstring& conflictAction);
void UpdateMonitorList();
std::wstring GetIniFilePath();
bool IsSupportedImageFormat(const std::wstring& filePath);
void DrawMenuItem(HDC hdc, RECT rc, const std::wstring& text, bool isSelected, bool isChecked, bool isDisabled, bool isTrayHideOverlay);
void CreateOwnerDrawMenu(HMENU hMenu, int position, UINT id, const wchar_t* text);
void ApplyOffsetFromField(EditField* field, bool isX);
void ApplyScaleFromField(EditField* field);
void ApplyHotkeyFromField(EditField* field, int id);
void UpdateEditFieldText(EditField* field, const std::wstring& newText);
void ShowOwnerDrawMessageBox(HWND hwndParent, const std::wstring& message);
LRESULT CALLBACK OwnerDrawMessageBoxProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam);
void DrawMessageBoxButton(HDC hdc, HWND hwnd, bool isPressed, bool isFocused);
void ResetHotkeyField(int fieldId, wchar_t defaultKey, wchar_t& currentKey, wchar_t& lastValidKey, EditField& field, bool showConflictMessage = true);
void UpdateOffsetBoundsDisplay();
void CheckMonitorConfiguration();
void UpdateOverlayPosition();
void RenderOverlay();
void RenderOverlayInThread();
struct RenderContext {
    HWND overlayWindow;
    int windowWidth;
    int windowHeight;
    int drawWidth;
    int drawHeight;
    int drawX;
    int drawY;
    std::wstring imagePath;
    float imageScale;
    int offsetX;
    int offsetY;
    int selectedMonitor;
};
NOTIFYICONDATA nid = {};
HWND g_hwnd = NULL;
HWND g_hImageList = NULL;
HWND g_hBrowseButton = NULL;
HWND g_hHideOverlayCheckbox = NULL;
HWND g_hMonitorList = NULL;
HWND g_hOffsetXDisplay = NULL;
HWND g_hOffsetYDisplay = NULL;
HWND g_hSizeSlider = NULL;
HWND g_hSizeDisplay = NULL;
HWND g_hHotkeyToggleDisplay = NULL;
HWND g_hHotkeyQuitDisplay = NULL;
HWND g_hThemeButton = NULL;
HWND g_hHotkeySettingsDisplay = NULL;
HWND g_hTabButton1 = NULL;
HWND g_hTabButton2 = NULL;
HWND g_hTabButton3 = NULL;
HWND g_hResetToggle = NULL;
HWND g_hResetSettings = NULL;
HWND g_hResetQuit = NULL;
HFONT g_hTabFont = NULL;
HFONT g_hMenuFont = NULL;
HICON g_hMainIcon = NULL;
UniqueHBrush g_hTrackbarBackgroundBrush(nullptr);
UniqueHBrush g_hActiveTabBrush(nullptr);
UniqueHBrush g_hListBoxBackgroundBrush(nullptr);
UniqueHBrush g_hListBoxSelectedBrush(nullptr);
bool g_windowVisible = false;
bool g_hideOverlay = false;
float g_imageScale = 1.0f;
std::vector<ImageFile> g_imageFiles;
std::vector<MonitorInfo> g_monitors;
HWND g_currentOverlay = NULL;
ULONG_PTR g_gdiplusToken = 0;
std::wstring g_lastDisplayedImage;
int g_selectedMonitor = 0;
int g_offsetX = 0;
int g_offsetY = 0;
int g_activeTab = 1;
bool g_darkTheme = false;
#define DEFAULT_TOGGLE_KEY L'H'
#define DEFAULT_QUIT_KEY L'Q'
#define DEFAULT_SETTINGS_KEY L'S'
wchar_t g_toggleHotkeyKey = DEFAULT_TOGGLE_KEY;
wchar_t g_quitHotkeyKey = DEFAULT_QUIT_KEY;
wchar_t g_settingsHotkeyKey = DEFAULT_SETTINGS_KEY;
int g_windowX = CW_USEDEFAULT;
int g_windowY = CW_USEDEFAULT;
UniqueHBrush g_hBackgroundBrush(nullptr);
UniqueHBrush g_hButtonBrush(nullptr);
UniqueHBrush g_hButtonPressedBrush(nullptr);
UniqueHBrush g_hMenuBackgroundBrush(nullptr);
UniqueHBrush g_hMenuSelectedBrush(nullptr);
COLORREF g_textColor = RGB(0, 0, 0);
COLORREF g_buttonTextColor = RGB(0, 0, 0);
COLORREF g_borderColor = RGB(200, 200, 200);
COLORREF g_buttonBorderColor = RGB(150, 150, 150);
COLORREF g_menuBackgroundColor = RGB(255, 255, 255);
COLORREF g_menuTextColor = RGB(0, 0, 0);
COLORREF g_menuBorderColor = RGB(200, 200, 200);
COLORREF g_menuSelectedColor = RGB(200, 200, 200);
wchar_t g_lastValidToggleKey = DEFAULT_TOGGLE_KEY;
wchar_t g_lastValidQuitKey = DEFAULT_QUIT_KEY;
wchar_t g_lastValidSettingsKey = DEFAULT_SETTINGS_KEY;
bool g_sliderDragging = false;
int g_imageListSelected = -1;
int g_monitorListSelected = -1;
int g_imageListScrollPos = 0;
int g_monitorListScrollPos = 0;
const int ITEM_HEIGHT = 16;
const int VISIBLE_ITEMS_IMAGES = 9;
const int VISIBLE_ITEMS_MONITORS = 3;
EditField g_offsetXField = {L"0", false, 0, ID_OFFSET_X_DISPLAY, NULL};
EditField g_offsetYField = {L"0", false, 0, ID_OFFSET_Y_DISPLAY, NULL};
EditField g_sizeField = {L"1.00", false, 0, ID_SIZE_DISPLAY, NULL};
EditField g_hotkeyToggleField = {L"H", false, 0, ID_HOTKEY_TOGGLE_DISPLAY, NULL};
EditField g_hotkeySettingsField = {L"S", false, 0, ID_HOTKEY_SETTINGS_DISPLAY, NULL};
EditField g_hotkeyQuitField = {L"Q", false, 0, ID_HOTKEY_QUIT_DISPLAY, NULL};
static int g_prevMonitorCount = 0;
static std::vector<RECT> g_prevMonitorRects;
UniqueBitmap g_cachedBitmap(nullptr);
int g_originalImageWidth = 0;
int g_originalImageHeight = 0;
int g_currentMonitor = -1;
std::unique_ptr<AnimatedGIF> g_animatedGIF;
std::mutex g_renderMutex;
std::mutex g_dataMutex;
std::thread g_renderThread;
std::atomic<bool> g_renderThreadRunning(false);
std::atomic<bool> g_renderThreadShouldExit(false);
std::atomic<bool> g_renderRequestPending(false);
RenderContext g_currentRenderContext;
std::atomic<bool> g_monitorNamesLoaded{false};
std::atomic<bool> g_programExiting{false};
std::mutex g_monitorMutex;
std::thread g_monitorNamesThread;
LRESULT CALLBACK OverlayProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam);
std::wstring GetIniFilePath() {
    wchar_t exePath[MAX_PATH] = {0};
    GetModuleFileNameW(NULL, exePath, MAX_PATH);
    std::wstring exeDir = exePath;
    size_t lastSlash = exeDir.find_last_of(L"\\/");
    if (lastSlash != std::wstring::npos) exeDir = exeDir.substr(0, lastSlash + 1);
    return exeDir + L"settings.ini";
}
std::map<std::wstring, std::wstring> GetDeviceIdMap() {
    std::map<std::wstring, std::wstring> deviceMap;
    DISPLAY_DEVICE ddAdapter, ddMonitor;
    ddAdapter.cb = sizeof(DISPLAY_DEVICE);
    ddMonitor.cb = sizeof(DISPLAY_DEVICE);
    for (DWORD adapter = 0; EnumDisplayDevices(NULL, adapter, &ddAdapter, 0); ++adapter) {
        for (DWORD monitor = 0; EnumDisplayDevices(ddAdapter.DeviceName, monitor, &ddMonitor, EDD_GET_DEVICE_INTERFACE_NAME); ++monitor) {
            if (ddMonitor.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP) {
                deviceMap[ddMonitor.DeviceName] = ddMonitor.DeviceID;
            }
        }
    }
    return deviceMap;
}
BOOL CALLBACK MonitorEnumProc(HMONITOR hMonitor, HDC hdcMonitor, LPRECT lprcMonitor, LPARAM dwData) {
    static std::map<std::wstring, std::wstring> deviceMap = GetDeviceIdMap();
    MONITORINFOEX monitorInfo = {0};
    monitorInfo.cbSize = sizeof(monitorInfo);
    if (!GetMonitorInfo(hMonitor, &monitorInfo)) return TRUE;
    MonitorInfo info;
    info.index = static_cast<int>(g_monitors.size());
    info.handle = hMonitor;
    info.rect = *lprcMonitor;
    std::wstring deviceName = monitorInfo.szDevice;
    std::wstring key = deviceName + L"\\Monitor0";
    auto it = deviceMap.find(key);
    if (it != deviceMap.end()) info.deviceID = it->second;
    DEVMODE devMode = {0};
    devMode.dmSize = sizeof(DEVMODE);
    if (EnumDisplaySettings(monitorInfo.szDevice, ENUM_CURRENT_SETTINGS, &devMode)) {
        info.name = std::to_wstring(devMode.dmPelsWidth) + L"x" + std::to_wstring(devMode.dmPelsHeight) + L" @" + std::to_wstring(devMode.dmDisplayFrequency) + L"Hz";
    } else {
        int width = info.rect.right - info.rect.left;
        int height = info.rect.bottom - info.rect.top;
        info.name = std::to_wstring(width) + L"x" + std::to_wstring(height);
    }
    g_monitors.push_back(info);
    return TRUE;
}
std::vector<std::pair<std::wstring, std::wstring>> GetMonitorFriendlyNamesPowerShellWithInstance() {
    std::vector<std::pair<std::wstring, std::wstring>> result;
    HANDLE hReadPipe, hWritePipe;
    SECURITY_ATTRIBUTES sa = { sizeof(SECURITY_ATTRIBUTES), NULL, TRUE };
    if (!CreatePipe(&hReadPipe, &hWritePipe, &sa, 0)) return result;
    STARTUPINFOW si = { sizeof(STARTUPINFOW) };
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = hWritePipe;
    si.hStdError = hWritePipe;
    PROCESS_INFORMATION pi;
    std::wstring command = L"C:\\Windows\\System32\\WindowsPowerShell\\v1.0\\powershell.exe -Command \"[Console]::OutputEncoding = [System.Text.Encoding]::UTF8; Get-CimInstance -Namespace root/wmi -ClassName WmiMonitorID | ForEach-Object { $instance = $_.InstanceName; if ($_.UserFriendlyName) { $name = ($_.UserFriendlyName | ForEach-Object { [char]$_ }) -join '' } else { $parts = $instance.Split('\\\\'); if ($parts.Count -ge 3) { $name = $parts[1] } else { $name = 'Unknown' } }; Write-Output \\\"$instance`t$name\\\"; }\"";
    BOOL success = CreateProcessW(NULL, &command[0], NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi);
    CloseHandle(hWritePipe);
    if (success) {
        char buffer[4096];
        DWORD bytesRead;
        std::string resultStr;
        while (ReadFile(hReadPipe, buffer, sizeof(buffer) - 1, &bytesRead, NULL) && bytesRead > 0) {
            buffer[bytesRead] = '\0';
            resultStr += buffer;
        }
        WaitForSingleObject(pi.hProcess, INFINITE);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        size_t pos = 0;
        while (pos < resultStr.size()) {
            size_t end = resultStr.find('\n', pos);
            if (end == std::string::npos) end = resultStr.size();
            std::string line = resultStr.substr(pos, end - pos);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (!line.empty()) {
                size_t tabPos = line.find('\t');
                if (tabPos != std::string::npos) {
                    std::string instancePart = line.substr(0, tabPos);
                    std::string namePart = line.substr(tabPos + 1);
                    auto toWide = [](const std::string& s) -> std::wstring {
                        int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, NULL, 0);
                        if (len <= 0) return L"";
                        std::wstring w(len, 0);
                        MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &w[0], len);
                        w.pop_back();
                        if (!w.empty() && w[0] == 0xFEFF) w.erase(0, 1);
                        return w;
                    };
                    std::wstring wInstance = toWide(instancePart);
                    std::wstring wName = toWide(namePart);
                    if (!wInstance.empty() && !wName.empty()) result.emplace_back(wInstance, wName);
                }
            }
            pos = end + 1;
        }
    }
    CloseHandle(hReadPipe);
    return result;
}
std::wstring ExtractModelFromInstanceName(const std::wstring& instanceName) {
    size_t start = instanceName.find(L'\\');
    if (start == std::wstring::npos) return L"";
    start++;
    size_t end = instanceName.find(L'\\', start);
    if (end == std::wstring::npos) return L"";
    return instanceName.substr(start, end - start);
}
std::wstring ExtractModelFromDeviceID(const std::wstring& deviceID) {
    size_t start = deviceID.find(L'#');
    if (start == std::wstring::npos) return L"";
    start++;
    size_t end = deviceID.find(L'#', start);
    if (end == std::wstring::npos) return L"";
    return deviceID.substr(start, end - start);
}
void LoadMonitorNamesThread() {
    auto psPairs = GetMonitorFriendlyNamesPowerShellWithInstance();
    std::map<std::wstring, std::wstring> modelToName;
    for (const auto& pair : psPairs) {
        std::wstring model = ExtractModelFromInstanceName(pair.first);
        if (!model.empty()) modelToName[model] = pair.second;
    }
    {
        std::lock_guard<std::mutex> lock(g_monitorMutex);
        for (auto& mon : g_monitors) {
            if (!mon.deviceID.empty()) {
                std::wstring model = ExtractModelFromDeviceID(mon.deviceID);
                if (!model.empty()) {
                    auto it = modelToName.find(model);
                    if (it != modelToName.end()) mon.friendlyName = it->second;
                }
            }
        }
        g_monitorNamesLoaded = true;
    }
    if (g_programExiting) return;
    PostMessage(g_hwnd, WM_UPDATE_MONITOR_NAMES, 0, 0);
}
void ForceForegroundWindow(HWND hwnd) {
    DWORD foreThread = GetWindowThreadProcessId(GetForegroundWindow(), NULL);
    DWORD appThread = GetCurrentThreadId();
    if (foreThread != appThread) {
        AttachThreadInput(foreThread, appThread, TRUE);
        SetForegroundWindow(hwnd);
        AttachThreadInput(foreThread, appThread, FALSE);
    } else {
        SetForegroundWindow(hwnd);
    }
    BringWindowToTop(hwnd);
    SetWindowPos(hwnd, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
}
void UpdateMonitorList() {
    {
        std::lock_guard<std::mutex> lock(g_monitorMutex);
        g_monitors.clear();
        EnumDisplayMonitors(NULL, NULL, MonitorEnumProc, 0);
    }
    if (g_monitorNamesThread.joinable()) g_monitorNamesThread.join();
    if (!g_monitorNamesLoaded) {
        g_monitorNamesThread = std::thread(LoadMonitorNamesThread);
    }
    if (g_selectedMonitor >= static_cast<int>(g_monitors.size())) {
        g_selectedMonitor = 0;
        g_monitorListSelected = 0;
    } else {
        g_monitorListSelected = g_selectedMonitor;
    }
    if (g_hMonitorList) InvalidateRect(g_hMonitorList, NULL, TRUE);
    UpdateOffsetBoundsDisplay();
}
bool IsMessageBoxWithTextOpen(const std::wstring& text) {
    HWND hWnd = NULL;
    while (true) {
        hWnd = FindWindowEx(NULL, hWnd, L"OwnerDrawMessageBox", NULL);
        if (!hWnd) break;
        HANDLE hProp = GetProp(hWnd, L"MessageText");
        if (hProp) {
            std::wstring* storedText = (std::wstring*)hProp;
            if (*storedText == text) {
                return true;
            }
        }
    }
    return false;
}
int GetOpenMessageBoxCount() {
    int count = 0;
    HWND hWnd = NULL;
    while (true) {
        hWnd = FindWindowEx(NULL, hWnd, L"OwnerDrawMessageBox", NULL);
        if (!hWnd) break;
        count++;
    }
    return count;
}
void ActivateNextMessageBox(HWND hCurrent) {
    HWND hWnd = NULL;
    while (true) {
        hWnd = FindWindowEx(NULL, hWnd, L"OwnerDrawMessageBox", NULL);
        if (!hWnd) break;
        if (hWnd != hCurrent) {
            ForceForegroundWindow(hWnd);
            return;
        }
    }
}
void UpdateOffsetBoundsDisplay() {
    if (g_hwnd && IsWindowVisible(g_hMonitorList)) {
        RECT boundsRect = {8, 118, 280, 180};
        InvalidateRect(g_hwnd, &boundsRect, TRUE);
        UpdateWindow(g_hwnd);
    }
}
void CheckMonitorConfiguration() {
    std::vector<MonitorInfo> currentMonitors = g_monitors;
    g_monitors.clear();
    EnumDisplayMonitors(NULL, NULL, MonitorEnumProc, 0);
    bool configChanged = false;
    if (g_monitors.size() != g_prevMonitorCount) {
        configChanged = true;
    } else {
        for (size_t i = 0; i < g_monitors.size(); ++i) {
            if (memcmp(&g_monitors[i].rect, &g_prevMonitorRects[i], sizeof(RECT)) != 0) {
                configChanged = true;
                break;
            }
        }
    }
    if (configChanged) {
        if (g_hMonitorList) InvalidateRect(g_hMonitorList, NULL, TRUE);
        if (g_selectedMonitor >= static_cast<int>(g_monitors.size())) {
            g_selectedMonitor = 0;
            g_monitorListSelected = 0;
        }
        CorrectOffsetValues();
        UpdateOffsetBoundsDisplay();
        if (g_currentOverlay) {
            if (g_selectedMonitor != g_currentMonitor) {
                UpdateOverlayPosition();
                g_renderRequestPending = true;
            }
        }
        g_prevMonitorCount = g_monitors.size();
        g_prevMonitorRects.clear();
        for (const auto& monitor : g_monitors) {
            g_prevMonitorRects.push_back(monitor.rect);
        }
    }
}
bool IsSupportedImageFormat(const std::wstring& filePath) {
    size_t dotPos = filePath.find_last_of(L".");
    if (dotPos == std::wstring::npos) return false;
    std::wstring ext = filePath.substr(dotPos + 1);
    for (wchar_t& c : ext) c = towlower(c);
    return ext == L"png" || ext == L"jpg" || ext == L"jpeg" || ext == L"gif";
}
void DrawOwnerDrawButton(HDC hdc, HWND hwnd, bool isPressed, const std::wstring& text, bool isActiveTab) {
    RECT rc;
    GetClientRect(hwnd, &rc);
    HBRUSH hBrush;
    if (isActiveTab) {
        hBrush = g_hActiveTabBrush.get();
    } else if (isPressed) {
        hBrush = g_hButtonPressedBrush.get();
    } else {
        hBrush = g_hButtonBrush.get();
    }
    FillRect(hdc, &rc, hBrush);
    HPEN hBorderPen = CreatePen(PS_SOLID, 1, g_buttonBorderColor);
    HPEN hOldPen = (HPEN)SelectObject(hdc, hBorderPen);
    HBRUSH hOldBrush = (HBRUSH)SelectObject(hdc, GetStockObject(NULL_BRUSH));
    Rectangle(hdc, rc.left, rc.top, rc.right, rc.bottom);
    SelectObject(hdc, hOldBrush);
    SelectObject(hdc, hOldPen);
    DeleteObject(hBorderPen);
    if (isPressed && !isActiveTab) {
        OffsetRect(&rc, 1, 1);
    }
    SetTextColor(hdc, g_buttonTextColor);
    SetBkMode(hdc, TRANSPARENT);
    HFONT hOldFont = (HFONT)SelectObject(hdc, g_hTabFont);
    DrawText(hdc, text.c_str(), -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    SelectObject(hdc, hOldFont);
}
void DrawOwnerDrawCheckbox(HDC hdc, HWND hwnd, bool isChecked, const std::wstring& text) {
    RECT rc;
    GetClientRect(hwnd, &rc);
    FillRect(hdc, &rc, g_hBackgroundBrush.get());
    int checkboxSize = 13;
    int textMargin = 5;
    int checkboxX = rc.left + 2;
    int checkboxY = rc.top + (rc.bottom - rc.top - checkboxSize) / 2;
    RECT checkboxRect = { checkboxX, checkboxY, checkboxX + checkboxSize, checkboxY + checkboxSize };
    HBRUSH hCheckboxBrush = CreateSolidBrush(g_darkTheme ? RGB(60, 60, 60) : RGB(220, 220, 220));
    FillRect(hdc, &checkboxRect, hCheckboxBrush);
    DeleteObject(hCheckboxBrush);
    HPEN hBorderPen = CreatePen(PS_SOLID, 1, g_darkTheme ? RGB(100, 100, 100) : RGB(150, 150, 150));
    HPEN hOldPen = (HPEN)SelectObject(hdc, hBorderPen);
    HBRUSH hOldBrush = (HBRUSH)SelectObject(hdc, GetStockObject(NULL_BRUSH));
    Rectangle(hdc, checkboxRect.left, checkboxRect.top, checkboxRect.right, checkboxRect.bottom);
    SelectObject(hdc, hOldBrush);
    SelectObject(hdc, hOldPen);
    DeleteObject(hBorderPen);
    if (isChecked) {
        COLORREF checkColor = g_darkTheme ? RGB(220, 220, 220) : RGB(50, 50, 50);
        HPEN hCheckPen = CreatePen(PS_SOLID, 2, checkColor);
        hOldPen = (HPEN)SelectObject(hdc, hCheckPen);
        int startX = checkboxX + 3;
        int startY = checkboxY + checkboxSize / 2;
        int middleX = checkboxX + checkboxSize / 2;
        int middleY = checkboxY + checkboxSize - 3;
        int endX = checkboxX + checkboxSize - 3;
        int endY = checkboxY + 2;
        MoveToEx(hdc, startX, startY, NULL);
        LineTo(hdc, middleX, middleY);
        LineTo(hdc, endX, endY);
        SelectObject(hdc, hOldPen);
        DeleteObject(hCheckPen);
    }
    SetTextColor(hdc, g_textColor);
    SetBkMode(hdc, TRANSPARENT);
    RECT textRect = rc;
    textRect.left = checkboxX + checkboxSize + textMargin;
    HFONT hOldFont = (HFONT)SelectObject(hdc, g_hTabFont);
    DrawText(hdc, text.c_str(), -1, &textRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    SelectObject(hdc, hOldFont);
}
void DrawOwnerDrawListBox(HDC hdc, HWND hwnd, int type) {
    RECT rc;
    GetClientRect(hwnd, &rc);
    FillRect(hdc, &rc, g_hListBoxBackgroundBrush.get());
    COLORREF borderColor = g_darkTheme ? RGB(80, 80, 80) : RGB(150, 150, 150);
    HPEN hBorderPen = CreatePen(PS_SOLID, 1, borderColor);
    HPEN hOldPen = (HPEN)SelectObject(hdc, hBorderPen);
    HBRUSH hOldBrush = (HBRUSH)SelectObject(hdc, GetStockObject(NULL_BRUSH));
    Rectangle(hdc, rc.left, rc.top, rc.right, rc.bottom);
    SelectObject(hdc, hOldBrush);
    SelectObject(hdc, hOldPen);
    DeleteObject(hBorderPen);
    SetTextColor(hdc, g_textColor);
    SetBkMode(hdc, TRANSPARENT);
    HFONT hOldFont = (HFONT)SelectObject(hdc, g_hTabFont);
    if (type == ID_IMAGE_LIST) {
        int visibleItems = VISIBLE_ITEMS_IMAGES;
        int startIdx = g_imageListScrollPos;
        int endIdx = std::min(startIdx + visibleItems, (int)g_imageFiles.size());
        for (int i = startIdx; i < endIdx; i++) {
            RECT itemRect = { 
                rc.left + 1, 
                rc.top + 1 + (i - startIdx) * ITEM_HEIGHT, 
                rc.right - 1, 
                rc.top + 1 + (i - startIdx + 1) * ITEM_HEIGHT 
            };
            if (i == g_imageListSelected) {
                RECT fillRect = itemRect;
                FillRect(hdc, &fillRect, g_hListBoxSelectedBrush.get());
            }
            RECT textRect = itemRect;
            textRect.left += 2;
            DrawText(hdc, g_imageFiles[i].filename.c_str(), -1, &textRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        }
    } else if (type == ID_MONITOR_LIST) {
        int visibleItems = VISIBLE_ITEMS_MONITORS;
        int startIdx = g_monitorListScrollPos;
        int endIdx = std::min(startIdx + visibleItems, (int)g_monitors.size());
        for (int i = startIdx; i < endIdx; i++) {
            RECT itemRect = { 
                rc.left + 1, 
                rc.top + 1 + (i - startIdx) * ITEM_HEIGHT, 
                rc.right - 1, 
                rc.top + 1 + (i - startIdx + 1) * ITEM_HEIGHT 
            };
            if (i == g_monitorListSelected) {
                RECT fillRect = itemRect;
                FillRect(hdc, &fillRect, g_hListBoxSelectedBrush.get());
            }
            std::wstring displayText = std::to_wstring(i + 1) + L". ";
            if (!g_monitors[i].friendlyName.empty()) {
                displayText += g_monitors[i].friendlyName + L" (";
            }
            displayText += g_monitors[i].name;
            if (!g_monitors[i].friendlyName.empty()) {
                displayText += L")";
            }
            RECT textRect = itemRect;
            textRect.left += 2;
            DrawText(hdc, displayText.c_str(), -1, &textRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        }
    }
    SelectObject(hdc, hOldFont);
}
void DrawOwnerDrawEdit(HDC hdc, HWND hwnd, EditField* field, bool darkTheme) {
    RECT rc;
    GetClientRect(hwnd, &rc);
    FillRect(hdc, &rc, g_hListBoxBackgroundBrush.get());
    COLORREF borderColor = darkTheme ? RGB(80, 80, 80) : RGB(150, 150, 150);
    if (field->hasFocus) {
        borderColor = darkTheme ? RGB(255, 255, 255) : RGB(0, 0, 0);
    }
    HPEN hBorderPen = CreatePen(PS_SOLID, 1, borderColor);
    HPEN hOldPen = (HPEN)SelectObject(hdc, hBorderPen);
    HBRUSH hOldBrush = (HBRUSH)SelectObject(hdc, GetStockObject(NULL_BRUSH));
    Rectangle(hdc, rc.left, rc.top, rc.right, rc.bottom);
    SelectObject(hdc, hOldBrush);
    SelectObject(hdc, hOldPen);
    DeleteObject(hBorderPen);
    SetTextColor(hdc, g_textColor);
    SetBkMode(hdc, TRANSPARENT);
    RECT textRect = rc;
    textRect.left += 2;
    textRect.right -= 2;
    HFONT hOldFont = (HFONT)SelectObject(hdc, g_hTabFont);
    DrawText(hdc, field->text.c_str(), -1, &textRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    if (field->hasFocus) {
        SIZE size;
        std::wstring prefix = field->text.substr(0, field->cursorPos);
        GetTextExtentPoint32(hdc, prefix.c_str(), prefix.length(), &size);
        int cursorX = textRect.left + size.cx;
        HPEN hCursorPen = CreatePen(PS_SOLID, 1, darkTheme ? RGB(240, 240, 240) : RGB(0, 0, 0));
        HPEN hOldPen2 = (HPEN)SelectObject(hdc, hCursorPen);
        MoveToEx(hdc, cursorX, textRect.top + 2, NULL);
        LineTo(hdc, cursorX, textRect.bottom - 2);
        SelectObject(hdc, hOldPen2);
        DeleteObject(hCursorPen);
    }
    SelectObject(hdc, hOldFont);
}
void DrawMenuItem(HDC hdc, RECT rc, const std::wstring& text, bool isSelected, bool isChecked, bool isDisabled, bool isTrayHideOverlay) {
    bool isTrayMenuItem = isTrayHideOverlay || text == L"Settings" || text == L"Quit";
    HBRUSH hBrush = g_hMenuBackgroundBrush.get();
    FillRect(hdc, &rc, hBrush);
    if (isTrayMenuItem) {
        int checkSize = 13;
        int checkX = rc.left + 5;
        int checkY = rc.top + (rc.bottom - rc.top - checkSize) / 2;
        RECT checkRect = { checkX, checkY, checkX + checkSize, checkY + checkSize };
        if (isTrayHideOverlay) {
            HPEN hCheckBorder = CreatePen(PS_SOLID, 1, g_darkTheme ? RGB(100, 100, 100) : RGB(180, 180, 180));
            HPEN hOldPen = (HPEN)SelectObject(hdc, hCheckBorder);
            HBRUSH hOldBrush = (HBRUSH)SelectObject(hdc, GetStockObject(NULL_BRUSH));
            Rectangle(hdc, checkRect.left, checkRect.top, checkRect.right, checkRect.bottom);
            SelectObject(hdc, hOldBrush);
            SelectObject(hdc, hOldPen);
            DeleteObject(hCheckBorder);
            HBRUSH hCheckboxBrush = CreateSolidBrush(g_darkTheme ? RGB(60, 60, 60) : RGB(220, 220, 220));
            FillRect(hdc, &checkRect, hCheckboxBrush);
            DeleteObject(hCheckboxBrush);
            if (isChecked) {
                HPEN hCheckPen = CreatePen(PS_SOLID, 2, isDisabled ? (g_darkTheme ? RGB(100, 100, 100) : RGB(180, 180, 180)) : g_menuTextColor);
                hOldPen = (HPEN)SelectObject(hdc, hCheckPen);
                MoveToEx(hdc, checkX + 3, checkY + checkSize / 2, NULL);
                LineTo(hdc, checkX + checkSize / 2, checkY + checkSize - 3);
                LineTo(hdc, checkX + checkSize - 3, checkY + 2);
                SelectObject(hdc, hOldPen);
                DeleteObject(hCheckPen);
            }
        }
    }
    SetTextColor(hdc, isDisabled ? (g_darkTheme ? RGB(120, 120, 120) : RGB(150, 150, 150)) : g_menuTextColor);
    SetBkMode(hdc, TRANSPARENT);
    RECT textRect = rc;
    textRect.left = rc.left + (isTrayMenuItem ? 25 : 10);
    textRect.right = rc.right - 10;
    HFONT hOldFont = (HFONT)SelectObject(hdc, g_hMenuFont);
    DrawText(hdc, text.c_str(), -1, &textRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    SelectObject(hdc, hOldFont);
}
void CreateOwnerDrawMenu(HMENU hMenu, int position, UINT id, const wchar_t* text) {
    MENUITEMINFO mii = {0};
    mii.cbSize = sizeof(MENUITEMINFO);
    mii.fMask = MIIM_ID | MIIM_TYPE | MIIM_STATE | MIIM_DATA;
    mii.fType = MFT_OWNERDRAW;
    mii.wID = id;
    mii.dwTypeData = (LPWSTR)text;
    mii.dwItemData = (ULONG_PTR)text;
    InsertMenuItem(hMenu, position, TRUE, &mii);
}
void UpdateAllButtons() {
    if (g_hTabButton1) InvalidateRect(g_hTabButton1, NULL, TRUE);
    if (g_hTabButton2) InvalidateRect(g_hTabButton2, NULL, TRUE);
    if (g_hTabButton3) InvalidateRect(g_hTabButton3, NULL, TRUE);
    if (g_hBrowseButton) InvalidateRect(g_hBrowseButton, NULL, TRUE);
    if (g_hThemeButton) InvalidateRect(g_hThemeButton, NULL, TRUE);
    if (g_hHideOverlayCheckbox) InvalidateRect(g_hHideOverlayCheckbox, NULL, TRUE);
}
LRESULT HandleOwnerDrawButton(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam, int buttonId) {
    static bool isPressed = false;
    switch (msg) {
        case WM_LBUTTONDOWN: {
            isPressed = true;
            SetCapture(hwnd);
            InvalidateRect(hwnd, NULL, TRUE);
            break;
        }
        case WM_LBUTTONUP: {
            if (isPressed) {
                isPressed = false;
                ReleaseCapture();
                InvalidateRect(hwnd, NULL, TRUE);
                if (buttonId == ID_TAB_BUTTON1 || buttonId == ID_TAB_BUTTON2 || buttonId == ID_TAB_BUTTON3) {
                    g_activeTab = (buttonId == ID_TAB_BUTTON1) ? 1 : (buttonId == ID_TAB_BUTTON2) ? 2 : 3;
                }
                POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
                ClientToScreen(hwnd, &pt);
                SendMessage(g_hwnd, WM_COMMAND, MAKEWPARAM(buttonId, BN_CLICKED), (LPARAM)hwnd);
            }
            break;
        }
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);
            wchar_t buffer[256];
            GetWindowText(hwnd, buffer, 256);
            bool isActiveTab = false;
            if (buttonId == ID_TAB_BUTTON1 && g_activeTab == 1) isActiveTab = true;
            else if (buttonId == ID_TAB_BUTTON2 && g_activeTab == 2) isActiveTab = true;
            else if (buttonId == ID_TAB_BUTTON3 && g_activeTab == 3) isActiveTab = true;
            DrawOwnerDrawButton(hdc, hwnd, isPressed, buffer, isActiveTab);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}
LRESULT HandleOwnerDrawCheckbox(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_LBUTTONDOWN: {
            InvalidateRect(hwnd, NULL, TRUE);
            SetCapture(hwnd);
            break;
        }
        case WM_LBUTTONUP: {
            InvalidateRect(hwnd, NULL, TRUE);
            ReleaseCapture();
            ToggleHideOverlay();
            break;
        }
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);
            wchar_t buffer[256];
            GetWindowText(hwnd, buffer, 256);
            std::wstring text = buffer;
            if (!text.empty() && text[0] == L' ')
                text = text.substr(1);
            DrawOwnerDrawCheckbox(hdc, hwnd, g_hideOverlay, text);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}
LRESULT HandleOwnerDrawListBox(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam, int listType) {
    switch (msg) {
        case WM_SETCURSOR:
            SetCursor(LoadCursor(NULL, IDC_ARROW));
            return TRUE;
        case WM_SETFOCUS:
            InvalidateRect(hwnd, NULL, TRUE);
            return 0;
        case WM_LBUTTONDOWN: {
            SetFocus(hwnd);
            RECT rc;
            GetClientRect(hwnd, &rc);
            int yPos = GET_Y_LPARAM(lParam);
            int xPos = GET_X_LPARAM(lParam);
            if (xPos < 0 || xPos > (rc.right - rc.left) || yPos < 0 || yPos > (rc.bottom - rc.top)) return 0;
            int visibleItems = (listType == ID_IMAGE_LIST) ? VISIBLE_ITEMS_IMAGES : VISIBLE_ITEMS_MONITORS;
            int startIdx = (listType == ID_IMAGE_LIST) ? g_imageListScrollPos : g_monitorListScrollPos;
            int itemIndex = startIdx + (yPos - 2) / ITEM_HEIGHT;
            int itemCount = (listType == ID_IMAGE_LIST) ? (int)g_imageFiles.size() : (int)g_monitors.size();
            if (itemIndex >= 0 && itemIndex < itemCount && itemIndex >= startIdx && itemIndex < startIdx + visibleItems) {
                if (listType == ID_IMAGE_LIST) {
                    g_imageListSelected = itemIndex;
                } else if (listType == ID_MONITOR_LIST) {
                    g_monitorListSelected = itemIndex;
                }
                InvalidateRect(hwnd, NULL, TRUE);
                UpdateWindow(hwnd);
            }
            return 0;
        }
        case WM_LBUTTONDBLCLK: {
            RECT rc;
            GetClientRect(hwnd, &rc);
            int yPos = GET_Y_LPARAM(lParam);
            int xPos = GET_X_LPARAM(lParam);
            if (xPos < 0 || xPos > (rc.right - rc.left) || yPos < 0 || yPos > (rc.bottom - rc.top)) return 0;
            int visibleItems = (listType == ID_IMAGE_LIST) ? VISIBLE_ITEMS_IMAGES : VISIBLE_ITEMS_MONITORS;
            int startIdx = (listType == ID_IMAGE_LIST) ? g_imageListScrollPos : g_monitorListScrollPos;
            int itemIndex = startIdx + (yPos - 2) / ITEM_HEIGHT;
            int itemCount = (listType == ID_IMAGE_LIST) ? (int)g_imageFiles.size() : (int)g_monitors.size();
            if (itemIndex >= 0 && itemIndex < itemCount && itemIndex >= startIdx && itemIndex < startIdx + visibleItems) {
                if (listType == ID_IMAGE_LIST) {
                    g_imageListSelected = itemIndex;
                    CreateOverlayWindow(g_imageFiles[itemIndex].path);
                } else if (listType == ID_MONITOR_LIST) {
                    g_monitorListSelected = itemIndex;
                    g_selectedMonitor = itemIndex;
                    CorrectOffsetValues();
                    SaveAllSettingsToIni();
                    UpdateOffsetBoundsDisplay();
                    if (g_currentOverlay) {
                        UpdateOverlayPosition();
                        g_renderRequestPending = true;
                        g_currentMonitor = g_selectedMonitor;
                    }
                }
                InvalidateRect(hwnd, NULL, TRUE);
                UpdateWindow(hwnd);
            }
            return 0;
        }
        case WM_RBUTTONDOWN: {
            if (listType == ID_IMAGE_LIST) {
                RECT rc;
                GetClientRect(hwnd, &rc);
                int yPos = GET_Y_LPARAM(lParam);
                int xPos = GET_X_LPARAM(lParam);
                if (xPos < 0 || xPos > (rc.right - rc.left) || yPos < 0 || yPos > (rc.bottom - rc.top)) return 0;
                int startIdx = g_imageListScrollPos;
                int itemIndex = startIdx + (yPos - 2) / ITEM_HEIGHT;
                if (itemIndex >= 0 && itemIndex < (int)g_imageFiles.size() && itemIndex >= startIdx && itemIndex < startIdx + VISIBLE_ITEMS_IMAGES) {
                    g_imageListSelected = itemIndex;
                    InvalidateRect(hwnd, NULL, TRUE);
                    UpdateWindow(hwnd);
                    POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
                    ClientToScreen(hwnd, &pt);
                    HMENU hMenu = CreatePopupMenu();
                    CreateOwnerDrawMenu(hMenu, 0, ID_CONTEXT_OPEN, L"Open File Location");
                    CreateOwnerDrawMenu(hMenu, 1, ID_CONTEXT_REMOVE, L"Remove From Image List");
                    SetForegroundWindow(g_hwnd);
                    TrackPopupMenu(hMenu, TPM_LEFTALIGN | TPM_LEFTBUTTON, pt.x, pt.y, 0, g_hwnd, NULL);
                    DestroyMenu(hMenu);
                }
            }
            return 0;
        }
        case WM_MOUSEWHEEL: {
            int delta = GET_WHEEL_DELTA_WPARAM(wParam);
            int& scrollPos = (listType == ID_IMAGE_LIST) ? g_imageListScrollPos : g_monitorListScrollPos;
            int maxItems = (listType == ID_IMAGE_LIST) ? (int)g_imageFiles.size() : (int)g_monitors.size();
            int visibleItems = (listType == ID_IMAGE_LIST) ? VISIBLE_ITEMS_IMAGES : VISIBLE_ITEMS_MONITORS;
            if (delta > 0) {
                scrollPos = std::max(0, scrollPos - 3);
            } else {
                int maxScroll = std::max(0, maxItems - visibleItems);
                scrollPos = std::min(maxScroll, scrollPos + 3);
            }
            InvalidateRect(hwnd, NULL, TRUE);
            UpdateWindow(hwnd);
            return 0;
        }
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);
            DrawOwnerDrawListBox(hdc, hwnd, listType);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}
void ApplyOffsetFromField(EditField* field, bool isX) {
    int value = 0;
    try {
        if (!field->text.empty()) {
            value = std::stoi(field->text);
        }
    } catch (...) {
        value = 0;
    }
    if (value > 9999) value = 9999;
    if (value < -9999) value = -9999;
    if (isX) {
        g_offsetX = value;
    } else {
        g_offsetY = value;
    }
    CorrectOffsetValues();
}
void ApplyScaleFromField(EditField* field) {
    float value = 1.0f;
    try {
        if (!field->text.empty()) {
            value = std::stof(field->text);
        }
    } catch (...) {
        value = 1.0f;
    }
    if (value < 0.0f) value = 0.0f;
    if (value > 2.0f) value = 2.0f;
    g_imageScale = value;
    CorrectScaleValue();
}
void ResetHotkeyField(int fieldId, wchar_t defaultKey, wchar_t& currentKey, wchar_t& lastValidKey, EditField& field, bool showConflictMessage) {
    std::wstring conflictAction;
    if (CheckHotkeyConflict(defaultKey, fieldId, conflictAction)) {
        if (showConflictMessage) {
            std::wstring message = L"Failed to reset hotkey. Key '" + std::wstring(1, defaultKey) + L"' is already used for: " + conflictAction;
            ShowOwnerDrawMessageBox(g_hwnd, message);
        }
        field.text = lastValidKey ? std::wstring(1, lastValidKey) : L"";
        currentKey = lastValidKey;
    } else {
        currentKey = defaultKey;
        lastValidKey = defaultKey;
        field.text = std::wstring(1, defaultKey);
    }
    field.cursorPos = field.text.length();
}
void ApplyHotkeyFromField(EditField* field, int id) {
    if (field->text.empty()) {
        switch (id) {
            case 1: g_toggleHotkeyKey = 0; break;
            case 2: g_quitHotkeyKey = 0; break;
            case 3: g_settingsHotkeyKey = 0; break;
        }
    } else {
        wchar_t key = std::towupper(field->text[0]);
        if (IsValidHotkeyChar(key)) {
            std::wstring conflictAction;
            if (CheckHotkeyConflict(key, id, conflictAction)) {
                std::wstring message = L"Failed to change hotkey. Key '" + std::wstring(1, key) + L"' is already used for: " + conflictAction;
                ShowOwnerDrawMessageBox(g_hwnd, message);
                switch (id) {
                    case 1: 
                        field->text = g_lastValidToggleKey ? std::wstring(1, g_lastValidToggleKey) : L"";
                        g_toggleHotkeyKey = g_lastValidToggleKey;
                        break;
                    case 2: 
                        field->text = g_lastValidQuitKey ? std::wstring(1, g_lastValidQuitKey) : L"";
                        g_quitHotkeyKey = g_lastValidQuitKey;
                        break;
                    case 3: 
                        field->text = g_lastValidSettingsKey ? std::wstring(1, g_lastValidSettingsKey) : L"";
                        g_settingsHotkeyKey = g_lastValidSettingsKey;
                        break;
                }
            } else {
                switch (id) {
                    case 1: 
                        g_toggleHotkeyKey = key;
                        g_lastValidToggleKey = key;
                        break;
                    case 2: 
                        g_quitHotkeyKey = key;
                        g_lastValidQuitKey = key;
                        break;
                    case 3: 
                        g_settingsHotkeyKey = key;
                        g_lastValidSettingsKey = key;
                        break;
                }
            }
        } else {
            switch (id) {
                case 1: 
                    field->text = g_lastValidToggleKey ? std::wstring(1, g_lastValidToggleKey) : L"";
                    g_toggleHotkeyKey = g_lastValidToggleKey;
                    break;
                case 2: 
                    field->text = g_lastValidQuitKey ? std::wstring(1, g_lastValidQuitKey) : L"";
                    g_quitHotkeyKey = g_lastValidQuitKey;
                    break;
                case 3: 
                    field->text = g_lastValidSettingsKey ? std::wstring(1, g_lastValidSettingsKey) : L"";
                    g_settingsHotkeyKey = g_lastValidSettingsKey;
                    break;
            }
        }
    }
    field->cursorPos = field->text.length();
}
void UpdateEditFieldText(EditField* field, const std::wstring& newText) {
    field->text = newText;
    field->cursorPos = field->text.length();
}
LRESULT HandleOwnerDrawEdit(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam, EditField* field) {
    switch (msg) {
        case WM_SETFOCUS:
            field->hasFocus = true;
            InvalidateRect(hwnd, NULL, TRUE);
            return 0;
        case WM_KILLFOCUS:
            field->hasFocus = false;
            InvalidateRect(hwnd, NULL, TRUE);
            switch (field->id) {
                case ID_OFFSET_X_DISPLAY:
                    ApplyOffsetFromField(field, true);
                    break;
                case ID_OFFSET_Y_DISPLAY:
                    ApplyOffsetFromField(field, false);
                    break;
                case ID_SIZE_DISPLAY:
                    ApplyScaleFromField(field);
                    break;
                case ID_HOTKEY_TOGGLE_DISPLAY:
                    ApplyHotkeyFromField(field, 1);
                    break;
                case ID_HOTKEY_SETTINGS_DISPLAY:
                    ApplyHotkeyFromField(field, 3);
                    break;
                case ID_HOTKEY_QUIT_DISPLAY:
                    ApplyHotkeyFromField(field, 2);
                    break;
            }
            RegisterHotkeys();
            SaveAllSettingsToIni();
            if (!g_lastDisplayedImage.empty()) {
                CreateOverlayWindow(g_lastDisplayedImage);
            }
            return 0;
        case WM_KEYDOWN:
            switch (wParam) {
                case VK_LEFT:
                    if (field->cursorPos > 0) {
                        field->cursorPos--;
                        InvalidateRect(hwnd, NULL, TRUE);
                    }
                    return 0;
                case VK_RIGHT:
                    if (field->cursorPos < field->text.length()) {
                        field->cursorPos++;
                        InvalidateRect(hwnd, NULL, TRUE);
                    }
                    return 0;
                case VK_BACK:
                    if (field->cursorPos > 0) {
                        field->text.erase(field->cursorPos - 1, 1);
                        field->cursorPos--;
                        InvalidateRect(hwnd, NULL, TRUE);
                    }
                    return 0;
                case VK_DELETE:
                    if (field->cursorPos < field->text.length()) {
                        field->text.erase(field->cursorPos, 1);
                        InvalidateRect(hwnd, NULL, TRUE);
                    }
                    return 0;
                case VK_ESCAPE:
                    SetFocus(GetParent(hwnd));
                    return 0;
                case VK_RETURN:
                    SetFocus(GetParent(hwnd));
                    return 0;
            }
            break;
        case WM_CHAR: {
            wchar_t ch = (wchar_t)wParam;
            if (ch == VK_RETURN || ch == VK_ESCAPE) {
                return 0;
            }
            if (field->id == ID_HOTKEY_TOGGLE_DISPLAY || field->id == ID_HOTKEY_SETTINGS_DISPLAY || field->id == ID_HOTKEY_QUIT_DISPLAY) {
                if (IsValidHotkeyChar(ch)) {
                    field->text = std::wstring(1, std::towupper(ch));
                    field->cursorPos = field->text.length();
                    InvalidateRect(hwnd, NULL, TRUE);
                } else if (ch == VK_BACK && !field->text.empty()) {
                    field->text.clear();
                    field->cursorPos = 0;
                    InvalidateRect(hwnd, NULL, TRUE);
                }
                return 0;
            } else if (field->id == ID_SIZE_DISPLAY) {
                if ((ch >= L'0' && ch <= L'9') || ch == L'.') {
                    if (ch == L'.' && field->text.find(L'.') != std::wstring::npos) {
                        return 0;
                    }
                    if (field->text.length() < 4) {
                        field->text.insert(field->cursorPos, 1, ch);
                        field->cursorPos++;
                        InvalidateRect(hwnd, NULL, TRUE);
                    }
                }
                return 0;
            } else {
                if ((ch >= L'0' && ch <= L'9') || ch == L'-') {
                    if (ch == L'-') {
                        if (field->cursorPos != 0 || field->text.find(L'-') != std::wstring::npos) {
                            return 0;
                        }
                    }
                    if (field->text.length() < 5) {
                        field->text.insert(field->cursorPos, 1, ch);
                        field->cursorPos++;
                        InvalidateRect(hwnd, NULL, TRUE);
                    }
                }
                return 0;
            }
            break;
        }
        case WM_LBUTTONDOWN:
            SetFocus(hwnd);
            return 0;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);
            DrawOwnerDrawEdit(hdc, hwnd, field, g_darkTheme);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_SETCURSOR:
            SetCursor(LoadCursor(NULL, IDC_IBEAM));
            return TRUE;
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}
WNDPROC g_oldImageListProc = NULL;
WNDPROC g_oldMonitorListProc = NULL;
WNDPROC g_oldOffsetXProc = NULL;
WNDPROC g_oldOffsetYProc = NULL;
WNDPROC g_oldSizeProc = NULL;
WNDPROC g_oldHotkeyToggleProc = NULL;
WNDPROC g_oldHotkeySettingsProc = NULL;
WNDPROC g_oldHotkeyQuitProc = NULL;
LRESULT CALLBACK ImageListSubclassProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    return HandleOwnerDrawListBox(hwnd, msg, wParam, lParam, ID_IMAGE_LIST);
}
LRESULT CALLBACK MonitorListSubclassProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    return HandleOwnerDrawListBox(hwnd, msg, wParam, lParam, ID_MONITOR_LIST);
}
LRESULT CALLBACK OffsetXSubclassProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    return HandleOwnerDrawEdit(hwnd, msg, wParam, lParam, &g_offsetXField);
}
LRESULT CALLBACK OffsetYSubclassProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    return HandleOwnerDrawEdit(hwnd, msg, wParam, lParam, &g_offsetYField);
}
LRESULT CALLBACK SizeSubclassProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    return HandleOwnerDrawEdit(hwnd, msg, wParam, lParam, &g_sizeField);
}
LRESULT CALLBACK HotkeyToggleSubclassProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    return HandleOwnerDrawEdit(hwnd, msg, wParam, lParam, &g_hotkeyToggleField);
}
LRESULT CALLBACK HotkeySettingsSubclassProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    return HandleOwnerDrawEdit(hwnd, msg, wParam, lParam, &g_hotkeySettingsField);
}
LRESULT CALLBACK HotkeyQuitSubclassProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    return HandleOwnerDrawEdit(hwnd, msg, wParam, lParam, &g_hotkeyQuitField);
}
void AddImageToList(const std::wstring& filePath, bool saveToIni) {
    if (!IsSupportedImageFormat(filePath)) {
        ShowOwnerDrawMessageBox(g_hwnd, L"Invalid format. Supported formats: .png, .jpg, .jpeg, .gif");
        return;
    }
    DWORD attrs = GetFileAttributesW(filePath.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES) {
        ShowOwnerDrawMessageBox(g_hwnd, L"Failed to load image");
        return;
    }
    for (const auto& image : g_imageFiles) {
        if (image.path == filePath) return;
    }
    ImageFile newImage;
    newImage.path = filePath;
    size_t lastSlash = filePath.find_last_of(L"\\/");
    newImage.filename = (lastSlash != std::wstring::npos) ? filePath.substr(lastSlash + 1) : filePath;
    g_imageFiles.push_back(newImage);
    if (g_hImageList) InvalidateRect(g_hImageList, NULL, TRUE);
    if (saveToIni) SaveAllSettingsToIni();
}
void LoadAllSettingsFromIni() {
    std::wstring iniPath = GetIniFilePath();
    DWORD attrs = GetFileAttributesW(iniPath.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES) {
        int screenWidth = GetSystemMetrics(SM_CXSCREEN);
        int screenHeight = GetSystemMetrics(SM_CYSCREEN);
        g_windowX = (screenWidth - 300) / 2;
        g_windowY = (screenHeight - 300) / 2;
        g_toggleHotkeyKey = DEFAULT_TOGGLE_KEY;
        g_quitHotkeyKey = DEFAULT_QUIT_KEY;
        g_settingsHotkeyKey = DEFAULT_SETTINGS_KEY;
        g_darkTheme = false;
        g_lastValidToggleKey = DEFAULT_TOGGLE_KEY;
        g_lastValidQuitKey = DEFAULT_QUIT_KEY;
        g_lastValidSettingsKey = DEFAULT_SETTINGS_KEY;
        return;
    }
    wchar_t themeStr[10] = {0};
    GetPrivateProfileStringW(L"Window", L"Theme", L"Light", themeStr, 10, iniPath.c_str());
    g_darkTheme = (wcscmp(themeStr, L"Dark") == 0);
    wchar_t countStr[10] = {0};
    GetPrivateProfileStringW(L"Images List", L"Count", L"0", countStr, 10, iniPath.c_str());
    int count = _wtoi(countStr);
    for (int i = 0; i < count; i++) {
        std::wstring key = L"Image" + std::to_wstring(i);
        wchar_t path[MAX_PATH] = {0};
        GetPrivateProfileStringW(L"Images List", key.c_str(), L"", path, MAX_PATH, iniPath.c_str());
        if (wcslen(path) > 0 && GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES) {
            AddImageToList(path, false);
        }
    }
    wchar_t lastDisplayed[MAX_PATH] = {0};
    GetPrivateProfileStringW(L"Images List", L"LastDisplayed", L"", lastDisplayed, MAX_PATH, iniPath.c_str());
    if (wcslen(lastDisplayed) > 0) g_lastDisplayedImage = lastDisplayed;
    wchar_t monitorStr[10] = {0};
    GetPrivateProfileStringW(L"Location & Size", L"SelectedMonitor", L"0", monitorStr, 10, iniPath.c_str());
    g_selectedMonitor = _wtoi(monitorStr);
    wchar_t offsetXStr[32] = {0}, offsetYStr[32] = {0};
    GetPrivateProfileStringW(L"Location & Size", L"OffsetX", L"0", offsetXStr, 32, iniPath.c_str());
    GetPrivateProfileStringW(L"Location & Size", L"OffsetY", L"0", offsetYStr, 32, iniPath.c_str());
    g_offsetX = _wtoi(offsetXStr);
    g_offsetY = _wtoi(offsetYStr);
    UpdateEditFieldText(&g_offsetXField, std::to_wstring(g_offsetX));
    UpdateEditFieldText(&g_offsetYField, std::to_wstring(g_offsetY));
    wchar_t scaleStr[32] = {0};
    GetPrivateProfileStringW(L"Location & Size", L"ImageScale", L"1.0", scaleStr, 32, iniPath.c_str());
    g_imageScale = (float)_wtof(scaleStr);
    std::wstringstream ss;
    ss << std::fixed << std::setprecision(2) << g_imageScale;
    UpdateEditFieldText(&g_sizeField, ss.str());
    wchar_t toggleKeyStr[10] = {0};
    GetPrivateProfileStringW(L"Hotkeys", L"ToggleOverlay", std::wstring(1, DEFAULT_TOGGLE_KEY).c_str(), toggleKeyStr, 10, iniPath.c_str());
    if (wcslen(toggleKeyStr) > 0) {
        wchar_t key = std::towupper(toggleKeyStr[0]);
        if (IsValidHotkeyChar(key)) {
            g_toggleHotkeyKey = key;
            g_lastValidToggleKey = key;
            UpdateEditFieldText(&g_hotkeyToggleField, std::wstring(1, key));
        } else {
            g_toggleHotkeyKey = DEFAULT_TOGGLE_KEY;
            g_lastValidToggleKey = DEFAULT_TOGGLE_KEY;
            UpdateEditFieldText(&g_hotkeyToggleField, std::wstring(1, DEFAULT_TOGGLE_KEY));
            WritePrivateProfileStringW(L"Hotkeys", L"ToggleOverlay", std::wstring(1, DEFAULT_TOGGLE_KEY).c_str(), iniPath.c_str());
        }
    }
    wchar_t settingsKeyStr[10] = {0};
    GetPrivateProfileStringW(L"Hotkeys", L"Settings", std::wstring(1, DEFAULT_SETTINGS_KEY).c_str(), settingsKeyStr, 10, iniPath.c_str());
    if (wcslen(settingsKeyStr) > 0) {
        wchar_t key = std::towupper(settingsKeyStr[0]);
        if (IsValidHotkeyChar(key)) {
            g_settingsHotkeyKey = key;
            g_lastValidSettingsKey = key;
            UpdateEditFieldText(&g_hotkeySettingsField, std::wstring(1, key));
        } else {
            g_settingsHotkeyKey = DEFAULT_SETTINGS_KEY;
            g_lastValidSettingsKey = DEFAULT_SETTINGS_KEY;
            UpdateEditFieldText(&g_hotkeySettingsField, std::wstring(1, DEFAULT_SETTINGS_KEY));
            WritePrivateProfileStringW(L"Hotkeys", L"Settings", std::wstring(1, DEFAULT_SETTINGS_KEY).c_str(), iniPath.c_str());
        }
    }
    wchar_t quitKeyStr[10] = {0};
    GetPrivateProfileStringW(L"Hotkeys", L"Quit", std::wstring(1, DEFAULT_QUIT_KEY).c_str(), quitKeyStr, 10, iniPath.c_str());
    if (wcslen(quitKeyStr) > 0) {
        wchar_t key = std::towupper(quitKeyStr[0]);
        if (IsValidHotkeyChar(key)) {
            g_quitHotkeyKey = key;
            g_lastValidQuitKey = key;
            UpdateEditFieldText(&g_hotkeyQuitField, std::wstring(1, key));
        } else {
            g_quitHotkeyKey = DEFAULT_QUIT_KEY;
            g_lastValidQuitKey = DEFAULT_QUIT_KEY;
            UpdateEditFieldText(&g_hotkeyQuitField, std::wstring(1, DEFAULT_QUIT_KEY));
            WritePrivateProfileStringW(L"Hotkeys", L"Quit", std::wstring(1, DEFAULT_QUIT_KEY).c_str(), iniPath.c_str());
        }
    }
    wchar_t windowXStr[32] = {0}, windowYStr[32] = {0};
    GetPrivateProfileStringW(L"Window", L"WindowX", L"-32768", windowXStr, 32, iniPath.c_str());
    GetPrivateProfileStringW(L"Window", L"WindowY", L"-32768", windowYStr, 32, iniPath.c_str());
    g_windowX = _wtoi(windowXStr);
    g_windowY = _wtoi(windowYStr);
    if (g_windowX == -32768 || g_windowY == -32768) {
        int screenWidth = GetSystemMetrics(SM_CXSCREEN);
        int screenHeight = GetSystemMetrics(SM_CYSCREEN);
        g_windowX = (screenWidth - 300) / 2;
        g_windowY = (screenHeight - 300) / 2;
    }
}
void SaveAllSettingsToIni() {
    if (!g_hwnd) return;
    std::wstring iniPath = GetIniFilePath();
    WritePrivateProfileStringW(L"Window", L"Theme", g_darkTheme ? L"Dark" : L"Light", iniPath.c_str());
    WritePrivateProfileStringW(L"Images List", L"Count", std::to_wstring(g_imageFiles.size()).c_str(), iniPath.c_str());
    for (size_t i = 0; i < g_imageFiles.size(); i++) {
        std::wstring key = L"Image" + std::to_wstring(i);
        WritePrivateProfileStringW(L"Images List", key.c_str(), g_imageFiles[i].path.c_str(), iniPath.c_str());
    }
    for (size_t i = g_imageFiles.size(); i < 100; i++) {
        std::wstring key = L"Image" + std::to_wstring(i);
        WritePrivateProfileStringW(L"Images List", key.c_str(), NULL, iniPath.c_str());
    }
    WritePrivateProfileStringW(L"Images List", L"LastDisplayed", g_lastDisplayedImage.c_str(), iniPath.c_str());
    WritePrivateProfileStringW(L"Location & Size", L"SelectedMonitor", std::to_wstring(g_selectedMonitor).c_str(), iniPath.c_str());
    WritePrivateProfileStringW(L"Location & Size", L"OffsetX", std::to_wstring(g_offsetX).c_str(), iniPath.c_str());
    WritePrivateProfileStringW(L"Location & Size", L"OffsetY", std::to_wstring(g_offsetY).c_str(), iniPath.c_str());
    wchar_t scaleStr[32] = {0};
    swprintf_s(scaleStr, 32, L"%.2f", g_imageScale);
    WritePrivateProfileStringW(L"Location & Size", L"ImageScale", scaleStr, iniPath.c_str());
    wchar_t toggleKey[2] = {g_toggleHotkeyKey, 0};
    WritePrivateProfileStringW(L"Hotkeys", L"ToggleOverlay", g_toggleHotkeyKey ? toggleKey : L"", iniPath.c_str());
    wchar_t settingsKey[2] = {g_settingsHotkeyKey, 0};
    WritePrivateProfileStringW(L"Hotkeys", L"Settings", g_settingsHotkeyKey ? settingsKey : L"", iniPath.c_str());
    wchar_t quitKey[2] = {g_quitHotkeyKey, 0};
    WritePrivateProfileStringW(L"Hotkeys", L"Quit", g_quitHotkeyKey ? quitKey : L"", iniPath.c_str());
    if (g_windowVisible) {
        RECT windowRect = {0};
        GetWindowRect(g_hwnd, &windowRect);
        WritePrivateProfileStringW(L"Window", L"WindowX", std::to_wstring(windowRect.left).c_str(), iniPath.c_str());
        WritePrivateProfileStringW(L"Window", L"WindowY", std::to_wstring(windowRect.top).c_str(), iniPath.c_str());
    }
}
void CorrectOffsetValues() {
    if (g_selectedMonitor >= 0 && g_selectedMonitor < static_cast<int>(g_monitors.size())) {
        const auto& monitor = g_monitors[g_selectedMonitor];
        int screenWidth = monitor.rect.right - monitor.rect.left;
        int screenHeight = monitor.rect.bottom - monitor.rect.top;
        int maxOffsetX = screenWidth / 2;
        int maxOffsetY = screenHeight / 2;
        if (g_offsetX > maxOffsetX) g_offsetX = maxOffsetX;
        if (g_offsetX < -maxOffsetX) g_offsetX = -maxOffsetX;
        if (g_offsetY > maxOffsetY) g_offsetY = maxOffsetY;
        if (g_offsetY < -maxOffsetY) g_offsetY = -maxOffsetY;
        UpdateEditFieldText(&g_offsetXField, std::to_wstring(g_offsetX));
        UpdateEditFieldText(&g_offsetYField, std::to_wstring(g_offsetY));
        if (g_hOffsetXDisplay) InvalidateRect(g_hOffsetXDisplay, NULL, TRUE);
        if (g_hOffsetYDisplay) InvalidateRect(g_hOffsetYDisplay, NULL, TRUE);
        SaveAllSettingsToIni();
    }
    if (g_currentOverlay) {
        UpdateOverlayPosition();
        g_renderRequestPending = true;
    }
}
void CorrectScaleValue() {
    if (g_imageScale < 0.0f) { g_imageScale = 0.0f; }
    if (g_imageScale > 2.0f) { g_imageScale = 2.0f; }
    std::wstringstream ss;
    ss << std::fixed << std::setprecision(2) << g_imageScale;
    UpdateEditFieldText(&g_sizeField, ss.str());
    if (g_hSizeDisplay) InvalidateRect(g_hSizeDisplay, NULL, TRUE);
    UpdateScaleControls();
    SaveAllSettingsToIni();
    if (g_currentOverlay) {
        g_renderRequestPending = true;
    }
}
void SafeGdiplusShutdown() {
    if (g_gdiplusToken) {
        GdiplusShutdown(g_gdiplusToken);
        g_gdiplusToken = 0;
    }
}
void UpdateOverlayPosition() {
    if (!g_currentOverlay) return;
    int windowWidth = 2 * g_originalImageWidth;
    int windowHeight = 2 * g_originalImageHeight;
    int x = 0, y = 0;
    if (g_selectedMonitor >= 0 && g_selectedMonitor < static_cast<int>(g_monitors.size())) {
        const auto& monitor = g_monitors[g_selectedMonitor];
        int screenWidth = monitor.rect.right - monitor.rect.left;
        int screenHeight = monitor.rect.bottom - monitor.rect.top;
        x = monitor.rect.left + (screenWidth - windowWidth) / 2 + g_offsetX;
        y = monitor.rect.top + (screenHeight - windowHeight) / 2 + g_offsetY;
    } else {
        int screenWidth = GetSystemMetrics(SM_CXSCREEN);
        int screenHeight = GetSystemMetrics(SM_CYSCREEN);
        x = (screenWidth - windowWidth) / 2 + g_offsetX;
        y = (screenHeight - windowHeight) / 2 + g_offsetY;
    }
    SetWindowPos(g_currentOverlay, NULL, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}
void RenderOverlayInThread() {
    while (!g_renderThreadShouldExit) {
        if (g_renderRequestPending) {
            g_renderRequestPending = false;
            std::lock_guard<std::mutex> lock(g_renderMutex);
            if (!g_currentOverlay) continue;
            RECT windowRect;
            GetWindowRect(g_currentOverlay, &windowRect);
            int windowWidth = windowRect.right - windowRect.left;
            int windowHeight = windowRect.bottom - windowRect.top;
            int drawWidth = (int)(g_originalImageWidth * g_imageScale);
            int drawHeight = (int)(g_originalImageHeight * g_imageScale);
            int drawX = (windowWidth - drawWidth) / 2;
            int drawY = (windowHeight - drawHeight) / 2;
            HDC hdcScreen = GetDC(NULL);
            if (!hdcScreen) continue;
            HDC hdcMem = CreateCompatibleDC(hdcScreen);
            if (!hdcMem) {
                ReleaseDC(NULL, hdcScreen);
                continue;
            }
            HBITMAP hBitmap = CreateCompatibleBitmap(hdcScreen, windowWidth, windowHeight);
            if (!hBitmap) {
                DeleteDC(hdcMem);
                ReleaseDC(NULL, hdcScreen);
                continue;
            }
            HBITMAP hOldBitmap = (HBITMAP)SelectObject(hdcMem, hBitmap);
            Graphics graphics(hdcMem);
            graphics.SetSmoothingMode(SmoothingModeHighQuality);
            graphics.SetInterpolationMode(InterpolationModeHighQualityBicubic);
            SolidBrush clearBrush(Color(0, 0, 0, 0));
            graphics.FillRectangle(&clearBrush, 0, 0, windowWidth, windowHeight);
            bool success = false;
            if (g_animatedGIF && g_animatedGIF->IsInitialized()) {
                success = g_animatedGIF->GetCurrentFrame(&graphics, drawX, drawY, drawWidth, drawHeight);
            } else if (g_cachedBitmap) {
                Rect destRect(drawX, drawY, drawWidth, drawHeight);
                graphics.DrawImage(g_cachedBitmap.get(), destRect);
                success = true;
            }
            if (success) {
                BLENDFUNCTION blend = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
                POINT ptPos = { windowRect.left, windowRect.top };
                SIZE sizeWnd = { windowWidth, windowHeight };
                POINT ptSrc = { 0, 0 };
                UpdateLayeredWindow(g_currentOverlay, hdcScreen, &ptPos, &sizeWnd, hdcMem, &ptSrc, 0, &blend, ULW_ALPHA);
            }
            SelectObject(hdcMem, hOldBitmap);
            DeleteObject(hBitmap);
            DeleteDC(hdcMem);
            ReleaseDC(NULL, hdcScreen);
        }
        Sleep(1);
    }
}
void StartRenderThread() {
    if (!g_renderThreadRunning) {
        g_renderThreadShouldExit = false;
        g_renderThreadRunning = true;
        g_renderThread = std::thread(RenderOverlayInThread);
    }
}
void StopRenderThread() {
    if (g_renderThreadRunning) {
        g_renderThreadShouldExit = true;
        if (g_renderThread.joinable()) {
            g_renderThread.join();
        }
        g_renderThreadRunning = false;
    }
}
void RenderOverlay() {
    g_renderRequestPending = true;
}
LRESULT CALLBACK OverlayProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    switch (uMsg) {
        case WM_TIMER:
            if (wParam == ID_ANIMATION_TIMER) {
                if (g_animatedGIF && g_animatedGIF->IsInitialized()) {
                    g_animatedGIF->NextFrame();
                    RenderOverlay();
                }
            }
            return 0;
        case WM_DESTROY:
            if (g_animatedGIF) {
                g_animatedGIF->StopAnimation();
            }
            return 0;
    }
    return DefWindowProc(hwnd, uMsg, wParam, lParam);
}
void CreateOverlayWindow(const std::wstring& imagePath) {
    if (imagePath.empty()) return;
    bool needNewWindow = false;
    bool isGif = false;
    size_t dotPos = imagePath.find_last_of(L".");
    if (dotPos != std::wstring::npos) {
        std::wstring ext = imagePath.substr(dotPos + 1);
        for (wchar_t& c : ext) c = towlower(c);
        isGif = (ext == L"gif");
    }
    if (isGif) {
        g_animatedGIF.reset(new AnimatedGIF());
        if (g_animatedGIF->Load(imagePath)) {
            g_originalImageWidth = g_animatedGIF->GetWidth();
            g_originalImageHeight = g_animatedGIF->GetHeight();
            g_lastDisplayedImage = imagePath;
        } else {
            g_animatedGIF.reset();
            return;
        }
    } else {
        g_animatedGIF.reset();
        if (!g_cachedBitmap || g_lastDisplayedImage != imagePath) {
            g_cachedBitmap.reset(Bitmap::FromFile(imagePath.c_str()));
            if (!g_cachedBitmap || g_cachedBitmap->GetLastStatus() != Ok) {
                g_cachedBitmap.reset();
                return;
            }
            g_originalImageWidth = g_cachedBitmap->GetWidth();
            g_originalImageHeight = g_cachedBitmap->GetHeight();
            g_lastDisplayedImage = imagePath;
        }
    }
    int windowWidth = 2 * g_originalImageWidth;
    int windowHeight = 2 * g_originalImageHeight;
    if (!g_currentOverlay) {
        needNewWindow = true;
    } else if (g_selectedMonitor != g_currentMonitor) {
        UpdateOverlayPosition();
        RenderOverlay();
        g_currentMonitor = g_selectedMonitor;
        SaveAllSettingsToIni();
        return;
    } else {
        RECT currentRect;
        GetWindowRect(g_currentOverlay, &currentRect);
        int currentWidth = currentRect.right - currentRect.left;
        int currentHeight = currentRect.bottom - currentRect.top;
        if (currentWidth != windowWidth || currentHeight != windowHeight) {
            SetWindowPos(g_currentOverlay, NULL, 0, 0, windowWidth, windowHeight, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
            UpdateOverlayPosition();
        }
    }
    if (needNewWindow) {
        int x = 0, y = 0;
        if (g_selectedMonitor >= 0 && g_selectedMonitor < static_cast<int>(g_monitors.size())) {
            const auto& monitor = g_monitors[g_selectedMonitor];
            int screenWidth = monitor.rect.right - monitor.rect.left;
            int screenHeight = monitor.rect.bottom - monitor.rect.top;
            x = monitor.rect.left + (screenWidth - windowWidth) / 2 + g_offsetX;
            y = monitor.rect.top + (screenHeight - windowHeight) / 2 + g_offsetY;
            g_currentMonitor = g_selectedMonitor;
        } else {
            int screenWidth = GetSystemMetrics(SM_CXSCREEN);
            int screenHeight = GetSystemMetrics(SM_CYSCREEN);
            x = (screenWidth - windowWidth) / 2 + g_offsetX;
            y = (screenHeight - windowHeight) / 2 + g_offsetY;
            g_currentMonitor = 0;
        }
        static bool registered = false;
        if (!registered) {
            WNDCLASS wc = {};
            wc.lpfnWndProc = OverlayProc;
            wc.hInstance = GetModuleHandle(NULL);
            wc.lpszClassName = L"CustomOverlayWindow";
            RegisterClass(&wc);
            registered = true;
        }
        g_currentOverlay = CreateWindowEx(WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, L"CustomOverlayWindow", NULL, WS_POPUP, x, y, windowWidth, windowHeight, NULL, NULL, GetModuleHandle(NULL), NULL);
        if (!g_currentOverlay) return;
        StartRenderThread();
        if (!g_hideOverlay) {
            ShowWindow(g_currentOverlay, SW_SHOWNA);
            SetWindowPos(g_currentOverlay, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        }
        if (g_animatedGIF && g_animatedGIF->IsInitialized() && g_animatedGIF->IsAnimated()) {
            g_animatedGIF->StartAnimation(g_currentOverlay);
        }
        RenderOverlay();
        SaveAllSettingsToIni();
    } else {
        if (g_animatedGIF && g_animatedGIF->IsInitialized() && g_animatedGIF->IsAnimated()) {
            g_animatedGIF->StartAnimation(g_currentOverlay);
        }
        RenderOverlay();
        SaveAllSettingsToIni();
    }
}
void RestoreLastOverlay() {
    if (!g_lastDisplayedImage.empty()) {
        DWORD attrs = GetFileAttributesW(g_lastDisplayedImage.c_str());
        if (attrs != INVALID_FILE_ATTRIBUTES) {
            CreateOverlayWindow(g_lastDisplayedImage);
        }
    }
}
void BrowseForImage() {
    OPENFILENAMEW ofn = {0};
    wchar_t szFile[MAX_PATH] = {0};
    ofn.lStructSize = sizeof(OPENFILENAMEW);
    ofn.hwndOwner = g_hwnd;
    ofn.lpstrFile = szFile;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrFilter = L"Images\0*.png;*.jpg;*.jpeg;*.gif\0All Files\0*.*\0";
    ofn.nFilterIndex = 1;
    ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST;
    if (GetOpenFileNameW(&ofn)) {
        AddImageToList(szFile, true);
    }
}
void OpenFileLocation(const std::wstring& filePath) {
    size_t lastSlash = filePath.find_last_of(L"\\/");
    if (lastSlash != std::wstring::npos) {
        std::wstring directory = filePath.substr(0, lastSlash);
        ShellExecuteW(NULL, L"explore", directory.c_str(), NULL, NULL, SW_SHOW);
    }
}
void RemoveImageFromList(int index) {
    if (index < 0 || index >= static_cast<int>(g_imageFiles.size())) return;
    if (g_imageFiles[index].path == g_lastDisplayedImage) {
        g_lastDisplayedImage.clear();
        if (g_currentOverlay) DestroyWindow(g_currentOverlay);
        g_currentOverlay = NULL;
        g_cachedBitmap.reset();
        g_animatedGIF.reset();
        g_originalImageWidth = 0;
        g_originalImageHeight = 0;
        StopRenderThread();
    }
    g_imageFiles.erase(g_imageFiles.begin() + index);
    if (g_imageListSelected >= index) {
        if (g_imageListSelected > 0) g_imageListSelected--;
        else g_imageListSelected = -1;
    }
    if (g_hImageList) InvalidateRect(g_hImageList, NULL, TRUE);
    SaveAllSettingsToIni();
}
void UpdateHideOverlayCheckbox() {
    if (g_hHideOverlayCheckbox) {
        InvalidateRect(g_hHideOverlayCheckbox, NULL, TRUE);
    }
}
void UpdateMonitorSelection() {
    g_monitorListSelected = g_selectedMonitor;
    if (g_hMonitorList) InvalidateRect(g_hMonitorList, NULL, TRUE);
}
void UpdateImageSelection() {
    if (!g_lastDisplayedImage.empty()) {
        for (int i = 0; i < static_cast<int>(g_imageFiles.size()); i++) {
            if (g_imageFiles[i].path == g_lastDisplayedImage) {
                g_imageListSelected = i;
                if (g_hImageList) InvalidateRect(g_hImageList, NULL, TRUE);
                return;
            }
        }
    }
}
void UpdateOffsetEditControls() {
    UpdateEditFieldText(&g_offsetXField, std::to_wstring(g_offsetX));
    UpdateEditFieldText(&g_offsetYField, std::to_wstring(g_offsetY));
    if (g_hOffsetXDisplay) InvalidateRect(g_hOffsetXDisplay, NULL, TRUE);
    if (g_hOffsetYDisplay) InvalidateRect(g_hOffsetYDisplay, NULL, TRUE);
}
void UpdateScaleControls() {
    int sliderValue = (int)(g_imageScale * 100.0f + 0.5f);
    if (sliderValue < 0) sliderValue = 0;
    if (sliderValue > 200) sliderValue = 200;
    if (g_hSizeSlider) InvalidateRect(g_hSizeSlider, NULL, TRUE);
    std::wstringstream ss;
    ss << std::fixed << std::setprecision(2) << g_imageScale;
    UpdateEditFieldText(&g_sizeField, ss.str());
    if (g_hSizeDisplay) InvalidateRect(g_hSizeDisplay, NULL, TRUE);
}
void UpdateHotkeyEdits() {
    UpdateEditFieldText(&g_hotkeyToggleField, std::wstring(1, g_toggleHotkeyKey));
    UpdateEditFieldText(&g_hotkeySettingsField, std::wstring(1, g_settingsHotkeyKey));
    UpdateEditFieldText(&g_hotkeyQuitField, std::wstring(1, g_quitHotkeyKey));
    if (g_hHotkeyToggleDisplay) InvalidateRect(g_hHotkeyToggleDisplay, NULL, TRUE);
    if (g_hHotkeySettingsDisplay) InvalidateRect(g_hHotkeySettingsDisplay, NULL, TRUE);
    if (g_hHotkeyQuitDisplay) InvalidateRect(g_hHotkeyQuitDisplay, NULL, TRUE);
}
void UnregisterHotkeys() {
    if (g_hwnd) {
        UnregisterHotKey(g_hwnd, 1);
        UnregisterHotKey(g_hwnd, 2);
        UnregisterHotKey(g_hwnd, 3);
    }
}
bool RegisterHotkeys() {
    UnregisterHotkeys();
    if (!g_hwnd) return false;
    if (g_toggleHotkeyKey && !RegisterHotKey(g_hwnd, 1, MOD_ALT | MOD_NOREPEAT, g_toggleHotkeyKey)) return false;
    if (g_quitHotkeyKey && !RegisterHotKey(g_hwnd, 2, MOD_ALT | MOD_NOREPEAT, g_quitHotkeyKey)) return false;
    if (g_settingsHotkeyKey && !RegisterHotKey(g_hwnd, 3, MOD_ALT | MOD_NOREPEAT, g_settingsHotkeyKey)) return false;
    return true;
}
void ToggleHideOverlay() {
    g_hideOverlay = !g_hideOverlay;
    if (g_currentOverlay) {
        ShowWindow(g_currentOverlay, g_hideOverlay ? SW_HIDE : SW_SHOWNA);
        if (!g_hideOverlay) {
            SetWindowPos(g_currentOverlay, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
            if (g_animatedGIF && g_animatedGIF->IsInitialized() && g_animatedGIF->IsAnimated()) {
                g_animatedGIF->StartAnimation(g_currentOverlay);
            }
            RenderOverlay();
        } else {
            if (g_animatedGIF) {
                g_animatedGIF->StopAnimation();
            }
        }
    }
    UpdateAllButtons();
    SaveAllSettingsToIni();
}
LRESULT HandleHotkey(WPARAM wParam) {
    switch (wParam) {
        case 1:
            ToggleHideOverlay();
            return 0;
        case 2:
            if (g_hwnd) PostMessage(g_hwnd, WM_DESTROY, 0, 0);
            return 0;
        case 3:
            if (!IsWindowVisible(g_hwnd)) {
                ShowWindow(g_hwnd, SW_RESTORE);
                ForceForegroundWindow(g_hwnd);
                g_windowVisible = true;
            } else {
                if (IsIconic(g_hwnd)) {
                    ShowWindow(g_hwnd, SW_RESTORE);
                    ForceForegroundWindow(g_hwnd);
                    g_windowVisible = true;
                } else {
                    if (GetForegroundWindow() == g_hwnd) {
                        ShowWindow(g_hwnd, SW_HIDE);
                        g_windowVisible = false;
                    } else {
                        ForceForegroundWindow(g_hwnd);
                    }
                }
            }
            return 0;
    }
    return 0;
}
void DrawCustomSlider(HDC hdc, RECT rc, int value, bool darkTheme, bool enabled) {
    int width = rc.right - rc.left;
    int height = rc.bottom - rc.top;
    int margin = 5;
    int visualThumbHalfWidth = 4;
    int hitboxThumbHalfWidth = 16;
    int trackStart = rc.left;
    int trackEnd = rc.right;
    int trackWidth = trackEnd - trackStart;
    int usableTrackStart = trackStart + margin;
    int usableTrackEnd = trackEnd - margin;
    int usableTrackWidth = usableTrackEnd - usableTrackStart;
    int tickStart = trackStart + margin;
    int tickEnd = trackEnd - margin;
    int tickWidth = tickEnd - tickStart;
    HBRUSH hTrackBrush = CreateSolidBrush(darkTheme ? RGB(25, 25, 25) : RGB(255, 255, 255));
    RECT trackRc = {trackStart, rc.top, trackEnd, rc.bottom};
    FillRect(hdc, &trackRc, hTrackBrush);
    DeleteObject(hTrackBrush);
    int thumbPos = usableTrackStart + (value * usableTrackWidth / 200);
    for (int i = 0; i <= 10; i++) {
        int tickPos = tickStart + (i * tickWidth / 10);
        HPEN hTickPen = CreatePen(PS_SOLID, 1, darkTheme ? RGB(120, 120, 120) : RGB(180, 180, 180));
        HPEN hOldPen = (HPEN)SelectObject(hdc, hTickPen);
        MoveToEx(hdc, tickPos, rc.bottom - 6, NULL);
        LineTo(hdc, tickPos, rc.bottom - 3);
        SelectObject(hdc, hOldPen);
        DeleteObject(hTickPen);
    }
    int thumbHeight = 12;
    POINT thumbPoints[12];
    thumbPoints[0].x = thumbPos - visualThumbHalfWidth;
    thumbPoints[0].y = rc.top - 1;
    thumbPoints[1].x = thumbPos + visualThumbHalfWidth;
    thumbPoints[1].y = rc.top - 1;
    thumbPoints[2].x = thumbPos + visualThumbHalfWidth;
    thumbPoints[2].y = rc.top + thumbHeight - 4;
    thumbPoints[3].x = thumbPos + 1;
    thumbPoints[3].y = rc.top + thumbHeight - 1;
    thumbPoints[4].x = thumbPos;
    thumbPoints[4].y = rc.top + thumbHeight;
    thumbPoints[5].x = thumbPos - 1;
    thumbPoints[5].y = rc.top + thumbHeight - 1;
    thumbPoints[6].x = thumbPos - visualThumbHalfWidth;
    thumbPoints[6].y = rc.top + thumbHeight - 4;
    HBRUSH hThumbBrush = CreateSolidBrush(enabled ? (darkTheme ? RGB(200, 200, 200) : RGB(80, 80, 80)) : (darkTheme ? RGB(100, 100, 100) : RGB(180, 180, 180)));
    HPEN hThumbPen = CreatePen(PS_SOLID, 1, enabled ? (darkTheme ? RGB(180, 180, 180) : RGB(60, 60, 60)) : (darkTheme ? RGB(80, 80, 80) : RGB(150, 150, 150)));
    HPEN hOldPen = (HPEN)SelectObject(hdc, hThumbPen);
    HBRUSH hOldBrush = (HBRUSH)SelectObject(hdc, hThumbBrush);
    Polygon(hdc, thumbPoints, 7);
    SelectObject(hdc, hOldPen);
    SelectObject(hdc, hOldBrush);
    DeleteObject(hThumbBrush);
    DeleteObject(hThumbPen);
    HPEN hBorder = CreatePen(PS_SOLID, 1, darkTheme ? RGB(80, 80, 80) : RGB(150, 150, 150));
    hOldPen = (HPEN)SelectObject(hdc, hBorder);
    hOldBrush = (HBRUSH)SelectObject(hdc, GetStockObject(NULL_BRUSH));
    Rectangle(hdc, trackStart, rc.top, trackEnd, rc.bottom);
    SelectObject(hdc, hOldPen);
    SelectObject(hdc, hOldBrush);
    DeleteObject(hBorder);
}
void ApplyTheme() {
    if (g_darkTheme) {
        g_textColor = RGB(240, 240, 240);
        g_buttonTextColor = RGB(240, 240, 240);
        g_borderColor = RGB(100, 100, 100);
        g_buttonBorderColor = RGB(80, 80, 80);
        g_menuBackgroundColor = RGB(40, 40, 40);
        g_menuTextColor = RGB(240, 240, 240);
        g_menuBorderColor = RGB(80, 80, 80);
        g_menuSelectedColor = RGB(70, 70, 70);
        g_hBackgroundBrush.reset(CreateSolidBrush(RGB(25, 25, 25)));
        g_hButtonBrush.reset(CreateSolidBrush(RGB(45, 45, 45)));
        g_hButtonPressedBrush.reset(CreateSolidBrush(RGB(35, 35, 35)));
        g_hActiveTabBrush.reset(CreateSolidBrush(RGB(70, 70, 70)));
        g_hTrackbarBackgroundBrush.reset(CreateSolidBrush(RGB(45, 45, 45)));
        g_hMenuBackgroundBrush.reset(CreateSolidBrush(g_menuBackgroundColor));
        g_hMenuSelectedBrush.reset(CreateSolidBrush(g_menuSelectedColor));
        g_hListBoxBackgroundBrush.reset(CreateSolidBrush(RGB(35, 35, 35)));
        g_hListBoxSelectedBrush.reset(CreateSolidBrush(RGB(70, 70, 70)));
        if (g_hThemeButton) SetWindowText(g_hThemeButton, L"Light");
    } else {
        g_textColor = RGB(0, 0, 0);
        g_buttonTextColor = RGB(0, 0, 0);
        g_borderColor = RGB(170, 170, 170);
        g_buttonBorderColor = RGB(150, 150, 150);
        g_menuBackgroundColor = RGB(255, 255, 255);
        g_menuTextColor = RGB(0, 0, 0);
        g_menuBorderColor = RGB(200, 200, 200);
        g_menuSelectedColor = RGB(220, 220, 220);
        g_hBackgroundBrush.reset(CreateSolidBrush(RGB(255, 255, 255)));
        g_hButtonBrush.reset(CreateSolidBrush(RGB(240, 240, 240)));
        g_hButtonPressedBrush.reset(CreateSolidBrush(RGB(200, 200, 200)));
        g_hActiveTabBrush.reset(CreateSolidBrush(RGB(220, 220, 220)));
        g_hTrackbarBackgroundBrush.reset(CreateSolidBrush(RGB(240, 240, 240)));
        g_hMenuBackgroundBrush.reset(CreateSolidBrush(g_menuBackgroundColor));
        g_hMenuSelectedBrush.reset(CreateSolidBrush(g_menuSelectedColor));
        g_hListBoxBackgroundBrush.reset(CreateSolidBrush(RGB(250, 250, 250)));
        g_hListBoxSelectedBrush.reset(CreateSolidBrush(RGB(220, 220, 220)));
        if (g_hThemeButton) SetWindowText(g_hThemeButton, L"Dark");
    }
    if (!g_hMenuFont) {
        g_hMenuFont = CreateFont(14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_OUTLINE_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, VARIABLE_PITCH, L"Segoe UI");
    }
    if (g_hwnd) {
        InvalidateRect(g_hwnd, NULL, TRUE);
        UpdateWindow(g_hwnd);
        HWND hChild = GetWindow(g_hwnd, GW_CHILD);
        while (hChild) {
            InvalidateRect(hChild, NULL, TRUE);
            hChild = GetWindow(hChild, GW_HWNDNEXT);
        }
        UpdateAllButtons();
    }
}
void ToggleTheme() {
    g_darkTheme = !g_darkTheme;
    ApplyTheme();
    SaveAllSettingsToIni();
}
bool IsValidHotkeyChar(wchar_t c) {
    wchar_t upperC = std::towupper(c);
    if((upperC >= L'A' && upperC <= L'Z') || (upperC >= L'0' && upperC <= L'9')) return true;
    return false;
}
bool CheckHotkeyConflict(wchar_t newKey, int excludeId, std::wstring& conflictAction) {
    if (newKey == 0) return false;
    if (excludeId != 1 && newKey == g_toggleHotkeyKey) {
        conflictAction = L"Toggle Overlay Visibility";
        return true;
    }
    if (excludeId != 2 && newKey == g_quitHotkeyKey) {
        conflictAction = L"Quit";
        return true;
    }
    if (excludeId != 3 && newKey == g_settingsHotkeyKey) {
        conflictAction = L"Open/Close Settings";
        return true;
    }
    return false;
}
void DrawMessageBoxButton(HDC hdc, HWND hwnd, bool isPressed, bool isFocused) {
    RECT rc;
    GetClientRect(hwnd, &rc);
    HBRUSH hBrush = isPressed ? g_hButtonPressedBrush.get() : g_hButtonBrush.get();
    FillRect(hdc, &rc, hBrush);
    HPEN hBorderPen = CreatePen(PS_SOLID, 1, isFocused ? (g_darkTheme ? RGB(255, 255, 255) : RGB(0, 0, 0)) : g_buttonBorderColor);
    HPEN hOldPen = (HPEN)SelectObject(hdc, hBorderPen);
    HBRUSH hOldBrush = (HBRUSH)SelectObject(hdc, GetStockObject(NULL_BRUSH));
    Rectangle(hdc, rc.left, rc.top, rc.right, rc.bottom);
    SelectObject(hdc, hOldBrush);
    SelectObject(hdc, hOldPen);
    DeleteObject(hBorderPen);
    if (isPressed) {
        OffsetRect(&rc, 1, 1);
    }
    SetTextColor(hdc, g_buttonTextColor);
    SetBkMode(hdc, TRANSPARENT);
    HFONT hOldFont = (HFONT)SelectObject(hdc, g_hTabFont);
    DrawText(hdc, L"OK", -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    SelectObject(hdc, hOldFont);
}
LRESULT CALLBACK OwnerDrawMessageBoxProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    static bool buttonPressed = false;
    static bool buttonFocused = true;
    switch (uMsg) {
        case WM_CREATE:
            SetFocus(GetDlgItem(hwnd, 1));
            return 0;
        case WM_ACTIVATE:
            if (LOWORD(wParam) != WA_INACTIVE) {
                HWND hButton = GetDlgItem(hwnd, 1);
                SetFocus(hButton);
            }
            return 0;
        case WM_KEYDOWN:
            if (wParam == VK_SPACE) {
                buttonPressed = true;
                InvalidateRect(GetDlgItem(hwnd, 1), NULL, TRUE);
                ShowWindow(hwnd, SW_HIDE);
                DestroyWindow(hwnd);
                return 0;
            }
            return 0;
        case WM_SYSKEYDOWN:
            return 0;
        case WM_COMMAND:
            if (LOWORD(wParam) == 1) {
                ShowWindow(hwnd, SW_HIDE);
                DestroyWindow(hwnd);
            }
            return 0;
        case WM_CTLCOLORSTATIC: {
            SetBkColor((HDC)wParam, g_darkTheme ? RGB(25, 25, 25) : RGB(255, 255, 255));
            SetTextColor((HDC)wParam, g_textColor);
            return (LRESULT)g_hBackgroundBrush.get();
        }
        case WM_CTLCOLORBTN:
            SetBkColor((HDC)wParam, g_darkTheme ? RGB(25, 25, 25) : RGB(255, 255, 255));
            SetTextColor((HDC)wParam, g_textColor);
            return (LRESULT)g_hBackgroundBrush.get();
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);
            RECT clientRect;
            GetClientRect(hwnd, &clientRect);
            FillRect(hdc, &clientRect, g_hBackgroundBrush.get());
            HFONT hOldFont = (HFONT)SelectObject(hdc, g_hTabFont);
            SetTextColor(hdc, g_textColor);
            SetBkMode(hdc, TRANSPARENT);
            RECT textRect = clientRect;
            textRect.left += 10;
            textRect.right -= 10;
            textRect.top += 5;
            textRect.bottom = textRect.top + 40;
            std::wstring* pMessage = (std::wstring*)GetWindowLongPtr(hwnd, GWLP_USERDATA);
            if (pMessage) {
                DrawText(hdc, pMessage->c_str(), -1, &textRect, DT_LEFT | DT_WORDBREAK);
            }
            SelectObject(hdc, hOldFont);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_DRAWITEM: {
            DRAWITEMSTRUCT* pDraw = (DRAWITEMSTRUCT*)lParam;
            if (pDraw->CtlID == 1) {
                bool pressed = (pDraw->itemState & ODS_SELECTED) != 0;
                bool focused = (pDraw->itemState & ODS_FOCUS) != 0;
                DrawMessageBoxButton(pDraw->hDC, pDraw->hwndItem, pressed, focused);
                return TRUE;
            }
            break;
        }
        case WM_DESTROY: {
            RemoveProp(hwnd, L"MessageText");
            std::wstring* pMessage = (std::wstring*)GetWindowLongPtr(hwnd, GWLP_USERDATA);
            if (pMessage) delete pMessage;
            int remaining = GetOpenMessageBoxCount() - 1;
            if (remaining > 0) {
                ActivateNextMessageBox(hwnd);
            } else {
                EnableWindow(g_hwnd, TRUE);
                ForceForegroundWindow(g_hwnd);
            }
            break;
        }
    }
    return DefWindowProc(hwnd, uMsg, wParam, lParam);
}
void ShowOwnerDrawMessageBox(HWND hwndParent, const std::wstring& message) {
    static bool registered = false;
    if (!registered) {
        WNDCLASS wc = {};
        wc.lpfnWndProc = OwnerDrawMessageBoxProc;
        wc.hInstance = GetModuleHandle(NULL);
        wc.lpszClassName = L"OwnerDrawMessageBox";
        wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
        wc.hCursor = LoadCursor(NULL, IDC_ARROW);
        RegisterClass(&wc);
        registered = true;
    }
    if (hwndParent) EnableWindow(hwndParent, FALSE);
    RECT parentRect;
    if (hwndParent) GetWindowRect(hwndParent, &parentRect);
    else GetWindowRect(GetDesktopWindow(), &parentRect);
    int width = 250;
    int height = 125;
    int x = parentRect.left + (parentRect.right - parentRect.left - width) / 2;
    int y = parentRect.top + (parentRect.bottom - parentRect.top - height) / 2;
    HWND hMsgBox = CreateWindowEx(WS_EX_DLGMODALFRAME, L"OwnerDrawMessageBox", L"Custom Desktop Overlay", WS_POPUP | WS_CAPTION | WS_SYSMENU, x, y, width, height, hwndParent, NULL, GetModuleHandle(NULL), NULL);
    std::wstring* pMessage = new std::wstring(message);
    SetWindowLongPtr(hMsgBox, GWLP_USERDATA, (LONG_PTR)pMessage);
    HWND hButton = CreateWindowW(L"BUTTON", L"OK", WS_VISIBLE | WS_CHILD | BS_OWNERDRAW, width / 2 - 30, height - 60, 60, 25, hMsgBox, (HMENU)1, GetModuleHandle(NULL), NULL);
    SendMessage(hButton, WM_SETFONT, (WPARAM)g_hTabFont, TRUE);
    ShowWindow(hMsgBox, SW_SHOW);
    SetFocus(hButton);
    SetProp(hMsgBox, L"MessageText", (HANDLE)pMessage);
}
LRESULT CALLBACK WindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    switch (uMsg) {
        case WM_CREATE: {
            g_hwnd = hwnd;
            if (!g_hTabFont) {
                g_hTabFont = CreateFont(14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_OUTLINE_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, VARIABLE_PITCH, L"Segoe UI");
            }
            g_hMenuFont = CreateFont(14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_OUTLINE_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, VARIABLE_PITCH, L"Segoe UI");
            g_hTabButton1 = CreateWindowW(L"BUTTON", L"Select Images", WS_VISIBLE | WS_CHILD | BS_OWNERDRAW, 0, 0, 99, 25, hwnd, (HMENU)ID_TAB_BUTTON1, ((LPCREATESTRUCT)lParam)->hInstance, NULL);
            g_hTabButton2 = CreateWindowW(L"BUTTON", L"Location && Size", WS_VISIBLE | WS_CHILD | BS_OWNERDRAW, 99, 0, 99, 25, hwnd, (HMENU)ID_TAB_BUTTON2, ((LPCREATESTRUCT)lParam)->hInstance, NULL);
            g_hTabButton3 = CreateWindowW(L"BUTTON", L"Hotkeys", WS_VISIBLE | WS_CHILD | BS_OWNERDRAW, 198, 0, 99, 25, hwnd, (HMENU)ID_TAB_BUTTON3, ((LPCREATESTRUCT)lParam)->hInstance, NULL);
            g_hImageList = CreateWindowEx(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_OWNERDRAW, 7, 45, 280, VISIBLE_ITEMS_IMAGES * ITEM_HEIGHT + 2, hwnd, (HMENU)ID_IMAGE_LIST, ((LPCREATESTRUCT)lParam)->hInstance, NULL);
            g_hBrowseButton = CreateWindowW(L"BUTTON", L"Browse Image", WS_VISIBLE | WS_CHILD | BS_OWNERDRAW, 7, 195, 280, 30, hwnd, (HMENU)ID_BROWSE_BUTTON, ((LPCREATESTRUCT)lParam)->hInstance, NULL);
            g_hHideOverlayCheckbox = CreateWindowW(L"BUTTON", L" Hide Overlay", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, 7, 250, 120, 20, hwnd, (HMENU)ID_HIDE_OVERLAY_CHECKBOX, ((LPCREATESTRUCT)lParam)->hInstance, NULL);
            g_hThemeButton = CreateWindowW(L"BUTTON", L"Dark", WS_VISIBLE | WS_CHILD | BS_OWNERDRAW, 237, 248, 50, 20, hwnd, (HMENU)ID_THEME_BUTTON, ((LPCREATESTRUCT)lParam)->hInstance, NULL);
            g_hMonitorList = CreateWindowEx(0, L"STATIC", L"", WS_CHILD | SS_OWNERDRAW, 7, 45, 280, VISIBLE_ITEMS_MONITORS * ITEM_HEIGHT + 2, hwnd, (HMENU)ID_MONITOR_LIST, ((LPCREATESTRUCT)lParam)->hInstance, NULL);
            g_hOffsetXDisplay = CreateWindowEx(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_OWNERDRAW, 25, 115, 50, 20, hwnd, (HMENU)ID_OFFSET_X_DISPLAY, ((LPCREATESTRUCT)lParam)->hInstance, NULL);
            g_hOffsetYDisplay = CreateWindowEx(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_OWNERDRAW, 25, 140, 50, 20, hwnd, (HMENU)ID_OFFSET_Y_DISPLAY, ((LPCREATESTRUCT)lParam)->hInstance, NULL);
            g_hSizeSlider = CreateWindowEx(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_OWNERDRAW, 7, 180, 280, 20, hwnd, (HMENU)ID_SIZE_SLIDER, ((LPCREATESTRUCT)lParam)->hInstance, NULL);
            g_hSizeDisplay = CreateWindowEx(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_OWNERDRAW, 7, 205, 50, 20, hwnd, (HMENU)ID_SIZE_DISPLAY, ((LPCREATESTRUCT)lParam)->hInstance, NULL);
            g_hHotkeyToggleDisplay = CreateWindowEx(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_OWNERDRAW, 40, 45, 30, 20, hwnd, (HMENU)ID_HOTKEY_TOGGLE_DISPLAY, ((LPCREATESTRUCT)lParam)->hInstance, NULL);
            g_hHotkeySettingsDisplay = CreateWindowEx(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_OWNERDRAW, 40, 90, 30, 20, hwnd, (HMENU)ID_HOTKEY_SETTINGS_DISPLAY, ((LPCREATESTRUCT)lParam)->hInstance, NULL);
            g_hHotkeyQuitDisplay = CreateWindowEx(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_OWNERDRAW, 40, 135, 30, 20, hwnd, (HMENU)ID_HOTKEY_QUIT_DISPLAY, ((LPCREATESTRUCT)lParam)->hInstance, NULL);
            g_hResetToggle = CreateWindowW(L"BUTTON", L"Reset", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, 75, 45, 60, 20, hwnd, (HMENU)ID_RESET_TOGGLE, ((LPCREATESTRUCT)lParam)->hInstance, NULL);
            g_hResetSettings = CreateWindowW(L"BUTTON", L"Reset", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, 75, 90, 60, 20, hwnd, (HMENU)ID_RESET_SETTINGS, ((LPCREATESTRUCT)lParam)->hInstance, NULL);
            g_hResetQuit = CreateWindowW(L"BUTTON", L"Reset", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, 75, 135, 60, 20, hwnd, (HMENU)ID_RESET_QUIT, ((LPCREATESTRUCT)lParam)->hInstance, NULL);
            g_offsetXField.hwnd = g_hOffsetXDisplay;
            g_offsetYField.hwnd = g_hOffsetYDisplay;
            g_sizeField.hwnd = g_hSizeDisplay;
            g_hotkeyToggleField.hwnd = g_hHotkeyToggleDisplay;
            g_hotkeySettingsField.hwnd = g_hHotkeySettingsDisplay;
            g_hotkeyQuitField.hwnd = g_hHotkeyQuitDisplay;
            g_oldImageListProc = (WNDPROC)SetWindowLongPtr(g_hImageList, GWLP_WNDPROC, (LONG_PTR)ImageListSubclassProc);
            g_oldMonitorListProc = (WNDPROC)SetWindowLongPtr(g_hMonitorList, GWLP_WNDPROC, (LONG_PTR)MonitorListSubclassProc);
            g_oldOffsetXProc = (WNDPROC)SetWindowLongPtr(g_hOffsetXDisplay, GWLP_WNDPROC, (LONG_PTR)OffsetXSubclassProc);
            g_oldOffsetYProc = (WNDPROC)SetWindowLongPtr(g_hOffsetYDisplay, GWLP_WNDPROC, (LONG_PTR)OffsetYSubclassProc);
            g_oldSizeProc = (WNDPROC)SetWindowLongPtr(g_hSizeDisplay, GWLP_WNDPROC, (LONG_PTR)SizeSubclassProc);
            g_oldHotkeyToggleProc = (WNDPROC)SetWindowLongPtr(g_hHotkeyToggleDisplay, GWLP_WNDPROC, (LONG_PTR)HotkeyToggleSubclassProc);
            g_oldHotkeySettingsProc = (WNDPROC)SetWindowLongPtr(g_hHotkeySettingsDisplay, GWLP_WNDPROC, (LONG_PTR)HotkeySettingsSubclassProc);
            g_oldHotkeyQuitProc = (WNDPROC)SetWindowLongPtr(g_hHotkeyQuitDisplay, GWLP_WNDPROC, (LONG_PTR)HotkeyQuitSubclassProc);
            if (g_hTabFont) {
                SendMessage(g_hImageList, WM_SETFONT, (WPARAM)g_hTabFont, TRUE);
                SendMessage(g_hBrowseButton, WM_SETFONT, (WPARAM)g_hTabFont, TRUE);
                SendMessage(g_hHideOverlayCheckbox, WM_SETFONT, (WPARAM)g_hTabFont, TRUE);
                SendMessage(g_hThemeButton, WM_SETFONT, (WPARAM)g_hTabFont, TRUE);
                SendMessage(g_hMonitorList, WM_SETFONT, (WPARAM)g_hTabFont, TRUE);
                SendMessage(g_hOffsetXDisplay, WM_SETFONT, (WPARAM)g_hTabFont, TRUE);
                SendMessage(g_hOffsetYDisplay, WM_SETFONT, (WPARAM)g_hTabFont, TRUE);
                SendMessage(g_hSizeSlider, WM_SETFONT, (WPARAM)g_hTabFont, TRUE);
                SendMessage(g_hSizeDisplay, WM_SETFONT, (WPARAM)g_hTabFont, TRUE);
                SendMessage(g_hHotkeyToggleDisplay, WM_SETFONT, (WPARAM)g_hTabFont, TRUE);
                SendMessage(g_hHotkeySettingsDisplay, WM_SETFONT, (WPARAM)g_hTabFont, TRUE);
                SendMessage(g_hHotkeyQuitDisplay, WM_SETFONT, (WPARAM)g_hTabFont, TRUE);
                SendMessage(g_hResetToggle, WM_SETFONT, (WPARAM)g_hTabFont, TRUE);
                SendMessage(g_hResetSettings, WM_SETFONT, (WPARAM)g_hTabFont, TRUE);
                SendMessage(g_hResetQuit, WM_SETFONT, (WPARAM)g_hTabFont, TRUE);
            }
            LoadAllSettingsFromIni();
            ApplyTheme();
            UpdateMonitorList();
            CorrectScaleValue();
            CorrectOffsetValues();
            UpdateImageSelection();
            UpdateMonitorSelection();
            UpdateScaleControls();
            UpdateOffsetEditControls();
            UpdateHotkeyEdits();
            RestoreLastOverlay();
            UpdateHideOverlayCheckbox();
            RegisterHotkeys();
            g_prevMonitorCount = g_monitors.size();
            g_prevMonitorRects.clear();
            for (const auto& monitor : g_monitors) {
                g_prevMonitorRects.push_back(monitor.rect);
            }
            nid = {sizeof(NOTIFYICONDATA), hwnd, ID_TRAY_APP_ICON, NIF_ICON | NIF_MESSAGE | NIF_TIP, WM_TRAYICON};
            nid.hIcon = g_hMainIcon;
            wcsncpy_s(nid.szTip, L"Custom Desktop Overlay", _countof(nid.szTip) - 1);
            Shell_NotifyIcon(NIM_ADD, &nid);
            if (g_windowX != CW_USEDEFAULT && g_windowY != CW_USEDEFAULT) {
                SetWindowPos(hwnd, NULL, g_windowX, g_windowY, 300, 300, SWP_NOZORDER | SWP_NOSIZE);
            }
            ShowWindow(g_hImageList, SW_SHOW);
            ShowWindow(g_hBrowseButton, SW_SHOW);
            ShowWindow(g_hMonitorList, SW_HIDE);
            ShowWindow(g_hOffsetXDisplay, SW_HIDE);
            ShowWindow(g_hOffsetYDisplay, SW_HIDE);
            ShowWindow(g_hSizeSlider, SW_HIDE);
            ShowWindow(g_hSizeDisplay, SW_HIDE);
            ShowWindow(g_hHotkeyToggleDisplay, SW_HIDE);
            ShowWindow(g_hHotkeySettingsDisplay, SW_HIDE);
            ShowWindow(g_hHotkeyQuitDisplay, SW_HIDE);
            ShowWindow(g_hResetToggle, SW_HIDE);
            ShowWindow(g_hResetSettings, SW_HIDE);
            ShowWindow(g_hResetQuit, SW_HIDE);
            return 0;
        }
        case WM_DISPLAYCHANGE: {
            UpdateMonitorList();
            CorrectOffsetValues();
            UpdateOffsetBoundsDisplay();
            if (g_currentOverlay) {
                CreateOverlayWindow(g_lastDisplayedImage);
            }
            g_prevMonitorCount = g_monitors.size();
            g_prevMonitorRects.clear();
            for (const auto& monitor : g_monitors) {
                g_prevMonitorRects.push_back(monitor.rect);
            }
            return 0;
        }
        case WM_HOTKEY:
            SendMessage(hwnd, WM_CANCELMODE, 0, 0);
            return HandleHotkey(wParam);
        case WM_MEASUREITEM: {
            MEASUREITEMSTRUCT* pMeasure = (MEASUREITEMSTRUCT*)lParam;
            if (pMeasure->CtlType == ODT_MENU) {
                pMeasure->itemHeight = 24;
                pMeasure->itemWidth = 150;
                return TRUE;
            }
            break;
        }
        case WM_COMMAND:
            switch (LOWORD(wParam)) {
                case ID_TRAY_SHOW:
                    ShowWindow(hwnd, SW_RESTORE);
                    SetForegroundWindow(hwnd);
                    g_windowVisible = true;
                    break;
                case ID_TRAY_HIDE_OVERLAY:
                    ToggleHideOverlay();
                    break;
                case ID_TRAY_EXIT:
                    DestroyWindow(hwnd);
                    break;
                case ID_TAB_BUTTON1:
                    g_activeTab = 1;
                    ShowWindow(g_hImageList, SW_SHOW);
                    ShowWindow(g_hBrowseButton, SW_SHOW);
                    ShowWindow(g_hMonitorList, SW_HIDE);
                    ShowWindow(g_hOffsetXDisplay, SW_HIDE);
                    ShowWindow(g_hOffsetYDisplay, SW_HIDE);
                    ShowWindow(g_hSizeSlider, SW_HIDE);
                    ShowWindow(g_hSizeDisplay, SW_HIDE);
                    ShowWindow(g_hHotkeyToggleDisplay, SW_HIDE);
                    ShowWindow(g_hHotkeySettingsDisplay, SW_HIDE);
                    ShowWindow(g_hHotkeyQuitDisplay, SW_HIDE);
                    ShowWindow(g_hResetToggle, SW_HIDE);
                    ShowWindow(g_hResetSettings, SW_HIDE);
                    ShowWindow(g_hResetQuit, SW_HIDE);
                    InvalidateRect(hwnd, NULL, TRUE);
                    break;
                case ID_TAB_BUTTON2:
                    g_activeTab = 2;
                    ShowWindow(g_hImageList, SW_HIDE);
                    ShowWindow(g_hBrowseButton, SW_HIDE);
                    ShowWindow(g_hMonitorList, SW_SHOW);
                    ShowWindow(g_hOffsetXDisplay, SW_SHOW);
                    ShowWindow(g_hOffsetYDisplay, SW_SHOW);
                    ShowWindow(g_hSizeSlider, SW_SHOW);
                    ShowWindow(g_hSizeDisplay, SW_SHOW);
                    ShowWindow(g_hHotkeyToggleDisplay, SW_HIDE);
                    ShowWindow(g_hHotkeySettingsDisplay, SW_HIDE);
                    ShowWindow(g_hHotkeyQuitDisplay, SW_HIDE);
                    ShowWindow(g_hResetToggle, SW_HIDE);
                    ShowWindow(g_hResetSettings, SW_HIDE);
                    ShowWindow(g_hResetQuit, SW_HIDE);
                    InvalidateRect(hwnd, NULL, TRUE);
                    break;
                case ID_TAB_BUTTON3:
                    g_activeTab = 3;
                    ShowWindow(g_hImageList, SW_HIDE);
                    ShowWindow(g_hBrowseButton, SW_HIDE);
                    ShowWindow(g_hMonitorList, SW_HIDE);
                    ShowWindow(g_hOffsetXDisplay, SW_HIDE);
                    ShowWindow(g_hOffsetYDisplay, SW_HIDE);
                    ShowWindow(g_hSizeSlider, SW_HIDE);
                    ShowWindow(g_hSizeDisplay, SW_HIDE);
                    ShowWindow(g_hHotkeyToggleDisplay, SW_SHOW);
                    ShowWindow(g_hHotkeySettingsDisplay, SW_SHOW);
                    ShowWindow(g_hHotkeyQuitDisplay, SW_SHOW);
                    ShowWindow(g_hResetToggle, SW_SHOW);
                    ShowWindow(g_hResetSettings, SW_SHOW);
                    ShowWindow(g_hResetQuit, SW_SHOW);
                    InvalidateRect(hwnd, NULL, TRUE);
                    break;
                case ID_BROWSE_BUTTON:
                    BrowseForImage();
                    break;
                case ID_CONTEXT_OPEN: {
                    if (g_imageListSelected >= 0 && g_imageListSelected < (int)g_imageFiles.size()) {
                        OpenFileLocation(g_imageFiles[g_imageListSelected].path);
                    }
                    break;
                }
                case ID_CONTEXT_REMOVE: {
                    if (g_imageListSelected >= 0) RemoveImageFromList(g_imageListSelected);
                    break;
                }
                case ID_HIDE_OVERLAY_CHECKBOX:
                    if (HIWORD(wParam) == BN_CLICKED) ToggleHideOverlay();
                    break;
                case ID_THEME_BUTTON:
                    if (HIWORD(wParam) == BN_CLICKED) ToggleTheme();
                    break;
                case ID_RESET_TOGGLE: {
                    ResetHotkeyField(1, DEFAULT_TOGGLE_KEY, g_toggleHotkeyKey, g_lastValidToggleKey, g_hotkeyToggleField);
                    if (g_hHotkeyToggleDisplay) InvalidateRect(g_hHotkeyToggleDisplay, NULL, TRUE);
                    RegisterHotkeys();
                    SaveAllSettingsToIni();
                    break;
                }
                case ID_RESET_SETTINGS: {
                    ResetHotkeyField(3, DEFAULT_SETTINGS_KEY, g_settingsHotkeyKey, g_lastValidSettingsKey, g_hotkeySettingsField);
                    if (g_hHotkeySettingsDisplay) InvalidateRect(g_hHotkeySettingsDisplay, NULL, TRUE);
                    RegisterHotkeys();
                    SaveAllSettingsToIni();
                    break;
                }
                case ID_RESET_QUIT: {
                    ResetHotkeyField(2, DEFAULT_QUIT_KEY, g_quitHotkeyKey, g_lastValidQuitKey, g_hotkeyQuitField);
                    if (g_hHotkeyQuitDisplay) InvalidateRect(g_hHotkeyQuitDisplay, NULL, TRUE);
                    RegisterHotkeys();
                    SaveAllSettingsToIni();
                    break;
                }
            }
            return 0;
        case WM_SYSCOMMAND:
        {
            UINT nID = wParam & 0xFFF0;
            if (nID == SC_KEYMENU || nID == SC_MOUSEMENU)
            {
                return 0;
            }
            return DefWindowProc(hwnd, uMsg, wParam, lParam);
        }
        case WM_NCLBUTTONDBLCLK:
            if (wParam == HTSYSMENU || wParam == HTCAPTION)
            {
                return 0;
            }
            return DefWindowProc(hwnd, uMsg, wParam, lParam);
        case WM_CONTEXTMENU:
            if ((HWND)wParam == g_hImageList) {
                if (g_imageListSelected != -1) {
                    POINT pt;
                    if (lParam == -1) {
                        RECT rc;
                        GetWindowRect(g_hImageList, &rc);
                        pt.x = rc.left + 10;
                        pt.y = rc.top + 10;
                    } else {
                        pt.x = GET_X_LPARAM(lParam);
                        pt.y = GET_Y_LPARAM(lParam);
                    }
                    HMENU hMenu = CreatePopupMenu();
                    CreateOwnerDrawMenu(hMenu, 0, ID_CONTEXT_OPEN, L"Open File Location");
                    CreateOwnerDrawMenu(hMenu, 1, ID_CONTEXT_REMOVE, L"Remove From Image List");
                    SetForegroundWindow(hwnd);
                    TrackPopupMenu(hMenu, TPM_LEFTALIGN | TPM_LEFTBUTTON, pt.x, pt.y, 0, hwnd, NULL);
                    DestroyMenu(hMenu);
                }
            }
            return 0;
        case WM_DRAWITEM: {
            DRAWITEMSTRUCT* pDraw = (DRAWITEMSTRUCT*)lParam;
            if (pDraw->CtlID == ID_HIDE_OVERLAY_CHECKBOX) {
                wchar_t buffer[256];
                GetWindowText(pDraw->hwndItem, buffer, 256);
                std::wstring text = buffer;
                if (!text.empty() && text[0] == L' ')
                    text = text.substr(1);
                bool isChecked = g_hideOverlay;
                DrawOwnerDrawCheckbox(pDraw->hDC, pDraw->hwndItem, isChecked, text);
                return TRUE;
            }
            else if (pDraw->CtlID == ID_TAB_BUTTON1 || pDraw->CtlID == ID_TAB_BUTTON2 || pDraw->CtlID == ID_TAB_BUTTON3 || pDraw->CtlID == ID_BROWSE_BUTTON || pDraw->CtlID == ID_THEME_BUTTON || pDraw->CtlID == ID_RESET_TOGGLE || pDraw->CtlID == ID_RESET_SETTINGS || pDraw->CtlID == ID_RESET_QUIT) {
                wchar_t buffer[256];
                GetWindowText(pDraw->hwndItem, buffer, 256);
                bool isPressed = (pDraw->itemState & ODS_SELECTED) != 0;
                bool isActiveTab = false;
                if (pDraw->CtlID == ID_TAB_BUTTON1 && g_activeTab == 1) isActiveTab = true;
                else if (pDraw->CtlID == ID_TAB_BUTTON2 && g_activeTab == 2) isActiveTab = true;
                else if (pDraw->CtlID == ID_TAB_BUTTON3 && g_activeTab == 3) isActiveTab = true;
                DrawOwnerDrawButton(pDraw->hDC, pDraw->hwndItem, isPressed, buffer, isActiveTab);
                return TRUE;
            }
            else if (pDraw->CtlID == ID_SIZE_SLIDER) {
                int sliderValue = (int)(g_imageScale * 100.0f + 0.5f);
                DrawCustomSlider(pDraw->hDC, pDraw->rcItem, sliderValue, g_darkTheme, IsWindowEnabled(pDraw->hwndItem));
                return TRUE;
            }
            else if (pDraw->CtlID == ID_IMAGE_LIST || pDraw->CtlID == ID_MONITOR_LIST) {
                DrawOwnerDrawListBox(pDraw->hDC, pDraw->hwndItem, pDraw->CtlID);
                return TRUE;
            }
            else if (pDraw->CtlType == ODT_MENU) {
                std::wstring menuText = L"";
                bool isChecked = false;
                bool isTrayHideOverlay = false;
                switch (pDraw->itemID) {
                    case ID_TRAY_HIDE_OVERLAY:
                        menuText = L"Hide Overlay";
                        isChecked = g_hideOverlay;
                        isTrayHideOverlay = true;
                        break;
                    case ID_TRAY_SHOW:
                        menuText = L"Settings";
                        isTrayHideOverlay = false;
                        break;
                    case ID_TRAY_EXIT:
                        menuText = L"Quit";
                        isTrayHideOverlay = false;
                        break;
                    case ID_CONTEXT_OPEN:
                        menuText = L"Open File Location";
                        isTrayHideOverlay = false;
                        break;
                    case ID_CONTEXT_REMOVE:
                        menuText = L"Remove From Image List";
                        isTrayHideOverlay = false;
                        break;
                    default:
                        if (pDraw->itemData) {
                            menuText = (const wchar_t*)pDraw->itemData;
                        }
                }
                if (!menuText.empty()) {
                    bool isSelected = (pDraw->itemState & ODS_SELECTED) != 0;
                    bool isDisabled = (pDraw->itemState & ODS_DISABLED) != 0;
                    DrawMenuItem(pDraw->hDC, pDraw->rcItem, menuText, isSelected, isChecked, isDisabled, isTrayHideOverlay);
                    return TRUE;
                }
            }
            break;
        }
        case WM_CTLCOLORSTATIC:
        {
            TCHAR className[256] = {0};
            GetClassName((HWND)lParam, className, 256);
            if (_wcsicmp(className, TRACKBAR_CLASS) == 0 && g_hTrackbarBackgroundBrush) {
                return (LRESULT)g_hTrackbarBackgroundBrush.get();
            }
            SetBkColor((HDC)wParam, g_darkTheme ? RGB(25, 25, 25) : RGB(255, 255, 255));
            SetTextColor((HDC)wParam, g_textColor);
            return (LRESULT)g_hBackgroundBrush.get();
        }
        case WM_CTLCOLORBTN:
            if (g_hHideOverlayCheckbox && (HWND)lParam == g_hHideOverlayCheckbox) {
                return 0;
            }
            SetBkColor((HDC)wParam, g_darkTheme ? RGB(25, 25, 25) : RGB(255, 255, 255));
            SetTextColor((HDC)wParam, g_textColor);
            return (LRESULT)g_hBackgroundBrush.get();
        case WM_CTLCOLOREDIT:
            SetBkColor((HDC)wParam, g_darkTheme ? RGB(25, 25, 25) : RGB(255, 255, 255));
            SetTextColor((HDC)wParam, g_textColor);
            return (LRESULT)g_hBackgroundBrush.get();
        case WM_TRAYICON:
            if (lParam == WM_LBUTTONDBLCLK) {
                g_windowVisible = !g_windowVisible;
                ShowWindow(hwnd, g_windowVisible ? SW_RESTORE : SW_HIDE);
                if (g_windowVisible) SetForegroundWindow(hwnd);
            }
            else if (lParam == WM_RBUTTONUP) {
                POINT pt;
                GetCursorPos(&pt);
                HMENU hMenu = CreatePopupMenu();
                CreateOwnerDrawMenu(hMenu, 0, ID_TRAY_HIDE_OVERLAY, L"Hide Overlay");
                CreateOwnerDrawMenu(hMenu, 1, ID_TRAY_SHOW, L"Settings");
                CreateOwnerDrawMenu(hMenu, 2, ID_TRAY_EXIT, L"Quit");
                SetForegroundWindow(hwnd);
                TrackPopupMenu(hMenu, TPM_BOTTOMALIGN | TPM_LEFTALIGN, pt.x, pt.y, 0, hwnd, NULL);
                DestroyMenu(hMenu);
            }
            return 0;
        case WM_DESTROY:
            g_programExiting = true;
            if (g_monitorNamesThread.joinable()) {
                g_monitorNamesThread.join();
            }
            if (g_oldImageListProc) {
                SetWindowLongPtr(g_hImageList, GWLP_WNDPROC, (LONG_PTR)g_oldImageListProc);
            }
            if (g_oldMonitorListProc) {
                SetWindowLongPtr(g_hMonitorList, GWLP_WNDPROC, (LONG_PTR)g_oldMonitorListProc);
            }
            if (g_oldOffsetXProc) {
                SetWindowLongPtr(g_hOffsetXDisplay, GWLP_WNDPROC, (LONG_PTR)g_oldOffsetXProc);
            }
            if (g_oldOffsetYProc) {
                SetWindowLongPtr(g_hOffsetYDisplay, GWLP_WNDPROC, (LONG_PTR)g_oldOffsetYProc);
            }
            if (g_oldSizeProc) {
                SetWindowLongPtr(g_hSizeDisplay, GWLP_WNDPROC, (LONG_PTR)g_oldSizeProc);
            }
            if (g_oldHotkeyToggleProc) {
                SetWindowLongPtr(g_hHotkeyToggleDisplay, GWLP_WNDPROC, (LONG_PTR)g_oldHotkeyToggleProc);
            }
            if (g_oldHotkeySettingsProc) {
                SetWindowLongPtr(g_hHotkeySettingsDisplay, GWLP_WNDPROC, (LONG_PTR)g_oldHotkeySettingsProc);
            }
            if (g_oldHotkeyQuitProc) {
                SetWindowLongPtr(g_hHotkeyQuitDisplay, GWLP_WNDPROC, (LONG_PTR)g_oldHotkeyQuitProc);
            }
            StopRenderThread();
            UnregisterHotkeys();
            SaveAllSettingsToIni();
            Shell_NotifyIcon(NIM_DELETE, &nid);
            if (nid.hIcon) DestroyIcon(nid.hIcon);
            g_cachedBitmap.reset();
            g_animatedGIF.reset();
            SafeGdiplusShutdown();
            if (g_hTabFont) {
                DeleteObject(g_hTabFont);
                g_hTabFont = NULL;
            }
            if (g_hMenuFont) {
                DeleteObject(g_hMenuFont);
                g_hMenuFont = NULL;
            }
            PostQuitMessage(0);
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);
            RECT clientRect;
            GetClientRect(hwnd, &clientRect);
            FillRect(hdc, &clientRect, g_hBackgroundBrush.get());
            HPEN hLinePen = CreatePen(PS_SOLID, 1, g_borderColor);
            HPEN hOldPen = (HPEN)SelectObject(hdc, hLinePen);
            MoveToEx(hdc, 0, 25, NULL);
            LineTo(hdc, clientRect.right, 25);
            SelectObject(hdc, hOldPen);
            DeleteObject(hLinePen);
            HPEN hSeparatorPen = CreatePen(PS_SOLID, 1, g_darkTheme ? RGB(60, 60, 60) : RGB(220, 220, 220));
            hOldPen = (HPEN)SelectObject(hdc, hSeparatorPen);
            MoveToEx(hdc, 0, 245, NULL);
            LineTo(hdc, clientRect.right, 245);
            SelectObject(hdc, hOldPen);
            DeleteObject(hSeparatorPen);
            HFONT hOldFont = (HFONT)SelectObject(hdc, g_hTabFont);
            SetTextColor(hdc, g_textColor);
            SetBkColor(hdc, g_darkTheme ? RGB(25, 25, 25) : RGB(255, 255, 255));
            SetBkMode(hdc, OPAQUE);
            bool showImageControls = (IsWindowVisible(g_hImageList) && IsWindowEnabled(g_hImageList));
            bool showMonitorControls = (IsWindowVisible(g_hMonitorList) && IsWindowEnabled(g_hMonitorList));
            bool showHotkeyControls = (IsWindowVisible(g_hHotkeyToggleDisplay) && IsWindowEnabled(g_hHotkeyToggleDisplay));
            if (showImageControls) {
                RECT textRect = {8, 30, 280, 50};
                FillRect(hdc, &textRect, g_hBackgroundBrush.get());
                DrawText(hdc, L"Images List", -1, &textRect, DT_LEFT | DT_TOP);
                RECT formatRect = {8, 230, 280, 245};
                FillRect(hdc, &formatRect, g_hBackgroundBrush.get());
                DrawText(hdc, L"Supported formats: .png, .jpg, .jpeg, .gif", -1, &formatRect, DT_LEFT | DT_TOP);
            }
            if (showMonitorControls) {
                RECT textRect = {8, 30, 280, 50};
                FillRect(hdc, &textRect, g_hBackgroundBrush.get());
                DrawText(hdc, L"Monitors List", -1, &textRect, DT_LEFT | DT_TOP);
                RECT offsetTextRect = {8, 100, 280, 130};
                FillRect(hdc, &offsetTextRect, g_hBackgroundBrush.get());
                DrawText(hdc, L"Location Offset", -1, &offsetTextRect, DT_LEFT | DT_TOP);
                RECT xLabelRect = {8, 118, 20, 155};
                FillRect(hdc, &xLabelRect, g_hBackgroundBrush.get());
                DrawText(hdc, L"X:", -1, &xLabelRect, DT_LEFT | DT_TOP);
                RECT yLabelRect = {8, 143, 20, 180};
                FillRect(hdc, &yLabelRect, g_hBackgroundBrush.get());
                DrawText(hdc, L"Y:", -1, &yLabelRect, DT_LEFT | DT_TOP);
                int maxX = 0, maxY = 0;
                if (g_selectedMonitor >= 0 && g_selectedMonitor < static_cast<int>(g_monitors.size())) {
                    const auto& monitor = g_monitors[g_selectedMonitor];
                    maxX = (monitor.rect.right - monitor.rect.left) / 2;
                    maxY = (monitor.rect.bottom - monitor.rect.top) / 2;
                }
                std::wstring xBounds = L"[" + std::to_wstring(-maxX) + L"; " + std::to_wstring(maxX) + L"]";
                std::wstring yBounds = L"[" + std::to_wstring(-maxY) + L"; " + std::to_wstring(maxY) + L"]";
                RECT xBoundsRect = {85, 118, 220, 155};
                FillRect(hdc, &xBoundsRect, g_hBackgroundBrush.get());
                DrawText(hdc, xBounds.c_str(), -1, &xBoundsRect, DT_LEFT | DT_TOP);
                RECT yBoundsRect = {85, 143, 220, 180};
                FillRect(hdc, &yBoundsRect, g_hBackgroundBrush.get());
                DrawText(hdc, yBounds.c_str(), -1, &yBoundsRect, DT_LEFT | DT_TOP);
                RECT sizeTextRect = {8, 165, 280, 180};
                FillRect(hdc, &sizeTextRect, g_hBackgroundBrush.get());
                DrawText(hdc, L"Size", -1, &sizeTextRect, DT_LEFT | DT_TOP);
                std::wstring sizeBounds = L"[0.00; 2.00]";
                RECT sizeBoundsRect = {67, 208, 200, 240};
                FillRect(hdc, &sizeBoundsRect, g_hBackgroundBrush.get());
                DrawText(hdc, sizeBounds.c_str(), -1, &sizeBoundsRect, DT_LEFT | DT_TOP);
            }
            if (showHotkeyControls) {
                RECT toggleTextRect = {8, 30, 280, 50};
                FillRect(hdc, &toggleTextRect, g_hBackgroundBrush.get());
                DrawText(hdc, L"Toggle Overlay Visibility:", -1, &toggleTextRect, DT_LEFT | DT_TOP);
                RECT keyLabelRect = {8, 48, 280, 68};
                FillRect(hdc, &keyLabelRect, g_hBackgroundBrush.get());
                DrawText(hdc, L"Alt +", -1, &keyLabelRect, DT_LEFT | DT_TOP);
                RECT settingsTextRect = {8, 75, 280, 95};
                FillRect(hdc, &settingsTextRect, g_hBackgroundBrush.get());
                DrawText(hdc, L"Open/Close Settings:", -1, &settingsTextRect, DT_LEFT | DT_TOP);
                RECT keyLabelRect2 = {8, 93, 280, 113};
                FillRect(hdc, &keyLabelRect2, g_hBackgroundBrush.get());
                DrawText(hdc, L"Alt +", -1, &keyLabelRect2, DT_LEFT | DT_TOP);
                RECT quitTextRect = {8, 120, 280, 140};
                FillRect(hdc, &quitTextRect, g_hBackgroundBrush.get());
                DrawText(hdc, L"Quit:", -1, &quitTextRect, DT_LEFT | DT_TOP);
                RECT keyLabelRect3 = {8, 138, 280, 158};
                FillRect(hdc, &keyLabelRect3, g_hBackgroundBrush.get());
                DrawText(hdc, L"Alt +", -1, &keyLabelRect3, DT_LEFT | DT_TOP);
                RECT formatRect1 = {8, 215, 280, 230};
                FillRect(hdc, &formatRect1, g_hBackgroundBrush.get());
                DrawText(hdc, L"Allowed input: 0-9 and A-Z", -1, &formatRect1, DT_LEFT | DT_TOP);
                RECT formatRect2 = {8, 230, 280, 245};
                FillRect(hdc, &formatRect2, g_hBackgroundBrush.get());
                DrawText(hdc, L"Null input disables hotkey", -1, &formatRect2, DT_LEFT | DT_TOP);
            }
            SelectObject(hdc, hOldFont);
            EndPaint(hwnd, &ps);
            return 0;
        } 
        case WM_CLOSE:
            ShowWindow(hwnd, SW_HIDE);
            g_windowVisible = false;
            return 0;
        case WM_SIZE:
            if (wParam == SIZE_MINIMIZED) {
                ShowWindow(hwnd, SW_HIDE);
                g_windowVisible = false;
            }
            else InvalidateRect(hwnd, NULL, TRUE);
            return 0;
        case WM_GETMINMAXINFO:
            ((MINMAXINFO*)lParam)->ptMinTrackSize.x = 300;
            ((MINMAXINFO*)lParam)->ptMinTrackSize.y = 300;
            ((MINMAXINFO*)lParam)->ptMaxTrackSize.x = 300;
            ((MINMAXINFO*)lParam)->ptMaxTrackSize.y = 300;
            return 0;
        case WM_SHOW_AND_MESSAGE:
        {
            std::wstring msgText = L"The program is already running. The main window has been restored";
            if (IsIconic(hwnd)) {
                ShowWindow(hwnd, SW_RESTORE);
            } else if (!IsWindowVisible(hwnd)) {
                ShowWindow(hwnd, SW_SHOWNA);
            }
            if (IsMessageBoxWithTextOpen(msgText)) {
                HWND hMsgBox = FindWindow(L"OwnerDrawMessageBox", NULL);
                if (hMsgBox) {
                    ForceForegroundWindow(hMsgBox);
                }
            } else {
                ForceForegroundWindow(hwnd);
                ShowOwnerDrawMessageBox(hwnd, msgText);
            }
            return 0;
        }
        case WM_LBUTTONDOWN: {
            POINT pt = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            RECT sliderRect;
            GetWindowRect(g_hSizeSlider, &sliderRect);
            MapWindowPoints(HWND_DESKTOP, hwnd, (LPPOINT)&sliderRect, 2);
            if (PtInRect(&sliderRect, pt) && IsWindowVisible(g_hSizeSlider)) {
                int margin = 5;
                int trackStart = sliderRect.left + margin;
                int trackEnd = sliderRect.right - margin;
                int trackWidth = trackEnd - trackStart;
                int clickX = pt.x;
                if (clickX < trackStart) clickX = trackStart;
                if (clickX > trackEnd) clickX = trackEnd;
                int newValue = ((clickX - trackStart) * 200) / trackWidth;
                if (newValue < 0) newValue = 0;
                if (newValue > 200) newValue = 200;
                float newScale = newValue / 100.0f;
                if (fabs(g_imageScale - newScale) > 0.001f) {
                    g_imageScale = newScale;
                    UpdateScaleControls();
                    CorrectScaleValue();
                }
                g_sliderDragging = true;
                SetCapture(hwnd);
                g_renderRequestPending = true;
            }
            break;
        }
        case WM_MOUSEMOVE: {
            if (g_sliderDragging && (wParam & MK_LBUTTON)) {
                POINT pt = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
                RECT sliderRect;
                GetWindowRect(g_hSizeSlider, &sliderRect);
                MapWindowPoints(HWND_DESKTOP, hwnd, (LPPOINT)&sliderRect, 2);
                int margin = 5;
                int trackStart = sliderRect.left + margin;
                int trackEnd = sliderRect.right - margin;
                int trackWidth = trackEnd - trackStart;
                int clickX = pt.x;
                if (clickX < trackStart) clickX = trackStart;
                if (clickX > trackEnd) clickX = trackEnd;
                int newValue = ((clickX - trackStart) * 200) / trackWidth;
                if (newValue < 0) newValue = 0;
                if (newValue > 200) newValue = 200;
                float newScale = newValue / 100.0f;
                if (fabs(g_imageScale - newScale) > 0.001f) {
                    g_imageScale = newScale;
                    UpdateScaleControls();
                    g_renderRequestPending = true;
                }
            }
            break;
        }
        case WM_LBUTTONUP: {
            if (g_sliderDragging) {
                g_sliderDragging = false;
                ReleaseCapture();
                SaveAllSettingsToIni();
            }
            break;
        }
        case WM_UPDATE_MONITOR_NAMES:
            if (g_hMonitorList) {
                InvalidateRect(g_hMonitorList, NULL, TRUE);
                UpdateWindow(g_hMonitorList);
            }
            break;
    }
    return DefWindowProc(hwnd, uMsg, wParam, lParam);
}
int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int) {
    HANDLE hMutex = CreateMutexW(NULL, TRUE, L"CustomDesktopOverlayMutex");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        if (hMutex) CloseHandle(hMutex);
        HWND hExistingWnd = FindWindowW(L"Custom Desktop Overlay", NULL);
        if (hExistingWnd) {
            SendMessage(hExistingWnd, WM_SHOW_AND_MESSAGE, 0, 0);
        }
        return 0;
    }
    GdiplusStartupInput gdiplusStartupInput;
    GdiplusStartup(&g_gdiplusToken, &gdiplusStartupInput, NULL);
    g_hMainIcon = LoadIcon(hInstance, MAKEINTRESOURCE(IDI_MYICON));
    if (!g_hMainIcon) {
        g_hMainIcon = LoadIcon(NULL, IDI_APPLICATION);
    }
    INITCOMMONCONTROLSEX icex = {sizeof(INITCOMMONCONTROLSEX), ICC_TAB_CLASSES | ICC_LISTVIEW_CLASSES | ICC_BAR_CLASSES};
    InitCommonControlsEx(&icex);
    const wchar_t CLASS_NAME[] = L"Custom Desktop Overlay";
    WNDCLASS wc = {};
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = CLASS_NAME;
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hIcon = g_hMainIcon;
    if (!RegisterClass(&wc)) return 0;
    g_hwnd = CreateWindowEx(0, CLASS_NAME, L"Custom Desktop Overlay", WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, CW_USEDEFAULT, CW_USEDEFAULT, 300, 300, NULL, NULL, hInstance, NULL);
    if (!g_hwnd) return 0;
    ShowWindow(g_hwnd, SW_HIDE);
    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    StopRenderThread();
    SafeGdiplusShutdown();
    if (hMutex) {
        ReleaseMutex(hMutex);
        CloseHandle(hMutex);
    }
    return 0;
}