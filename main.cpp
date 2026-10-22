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
#include <shared_mutex>
#include <atomic>
#include <map>
#include <unordered_map>
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
#define ID_TRAY_NEXT_IMAGE 1005
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
#define ID_HOTKEY_CYCLE_DISPLAY 2030
#define ID_REVERSE_DIRECTION_CHECKBOX 2031
#define ID_RESET_CYCLE 2032
#define WM_UPDATE_MONITOR_NAMES (WM_APP + 1)
#define WM_SHOW_AND_MESSAGE (WM_APP + 2)
using namespace Gdiplus;
struct ImageSettings {
    int monitor = 0;
    int offsetX = 0;
    int offsetY = 0;
    float scale = 1.0f;
};
struct ImageFile {
    std::wstring path;
    std::wstring filename;
    ImageSettings settings;
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
    int selStart;
    bool selecting;
    UINT id;
    HWND hwnd;
    bool hover;
};
struct GdiObjectDeleter {
    void operator()(HGDIOBJ obj) const {
        if (obj) DeleteObject(obj);
    }
};
struct IconDeleter {
    void operator()(HICON icon) const {
        if (icon) DestroyIcon(icon);
    }
};
using UniqueHBrush = std::unique_ptr<std::remove_pointer<HBRUSH>::type, GdiObjectDeleter>;
using UniqueHIcon = std::unique_ptr<std::remove_pointer<HICON>::type, IconDeleter>;
using UniqueBitmap = std::unique_ptr<Bitmap>;
struct ButtonState {
    bool hover;
    bool pressed;
};
struct ListBoxState {
    int hoverIndex;
    int dragSource;
    int dragTarget;
    bool dragging;
    bool dragPending;
    POINT dragStart;
};
std::unordered_map<HWND, ButtonState> g_buttonStates;
std::unordered_map<HWND, ListBoxState> g_listBoxStates;
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
    std::vector<std::shared_ptr<Gdiplus::Bitmap>> m_scaledFrames;
    float m_scaledCacheScale;
    std::atomic<UINT> m_currentFrame;
    UINT m_width, m_height;
    UINT_PTR m_timerId;
    HWND m_hwndOverlay;
    bool m_isInitialized;
    std::mutex m_cacheMutex;
    void Cleanup() {
        m_frames.clear();
        if (m_pDecoder) {
            m_pDecoder->Release();
            m_pDecoder = NULL;
        }
        m_currentFrame = 0;
        m_width = m_height = 0;
        m_isInitialized = false;
        {
            std::lock_guard<std::mutex> lock(m_cacheMutex);
            m_scaledFrames.clear();
            m_scaledCacheScale = 0;
        }
    }
    std::unique_ptr<Gdiplus::Bitmap> CreateBitmapFromConverter(IWICFormatConverter* pConverter, UINT width, UINT height) {
        auto bitmap = std::make_unique<Gdiplus::Bitmap>(width, height, PixelFormat32bppPARGB);
        Gdiplus::BitmapData bmpData;
        Gdiplus::Rect rect(0, 0, width, height);
        if (bitmap->LockBits(&rect, Gdiplus::ImageLockModeWrite, PixelFormat32bppPARGB, &bmpData) == Gdiplus::Ok) {
            WICRect rc = {0, 0, (INT)width, (INT)height};
            HRESULT hr = pConverter->CopyPixels(&rc, bmpData.Stride, bmpData.Stride * height, (BYTE*)bmpData.Scan0);
            bitmap->UnlockBits(&bmpData);
            if (SUCCEEDED(hr)) return bitmap;
        }
        return nullptr;
    }
public:
    AnimatedGIF() : m_pFactory(NULL), m_pDecoder(NULL), m_currentFrame(0), m_width(0), m_height(0), m_timerId(0), m_hwndOverlay(NULL), m_isInitialized(false), m_scaledCacheScale(0) {
        CoCreateInstance(CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&m_pFactory));
    }
    ~AnimatedGIF() {
        StopAnimation();
        Cleanup();
        if (m_pFactory) m_pFactory->Release();
    }
    bool Load(const std::wstring& filename) {
        Cleanup();
        HRESULT hr = m_pFactory->CreateDecoderFromFilename(filename.c_str(), NULL, GENERIC_READ, WICDecodeMetadataCacheOnLoad, &m_pDecoder);
        if (FAILED(hr)) return false;
        UINT frameCount = 0;
        m_pDecoder->GetFrameCount(&frameCount);
        if (frameCount == 0) {
            Cleanup();
            return false;
        }
        IWICBitmapFrameDecode* pFirstFrame = NULL;
        hr = m_pDecoder->GetFrame(0, &pFirstFrame);
        if (FAILED(hr)) {
            Cleanup();
            return false;
        }
        pFirstFrame->GetSize(&m_width, &m_height);
        pFirstFrame->Release();
        auto composite = std::make_unique<Gdiplus::Bitmap>(m_width, m_height, PixelFormat32bppPARGB);
        {
            Gdiplus::Graphics g(composite.get());
            Gdiplus::SolidBrush clearBrush(Gdiplus::Color(0, 0, 0, 0));
            g.FillRectangle(&clearBrush, 0, 0, (INT)m_width, (INT)m_height);
        }
        m_frames.reserve(frameCount);
        for (UINT i = 0; 
            i < frameCount; ++i) {
            IWICBitmapFrameDecode* pFrame = NULL;
            hr = m_pDecoder->GetFrame(i, &pFrame);
            if (FAILED(hr)) 
                continue;
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
                pMeta = NULL;
            }
            IWICFormatConverter* pConverter = NULL;
            std::unique_ptr<Gdiplus::Bitmap> frameBitmap;
            hr = m_pFactory->CreateFormatConverter(&pConverter);
            if (SUCCEEDED(hr)) {
                hr = pConverter->Initialize(pFrame, GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, NULL, 0.0, WICBitmapPaletteTypeCustom);
                if (SUCCEEDED(hr)) {
                    UINT frameW, frameH;
                    pConverter->GetSize(&frameW, &frameH);
                    if (frameW > 0 && frameH > 0) frameBitmap = CreateBitmapFromConverter(pConverter, frameW, frameH);
                }
                pConverter->Release();
                pConverter = NULL;
            }
            pFrame->Release();
            pFrame = NULL;
            if (!frameBitmap) {
                frameBitmap = std::make_unique<Gdiplus::Bitmap>(m_width, m_height, PixelFormat32bppPARGB);
                Gdiplus::Graphics g(frameBitmap.get());
                Gdiplus::SolidBrush clear(Gdiplus::Color(0, 0, 0, 0));
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
                case 2: {
                    Gdiplus::Graphics g(composite.get());
                    g.SetCompositingMode(Gdiplus::CompositingModeSourceCopy);
                    Gdiplus::SolidBrush clear(Gdiplus::Color(0, 0, 0, 0));
                    g.FillRectangle(&clear, (INT)left, (INT)top, (INT)width, (INT)height);
                    break;
                }
                case 3: 
                    break;
                default: {
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
        if (!m_isInitialized || m_frames.empty()) 
            return;
        m_hwndOverlay = hwndOverlay;
        if (m_frames.size() > 1 && m_hwndOverlay) {
            if (m_timerId) 
                KillTimer(m_hwndOverlay, m_timerId);
            m_timerId = SetTimer(m_hwndOverlay, ID_ANIMATION_TIMER, m_frames[m_currentFrame.load()].delay, NULL);
        }
    }
    void StopAnimation() {
        if (m_timerId && m_hwndOverlay && IsWindow(m_hwndOverlay)) {
            KillTimer(m_hwndOverlay, m_timerId);
            m_timerId = 0;
        }
    }
    void NextFrame() {
        if (!m_isInitialized || m_frames.empty()) 
            return;
        m_currentFrame = (m_currentFrame + 1) % m_frames.size();
        if (m_timerId && m_hwndOverlay && IsWindow(m_hwndOverlay)) {
            KillTimer(m_hwndOverlay, m_timerId);
            m_timerId = SetTimer(m_hwndOverlay, ID_ANIMATION_TIMER, m_frames[m_currentFrame.load()].delay, NULL);
        }
    }
    std::shared_ptr<Gdiplus::Bitmap> GetScaledFrame(int index, float scale, int targetWidth, int targetHeight) {
        if (index < 0 || index >= (int)m_frames.size()) 
            return nullptr;
        std::lock_guard<std::mutex> lock(m_cacheMutex);
        if (m_scaledCacheScale != scale || m_scaledFrames.size() != m_frames.size()) {
            m_scaledFrames.clear();
            m_scaledFrames.resize(m_frames.size());
            m_scaledCacheScale = scale;
        }
        if (!m_scaledFrames[index]) {
            Bitmap* orig = m_frames[index].bitmap.get();
            auto scaled = std::make_shared<Bitmap>(targetWidth, targetHeight, PixelFormat32bppPARGB);
            Graphics g(scaled.get());
            g.SetInterpolationMode(InterpolationModeHighQualityBicubic);
            Rect destRect(0, 0, targetWidth, targetHeight);
            g.DrawImage(orig, destRect, 0, 0, orig->GetWidth(), orig->GetHeight(), UnitPixel);
            m_scaledFrames[index] = scaled;
        }
        return m_scaledFrames[index];
    }
    bool IsInitialized() const {
        return m_isInitialized; 
        }
    bool IsAnimated() const {
        return m_frames.size() > 1; 
        }
    int GetWidth() const {
        return (int)m_width; 
    }
    int GetHeight() const {
        return (int)m_height; 
    }
    UINT GetCurrentFrameIndex() const {
        return m_currentFrame.load(); 
    }
    void ClearCache() {
        std::lock_guard<std::mutex> lock(m_cacheMutex);
        m_scaledFrames.clear();
        m_scaledCacheScale = 0;
    }
};
void DrawOwnerDrawButton(HDC hdc, HWND hwnd, bool isPressed, const std::wstring& text, bool isActiveTab, bool isHovered);
void DrawOwnerDrawCheckbox(HDC hdc, HWND hwnd, bool isChecked, const std::wstring& text, bool isHovered);
void DrawOwnerDrawListBox(HDC hdc, HWND hwnd, int type, int hoverIndex);
void DrawOwnerDrawEdit(HDC hdc, HWND hwnd, EditField* field, bool darkTheme, bool isHovered);
LRESULT HandleOwnerDrawButton(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam, int buttonId);
LRESULT HandleOwnerDrawCheckbox(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
LRESULT HandleOwnerDrawListBox(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam, int listType);
LRESULT HandleOwnerDrawEdit(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam, EditField* field);
void UpdateAllButtons();
void AddImageToList(const std::wstring& filePath, bool saveToIni);
void SaveAllSettingsToIni();
void LoadAllSettingsFromIni();
void CreateOverlayWindow(const std::wstring& imagePath, bool saveToIni = true);
void UpdateScaleControls();
void UpdateOffsetEditControls();
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
bool ResetHotkeyField(int fieldId, wchar_t defaultKey, wchar_t& currentKey, wchar_t& lastValidKey, EditField& field, bool showConflictMessage = true);
void UpdateOffsetBoundsDisplay();
void CheckMonitorConfiguration();
void UpdateOverlayPosition();
void RenderOverlay();
void RenderOverlayInThread();
void ApplyScaleChange();
void CycleImageSelection();
void ToggleReverseDirection();
void SetCurrentImageSettings(int index);
void SaveCurrentImageSettings();
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
HWND g_hHotkeySettingsDisplay = NULL;
HWND g_hHotkeyCycleDisplay = NULL;
HWND g_hHotkeyQuitDisplay = NULL;
HWND g_hThemeButton = NULL;
HWND g_hTabButton1 = NULL;
HWND g_hTabButton2 = NULL;
HWND g_hTabButton3 = NULL;
HWND g_hResetToggle = NULL;
HWND g_hResetSettings = NULL;
HWND g_hResetCycle = NULL;
HWND g_hResetQuit = NULL;
HWND g_hReverseDirectionCheckbox = NULL;
HFONT g_hTabFont = NULL;
HFONT g_hMenuFont = NULL;
HICON g_hMainIcon = NULL;
UniqueHBrush g_hBackgroundBrush(nullptr);
UniqueHBrush g_hButtonBrush(nullptr);
UniqueHBrush g_hButtonPressedBrush(nullptr);
UniqueHBrush g_hButtonHoverBrush(nullptr);
UniqueHBrush g_hCheckboxBrush(nullptr);
UniqueHBrush g_hCheckboxHoverBrush(nullptr);
UniqueHBrush g_hActiveTabBrush(nullptr);
UniqueHBrush g_hMenuBackgroundBrush(nullptr);
UniqueHBrush g_hMenuSelectedBrush(nullptr);
UniqueHBrush g_hListBoxBackgroundBrush(nullptr);
UniqueHBrush g_hListBoxSelectedBrush(nullptr);
UniqueHBrush g_hListBoxHoverBrush(nullptr);
UniqueHBrush g_hEditHoverBrush(nullptr);
bool g_windowVisible = false;
bool g_hideOverlay = false;
std::atomic<float> g_imageScale{1.0f};
std::vector<ImageFile> g_imageFiles;
std::vector<MonitorInfo> g_monitors;
std::shared_mutex g_monitorMutex;
HWND g_currentOverlay = NULL;
std::atomic<HWND> g_currentOverlayAtomic{nullptr};
ULONG_PTR g_gdiplusToken = 0;
std::wstring g_lastDisplayedImage;
int g_selectedMonitor = 0;
int g_offsetX = 0;
int g_offsetY = 0;
int g_activeTab = 1;
bool g_darkTheme = false;
int g_currentImageIndex = -1;
#define DEFAULT_TOGGLE_KEY L'H'
#define DEFAULT_QUIT_KEY L'Q'
#define DEFAULT_SETTINGS_KEY L'S'
#define DEFAULT_CYCLE_KEY L'N'
wchar_t g_toggleHotkeyKey = DEFAULT_TOGGLE_KEY;
wchar_t g_quitHotkeyKey = DEFAULT_QUIT_KEY;
wchar_t g_settingsHotkeyKey = DEFAULT_SETTINGS_KEY;
wchar_t g_cycleHotkeyKey = DEFAULT_CYCLE_KEY;
int g_windowX = CW_USEDEFAULT;
int g_windowY = CW_USEDEFAULT;
COLORREF g_textColor = RGB(0, 0, 0);
COLORREF g_buttonTextColor = RGB(0, 0, 0);
COLORREF g_borderColor = RGB(200, 200, 200);
COLORREF g_buttonBorderColor = RGB(150, 150, 150);
COLORREF g_menuBackgroundColor = RGB(255, 255, 255);
COLORREF g_menuTextColor = RGB(0, 0, 0);
COLORREF g_menuSelectedColor = RGB(220, 220, 220);
wchar_t g_lastValidToggleKey = DEFAULT_TOGGLE_KEY;
wchar_t g_lastValidQuitKey = DEFAULT_QUIT_KEY;
wchar_t g_lastValidSettingsKey = DEFAULT_SETTINGS_KEY;
wchar_t g_lastValidCycleKey = DEFAULT_CYCLE_KEY;
bool g_reverseDirection = false;
int g_imageListSelected = -1;
int g_monitorListSelected = -1;
int g_imageListScrollPos = 0;
int g_monitorListScrollPos = 0;
const int ITEM_HEIGHT = 16;
const int VISIBLE_ITEMS_IMAGES = 9;
const int VISIBLE_ITEMS_MONITORS = 3;
EditField g_offsetXField = {L"0", false, 0, 0, false, ID_OFFSET_X_DISPLAY, NULL, false};
EditField g_offsetYField = {L"0", false, 0, 0, false, ID_OFFSET_Y_DISPLAY, NULL, false};
EditField g_sizeField = {L"1.00", false, 0, 0, false, ID_SIZE_DISPLAY, NULL, false};
EditField g_hotkeyToggleField = {L"H", false, 0, 0, false, ID_HOTKEY_TOGGLE_DISPLAY, NULL, false};
EditField g_hotkeySettingsField = {L"S", false, 0, 0, false, ID_HOTKEY_SETTINGS_DISPLAY, NULL, false};
EditField g_hotkeyCycleField = {L"N", false, 0, 0, false, ID_HOTKEY_CYCLE_DISPLAY, NULL, false};
EditField g_hotkeyQuitField = {L"Q", false, 0, 0, false, ID_HOTKEY_QUIT_DISPLAY, NULL, false};
static int g_prevMonitorCount = 0;
static std::vector<RECT> g_prevMonitorRects;
std::shared_ptr<Bitmap> g_cachedBitmapShared;
int g_originalImageWidth = 0;
int g_originalImageHeight = 0;
int g_currentMonitor = -1;
std::shared_ptr<AnimatedGIF> g_animatedGIFShared;
std::mutex g_renderDataMutex;
HANDLE g_renderEvent = NULL;
std::thread g_renderThread;
std::atomic<bool> g_renderThreadRunning(false);
std::atomic<bool> g_renderThreadShouldExit(false);
std::atomic<bool> g_monitorNamesLoaded{false};
std::atomic<bool> g_programExiting{false};
std::atomic<bool> g_monitorNamesThreadRunning{false};
std::thread g_monitorNamesThread;
bool g_settingsChanged = false;
std::atomic<int> g_overlayX = 0;
std::atomic<int> g_overlayY = 0;
WNDPROC g_oldImageListProc = NULL;
WNDPROC g_oldMonitorListProc = NULL;
WNDPROC g_oldOffsetXProc = NULL;
WNDPROC g_oldOffsetYProc = NULL;
WNDPROC g_oldSizeProc = NULL;
WNDPROC g_oldHotkeyToggleProc = NULL;
WNDPROC g_oldHotkeySettingsProc = NULL;
WNDPROC g_oldHotkeyCycleProc = NULL;
WNDPROC g_oldHotkeyQuitProc = NULL;
WNDPROC g_oldTabButton1Proc = NULL;
WNDPROC g_oldTabButton2Proc = NULL;
WNDPROC g_oldTabButton3Proc = NULL;
WNDPROC g_oldBrowseButtonProc = NULL;
WNDPROC g_oldThemeButtonProc = NULL;
WNDPROC g_oldResetToggleProc = NULL;
WNDPROC g_oldResetSettingsProc = NULL;
WNDPROC g_oldResetCycleProc = NULL;
WNDPROC g_oldResetQuitProc = NULL;
WNDPROC g_oldHideOverlayCheckboxProc = NULL;
WNDPROC g_oldReverseDirectionCheckboxProc = NULL;
LRESULT CALLBACK OverlayProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam);
std::wstring GetIniFilePath() {
    wchar_t exePath[MAX_PATH] = {0};
    GetModuleFileNameW(NULL, exePath, MAX_PATH);
    std::wstring exeDir = exePath;
    size_t lastSlash = exeDir.find_last_of(L"\\/");
    if (lastSlash != std::wstring::npos) 
        exeDir = exeDir.substr(0, lastSlash + 1);
    return exeDir + L"settings.ini";
}
std::map<std::wstring, std::wstring> GetDeviceIdMap() {
    std::map<std::wstring, std::wstring> deviceMap;
    DISPLAY_DEVICE ddAdapter, ddMonitor;
    ddAdapter.cb = sizeof(DISPLAY_DEVICE);
    ddMonitor.cb = sizeof(DISPLAY_DEVICE);
    for (DWORD adapter = 0; EnumDisplayDevices(NULL, adapter, &ddAdapter, 0); ++adapter) {
        for (DWORD monitor = 0; EnumDisplayDevices(ddAdapter.DeviceName, monitor, &ddMonitor, EDD_GET_DEVICE_INTERFACE_NAME); ++monitor) {
            if (ddMonitor.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP) 
                deviceMap[ddMonitor.DeviceName] = ddMonitor.DeviceID;
        }
    }
    return deviceMap;
}
BOOL CALLBACK MonitorEnumProc(HMONITOR hMonitor, HDC hdcMonitor, LPRECT lprcMonitor, LPARAM dwData) {
    static std::map<std::wstring, std::wstring> deviceMap = GetDeviceIdMap();
    MONITORINFOEX monitorInfo = {0};
    monitorInfo.cbSize = sizeof(monitorInfo);
    if (!GetMonitorInfo(hMonitor, &monitorInfo)) 
        return TRUE;
    MonitorInfo info;
    info.index = (int)g_monitors.size();
    info.handle = hMonitor;
    info.rect = *lprcMonitor;
    std::wstring deviceName = monitorInfo.szDevice;
    std::wstring key = deviceName + L"\\Monitor0";
    auto it = deviceMap.find(key);
    if (it != deviceMap.end()) 
        info.deviceID = it->second;
    DEVMODE devMode = {0};
    devMode.dmSize = sizeof(DEVMODE);
    if (EnumDisplaySettings(monitorInfo.szDevice, ENUM_CURRENT_SETTINGS, &devMode)) {
        info.name = std::to_wstring(devMode.dmPelsWidth) + L"x" + std::to_wstring(devMode.dmPelsHeight) + L" @" + std::to_wstring(devMode.dmDisplayFrequency) + L"Hz";
    }
    else {
        int w = info.rect.right - info.rect.left;
        int h = info.rect.bottom - info.rect.top;
        info.name = std::to_wstring(w) + L"x" + std::to_wstring(h);
    }
    g_monitors.push_back(info);
    return TRUE;
}
std::vector<std::pair<std::wstring, std::wstring>> GetMonitorFriendlyNamesPowerShellWithInstance() {
    std::vector<std::pair<std::wstring, std::wstring>> result;
    HANDLE hReadPipe, hWritePipe;
    SECURITY_ATTRIBUTES sa = {sizeof(SECURITY_ATTRIBUTES), NULL, TRUE };
    if (!CreatePipe(&hReadPipe, &hWritePipe, &sa, 0)) 
        return result;
    STARTUPINFOW si = {sizeof(STARTUPINFOW)};
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = hWritePipe;
    si.hStdError = hWritePipe;
    PROCESS_INFORMATION pi;
    std::wstring command = L"C:\\Windows\\System32\\WindowsPowerShell\\v1.0\\powershell.exe -Command \"[Console]::OutputEncoding = [System.Text.Encoding]::UTF8; Get-CimInstance -Namespace root/wmi -ClassName WmiMonitorID | ForEach-Object { $instance = $_.InstanceName; if ($_.UserFriendlyName) { $name = ($_.UserFriendlyName | ForEach-Object { [char]$_ }) -join '' } else { $parts = $instance.Split('\\\\'); if ($parts.Count -ge 3) { $name = $parts[1] } else { $name = 'Unknown' } }; Write-Output \\\"$instance`t$name\\\"; } \"";
    BOOL success = CreateProcessW(NULL, &command[0], NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi);
    CloseHandle(hWritePipe);
    if (success) {
        char buffer[4096];
        DWORD bytesRead;
        std::string resultStr;
        while (ReadFile(hReadPipe, buffer, sizeof(buffer)-1, &bytesRead, NULL) && bytesRead > 0) {
            buffer[bytesRead] = '\0';
            resultStr += buffer;
        }
        WaitForSingleObject(pi.hProcess, INFINITE);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        size_t pos = 0;
        while (pos < resultStr.size()) {
            size_t end = resultStr.find('\n', pos);
            if (end == std::wstring::npos) 
                end = resultStr.size();
            std::string line = resultStr.substr(pos, end - pos);
            if (!line.empty() && line.back() == '\r') 
                line.pop_back();
            if (!line.empty()) {
                size_t tabPos = line.find('\t');
                if (tabPos != std::string::npos) {
                    std::string instancePart = line.substr(0, tabPos);
                    std::string namePart = line.substr(tabPos + 1);
                    auto toWide = [](const std::string& s) -> std::wstring {
                        int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, NULL, 0);
                        if (len <= 0) 
                            return L"";
                        std::wstring w(len, 0);
                        MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &w[0], len);
                        w.pop_back();
                        if (!w.empty() && w[0] == 0xFEFF) 
                            w.erase(0, 1);
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
    if (start == std::wstring::npos) 
        return L"";
    start++;
    size_t end = instanceName.find(L'\\', start);
    if (end == std::wstring::npos) 
        return L"";
    return instanceName.substr(start, end - start);
}
std::wstring ExtractModelFromDeviceID(const std::wstring& deviceID) {
    size_t start = deviceID.find(L'#');
    if (start == std::wstring::npos) 
        return L"";
    start++;
    size_t end = deviceID.find(L'#', start);
    if (end == std::wstring::npos) 
        return L"";
    return deviceID.substr(start, end - start);
}
void LoadMonitorNamesThread() {
    if (g_programExiting) 
        return;
    auto psPairs = GetMonitorFriendlyNamesPowerShellWithInstance();
    std::map<std::wstring, std::wstring> modelToName;
    for (const auto& pair : psPairs) {
        std::wstring model = ExtractModelFromInstanceName(pair.first);
        if (!model.empty()) 
            modelToName[model] = pair.second;
    }
    {
        std::unique_lock<std::shared_mutex> lock(g_monitorMutex);
        for (auto& mon : g_monitors) {
            if (!mon.deviceID.empty()) {
                std::wstring model = ExtractModelFromDeviceID(mon.deviceID);
                if (!model.empty()) {
                    auto it = modelToName.find(model);
                    if (it != modelToName.end()) 
                        mon.friendlyName = it->second;
                }
            }
        }
        g_monitorNamesLoaded = true;
    }
    if (g_programExiting) 
        return;
    PostMessage(g_hwnd, WM_UPDATE_MONITOR_NAMES, 0, 0);
    g_monitorNamesThreadRunning = false;
}
void ForceForegroundWindow(HWND hwnd) {
    DWORD foreThread = GetWindowThreadProcessId(GetForegroundWindow(), NULL);
    DWORD appThread = GetCurrentThreadId();
    if (foreThread != appThread) {
        AttachThreadInput(foreThread, appThread, TRUE);
        SetForegroundWindow(hwnd);
        AttachThreadInput(foreThread, appThread, FALSE);
    }
    else SetForegroundWindow(hwnd);
    BringWindowToTop(hwnd);
    SetWindowPos(hwnd, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
}
void UpdateMonitorList() {
    {
        std::unique_lock<std::shared_mutex> lock(g_monitorMutex);
        g_monitors.clear();
        g_monitorNamesLoaded = false;
        EnumDisplayMonitors(NULL, NULL, MonitorEnumProc, 0);
    }
    if (!g_monitorNamesThreadRunning.exchange(true)) {
        if (g_monitorNamesThread.joinable()) 
            g_monitorNamesThread.join();
        g_monitorNamesThread = std::thread(LoadMonitorNamesThread);
    }
    {
        std::shared_lock<std::shared_mutex> lock(g_monitorMutex);
        if (g_selectedMonitor >= (int)g_monitors.size()) {
            g_selectedMonitor = 0;
            g_monitorListSelected = 0;
        }
        else g_monitorListSelected = g_selectedMonitor;
    }
    if (g_hMonitorList) 
        InvalidateRect(g_hMonitorList, NULL, TRUE);
    UpdateOffsetBoundsDisplay();
}
bool IsMessageBoxWithTextOpen(const std::wstring& text) {
    HWND hWnd = NULL;
    while ((hWnd = FindWindowEx(NULL, hWnd, L"OwnerDrawMessageBox", NULL)) != NULL) {
        HANDLE hProp = GetProp(hWnd, L"MessageText");
        if (hProp && *(std::wstring*)hProp == text) 
            return true;
    }
    return false;
}
int GetOpenMessageBoxCount() {
    int count = 0;
    HWND hWnd = NULL;
    while ((hWnd = FindWindowEx(NULL, hWnd, L"OwnerDrawMessageBox", NULL)) != NULL) 
        count++;
    return count;
}
void ActivateNextMessageBox(HWND hCurrent) {
    HWND hWnd = NULL;
    while ((hWnd = FindWindowEx(NULL, hWnd, L"OwnerDrawMessageBox", NULL)) != NULL) {
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
    std::vector<MonitorInfo> currentMonitors;
    {
        std::shared_lock<std::shared_mutex> lock(g_monitorMutex);
        currentMonitors = g_monitors;
    }
    std::vector<MonitorInfo> newMonitors;
    EnumDisplayMonitors(NULL, NULL, [](HMONITOR hMon, HDC, LPRECT rc, LPARAM lp) -> BOOL {
        auto* vec = reinterpret_cast<std::vector<MonitorInfo>*>(lp);
        MONITORINFOEX mi = {sizeof(mi)};
        if (GetMonitorInfo(hMon, &mi)) {
            MonitorInfo info;
            info.handle = hMon;
            info.rect = *rc;
            info.name = L"";
            vec->push_back(info);
        }
        return TRUE;
    }, (LPARAM)&newMonitors);
    bool configChanged = false;
    if (newMonitors.size() != currentMonitors.size()) {
        configChanged = true;
    }
    else {
        for (size_t i = 0; i < newMonitors.size(); ++i) {
            if (memcmp(&newMonitors[i].rect, &currentMonitors[i].rect, sizeof(RECT)) != 0) {
                configChanged = true;
                break;
            }
        }
    }
    if (configChanged) {
        if (g_hMonitorList) InvalidateRect(g_hMonitorList, NULL, TRUE);
        {
            std::shared_lock<std::shared_mutex> lock(g_monitorMutex);
            if (g_selectedMonitor >= (int)g_monitors.size()) {
                g_selectedMonitor = 0;
                g_monitorListSelected = 0;
            }
        }
        CorrectOffsetValues();
        UpdateOffsetBoundsDisplay();
        if (g_currentOverlay) {
            if (g_selectedMonitor != g_currentMonitor) {
                UpdateOverlayPosition();
                RenderOverlay();
            }
        }
        g_prevMonitorCount = (int)newMonitors.size();
        g_prevMonitorRects.clear();
        for (const auto& m : newMonitors) 
            g_prevMonitorRects.push_back(m.rect);
    }
}
bool IsSupportedImageFormat(const std::wstring& filePath) {
    size_t dotPos = filePath.find_last_of(L".");
    if (dotPos == std::wstring::npos) 
        return false;
    std::wstring ext = filePath.substr(dotPos + 1);
    for (wchar_t& c : ext) 
        c = towlower(c);
    return ext == L"png" || ext == L"jpg" || ext == L"jpeg" || ext == L"gif";
}
void DrawOwnerDrawButton(HDC hdc, HWND hwnd, bool isPressed, const std::wstring& text, bool isActiveTab, bool isHovered) {
    RECT rc;
    GetClientRect(hwnd, &rc);
    HBRUSH hBrush;
    if (isActiveTab) 
        hBrush = g_hActiveTabBrush.get();
    else if (isPressed) 
        hBrush = g_hButtonPressedBrush.get();
    else if (isHovered) 
        hBrush = g_hButtonHoverBrush.get();
    else hBrush = g_hButtonBrush.get();
    FillRect(hdc, &rc, hBrush);
    HPEN hBorderPen = CreatePen(PS_SOLID, 1, g_buttonBorderColor);
    HPEN hOldPen = (HPEN)SelectObject(hdc, hBorderPen);
    HBRUSH hOldBrush = (HBRUSH)SelectObject(hdc, GetStockObject(NULL_BRUSH));
    Rectangle(hdc, rc.left, rc.top, rc.right, rc.bottom);
    SelectObject(hdc, hOldBrush);
    SelectObject(hdc, hOldPen);
    DeleteObject(hBorderPen);
    if (isPressed && !isActiveTab) 
        OffsetRect(&rc, 1, 1);
    SetTextColor(hdc, g_buttonTextColor);
    SetBkMode(hdc, TRANSPARENT);
    HFONT hOldFont = (HFONT)SelectObject(hdc, g_hTabFont);
    DrawText(hdc, text.c_str(), -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    SelectObject(hdc, hOldFont);
}
void DrawOwnerDrawCheckbox(HDC hdc, HWND hwnd, bool isChecked, const std::wstring& text, bool isHovered) {
    RECT rc;
    GetClientRect(hwnd, &rc);
    FillRect(hdc, &rc, g_hBackgroundBrush.get());
    int checkboxSize = 13;
    int textMargin = 5;
    int checkboxX = rc.left + 2;
    int checkboxY = rc.top + (rc.bottom - rc.top - checkboxSize) / 2;
    RECT checkboxRect = {checkboxX, checkboxY, checkboxX + checkboxSize, checkboxY + checkboxSize};
    HBRUSH hCheckBrush = isHovered ? g_hCheckboxHoverBrush.get() : g_hCheckboxBrush.get();
    FillRect(hdc, &checkboxRect, hCheckBrush);
    HPEN hBorderPen = CreatePen(PS_SOLID, 1, g_darkTheme ? RGB(100,100,100) : RGB(150,150,150));
    HPEN hOldPen = (HPEN)SelectObject(hdc, hBorderPen);
    HBRUSH hOldBrush = (HBRUSH)SelectObject(hdc, GetStockObject(NULL_BRUSH));
    Rectangle(hdc, checkboxRect.left, checkboxRect.top, checkboxRect.right, checkboxRect.bottom);
    SelectObject(hdc, hOldBrush);
    SelectObject(hdc, hOldPen);
    DeleteObject(hBorderPen);
    if (isChecked) {
        COLORREF checkColor = g_darkTheme ? RGB(220,220,220) : RGB(50,50,50);
        HPEN hCheckPen = CreatePen(PS_SOLID, 2, checkColor);
        hOldPen = (HPEN)SelectObject(hdc, hCheckPen);
        MoveToEx(hdc, checkboxX + 3, checkboxY + checkboxSize/2, NULL);
        LineTo(hdc, checkboxX + checkboxSize/2, checkboxY + checkboxSize - 3);
        LineTo(hdc, checkboxX + checkboxSize - 3, checkboxY + 2);
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
void DrawOwnerDrawListBox(HDC hdc, HWND hwnd, int listType, int hoverIndex) {
    RECT rc;
    GetClientRect(hwnd, &rc);
    FillRect(hdc, &rc, g_hListBoxBackgroundBrush.get());
    HPEN hBorderPen = CreatePen(PS_SOLID, 1, g_darkTheme ? RGB(80,80,80) : RGB(150,150,150));
    HPEN hOldPen = (HPEN)SelectObject(hdc, hBorderPen);
    HBRUSH hOldBrush = (HBRUSH)SelectObject(hdc, GetStockObject(NULL_BRUSH));
    Rectangle(hdc, rc.left, rc.top, rc.right, rc.bottom);
    SelectObject(hdc, hOldBrush);
    SelectObject(hdc, hOldPen);
    DeleteObject(hBorderPen);
    SetTextColor(hdc, g_textColor);
    SetBkMode(hdc, TRANSPARENT);
    HFONT hOldFont = (HFONT)SelectObject(hdc, g_hTabFont);
    if (listType == ID_IMAGE_LIST) {
        int total = (int)g_imageFiles.size();
        int startIdx = g_imageListScrollPos;
        int visibleItems = VISIBLE_ITEMS_IMAGES;
        ListBoxState& state = g_listBoxStates[hwnd];
        if (state.dragging && state.dragSource != -1) {
            std::vector<int> displayIndices;
            displayIndices.reserve(total);
            for (int i = 0; i < total; ++i)
                if (i != state.dragSource)
                    displayIndices.push_back(i);
            int insertPos = state.dragTarget;
            if (insertPos < 0) 
                insertPos = 0;
            if (insertPos > (int)displayIndices.size()) 
                insertPos = (int)displayIndices.size();
            displayIndices.insert(displayIndices.begin() + insertPos, -1);
            for (int v = 0; v < (int)displayIndices.size(); ++v) {
                if (v < startIdx) 
                    continue;
                if (v >= startIdx + visibleItems) 
                    break;
                RECT itemRect = {rc.left + 1, rc.top + 1 + (v - startIdx) * ITEM_HEIGHT, rc.right - 1, rc.top + 1 + (v - startIdx + 1) * ITEM_HEIGHT};
                int realIdx = displayIndices[v];
                if (realIdx == -1) {
                    FillRect(hdc, &itemRect, g_hListBoxSelectedBrush.get());
                    RECT textRect = itemRect;
                    textRect.left += 2;
                    std::wstring dragText = std::to_wstring(v + 1) + L". " + g_imageFiles[state.dragSource].filename;
                    DrawText(hdc, dragText.c_str(), -1, &textRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
                }
                else {
                    HBRUSH bgBrush = g_hListBoxBackgroundBrush.get();
                    if (realIdx == g_imageListSelected)
                        bgBrush = g_hListBoxSelectedBrush.get();
                    else if (realIdx == state.hoverIndex && !state.dragging)
                        bgBrush = g_hListBoxHoverBrush.get();
                    FillRect(hdc, &itemRect, bgBrush);
                    RECT textRect = itemRect;
                    textRect.left += 2;
                    std::wstring displayText = std::to_wstring(v + 1) + L". " + g_imageFiles[realIdx].filename;
                    DrawText(hdc, displayText.c_str(), -1, &textRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
                }
            }
            if (g_currentImageIndex != -1) {
                int targetRealIdx = g_currentImageIndex;
                int targetV = -1;
                for (int v = 0; v < (int)displayIndices.size(); ++v) {
                    if (v < startIdx) 
                        continue;
                    if (v >= startIdx + visibleItems) 
                        break;
                    if (displayIndices[v] == targetRealIdx) {
                        targetV = v;
                        break;
                    }
                }
                if (targetV == -1 && targetRealIdx == state.dragSource) {
                    for (int v = 0; v < (int)displayIndices.size(); ++v) {
                        if (v < startIdx) 
                            continue;
                        if (v >= startIdx + visibleItems) 
                            break;
                        if (displayIndices[v] == -1) {
                            targetV = v;
                            break;
                        }
                    }
                }
                if (targetV != -1) {
                    RECT itemRect = {rc.left + 1, rc.top + 1 + (targetV - startIdx) * ITEM_HEIGHT, rc.right - 1, rc.top + 1 + (targetV - startIdx + 1) * ITEM_HEIGHT};
                    HPEN hDashPen = CreatePen(PS_DOT, 1, g_darkTheme ? RGB(150,150,150) : RGB(100,100,100));
                    HPEN hOldPen2 = (HPEN)SelectObject(hdc, hDashPen);
                    HBRUSH hOldBrush2 = (HBRUSH)SelectObject(hdc, GetStockObject(NULL_BRUSH));
                    Rectangle(hdc, itemRect.left, itemRect.top, itemRect.right, itemRect.bottom);
                    SelectObject(hdc, hOldBrush2);
                    SelectObject(hdc, hOldPen2);
                    DeleteObject(hDashPen);
                }
            }
        }
        else {
            int endIdx = std::min(startIdx + visibleItems, total);
            for (int i = startIdx; i < endIdx; i++) {
                RECT itemRect = {rc.left + 1, rc.top + 1 + (i - startIdx) * ITEM_HEIGHT, rc.right - 1, rc.top + 1 + (i - startIdx + 1) * ITEM_HEIGHT};
                HBRUSH bgBrush;
                if (i == g_imageListSelected)
                    bgBrush = g_hListBoxSelectedBrush.get();
                else if (i == state.hoverIndex)
                    bgBrush = g_hListBoxHoverBrush.get();
                else
                    bgBrush = g_hListBoxBackgroundBrush.get();
                FillRect(hdc, &itemRect, bgBrush);
                RECT textRect = itemRect;
                textRect.left += 2;
                std::wstring displayText = std::to_wstring(i + 1) + L". " + g_imageFiles[i].filename;
                DrawText(hdc, displayText.c_str(), -1, &textRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            }
            if (g_currentImageIndex >= startIdx && g_currentImageIndex < endIdx) {
                int i = g_currentImageIndex;
                RECT itemRect = {rc.left + 1, rc.top + 1 + (i - startIdx) * ITEM_HEIGHT, rc.right - 1, rc.top + 1 + (i - startIdx + 1) * ITEM_HEIGHT};
                HPEN hDashPen = CreatePen(PS_DOT, 1, g_darkTheme ? RGB(150,150,150) : RGB(100,100,100));
                HPEN hOldPen2 = (HPEN)SelectObject(hdc, hDashPen);
                HBRUSH hOldBrush2 = (HBRUSH)SelectObject(hdc, GetStockObject(NULL_BRUSH));
                Rectangle(hdc, itemRect.left, itemRect.top, itemRect.right, itemRect.bottom);
                SelectObject(hdc, hOldBrush2);
                SelectObject(hdc, hOldPen2);
                DeleteObject(hDashPen);
            }
        }
    }
    else if (listType == ID_MONITOR_LIST) {
        int startIdx = g_monitorListScrollPos;
        int endIdx = std::min(startIdx + VISIBLE_ITEMS_MONITORS, (int)g_monitors.size());
        ListBoxState& state = g_listBoxStates[hwnd];
        for (int i = startIdx; i < endIdx; i++) {
            RECT itemRect = {rc.left+1, rc.top+1 + (i-startIdx)*ITEM_HEIGHT, rc.right-1, rc.top+1 + (i-startIdx+1)*ITEM_HEIGHT};
            HBRUSH bgBrush;
            if (i == g_monitorListSelected)
                bgBrush = g_hListBoxSelectedBrush.get();
            else if (i == state.hoverIndex)
                bgBrush = g_hListBoxHoverBrush.get();
            else
                bgBrush = g_hListBoxBackgroundBrush.get();
            FillRect(hdc, &itemRect, bgBrush);
            std::wstring displayText = std::to_wstring(i+1) + L". ";
            if (!g_monitors[i].friendlyName.empty())
                displayText += g_monitors[i].friendlyName + L" (";
            displayText += g_monitors[i].name;
            if (!g_monitors[i].friendlyName.empty())
                displayText += L")";
            RECT textRect = itemRect;
            textRect.left += 2;
            DrawText(hdc, displayText.c_str(), -1, &textRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        }
        if (g_selectedMonitor >= startIdx && g_selectedMonitor < endIdx) {
            int i = g_selectedMonitor;
            RECT itemRect = {rc.left + 1, rc.top + 1 + (i - startIdx) * ITEM_HEIGHT, rc.right - 1, rc.top + 1 + (i - startIdx + 1) * ITEM_HEIGHT};
            HPEN hDashPen = CreatePen(PS_DOT, 1, g_darkTheme ? RGB(150,150,150) : RGB(100,100,100));
            HPEN hOldPen2 = (HPEN)SelectObject(hdc, hDashPen);
            HBRUSH hOldBrush2 = (HBRUSH)SelectObject(hdc, GetStockObject(NULL_BRUSH));
            Rectangle(hdc, itemRect.left, itemRect.top, itemRect.right, itemRect.bottom);
            SelectObject(hdc, hOldBrush2);
            SelectObject(hdc, hOldPen2);
            DeleteObject(hDashPen);
        }
    }
    SelectObject(hdc, hOldFont);
}
int GetCharIndexFromEdit(HWND hwnd, const std::wstring& text, POINT pt) {
    RECT rc;
    GetClientRect(hwnd, &rc);
    rc.left += 2;
    rc.right -= 2;
    if (pt.x < rc.left) 
        return 0;
    if (pt.x > rc.right) 
        return (int)text.length();
    HDC hdc = GetDC(hwnd);
    HFONT hOldFont = (HFONT)SelectObject(hdc, g_hTabFont);
    int xRel = pt.x - rc.left;
    int index = (int)text.length();
    std::vector<int> cum(text.length()+1, 0);
    for (size_t i = 1; i <= text.length(); ++i) {
        std::wstring prefix = text.substr(0, i);
        SIZE sz;
        GetTextExtentPoint32(hdc, prefix.c_str(), (int)prefix.length(), &sz);
        cum[i] = sz.cx;
        if (xRel <= cum[i]) {
            index = (int)i;
            break;
        }
    }
    SelectObject(hdc, hOldFont);
    ReleaseDC(hwnd, hdc);
    return index;
}
void DrawOwnerDrawEdit(HDC hdc, HWND hwnd, EditField* field, bool darkTheme, bool isHovered) {
    RECT rc;
    GetClientRect(hwnd, &rc);
    HBRUSH bgBrush = (isHovered && !field->hasFocus) ? g_hEditHoverBrush.get() : g_hListBoxBackgroundBrush.get();
    FillRect(hdc, &rc, bgBrush);
    if (field->hasFocus && field->selStart != field->cursorPos) {
        int selMin = std::min(field->selStart, field->cursorPos);
        int selMax = std::max(field->selStart, field->cursorPos);
        std::wstring prefixMin = field->text.substr(0, selMin);
        std::wstring prefixMax = field->text.substr(0, selMax);
        HFONT hOldFont = (HFONT)SelectObject(hdc, g_hTabFont);
        SIZE sizeMin, sizeMax;
        GetTextExtentPoint32(hdc, prefixMin.c_str(), (int)prefixMin.length(), &sizeMin);
        GetTextExtentPoint32(hdc, prefixMax.c_str(), (int)prefixMax.length(), &sizeMax);
        int leftX = rc.left + 2 + sizeMin.cx;
        int rightX = rc.left + 2 + sizeMax.cx;
        RECT selRect = {leftX, rc.top+1, rightX, rc.bottom-1};
        FillRect(hdc, &selRect, g_hListBoxSelectedBrush.get());
        SelectObject(hdc, hOldFont);
    }
    COLORREF borderColor = darkTheme ? RGB(80,80,80) : RGB(150,150,150);
    if (field->hasFocus) 
        borderColor = darkTheme ? RGB(255,255,255) : RGB(0,0,0);
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
        GetTextExtentPoint32(hdc, prefix.c_str(), (int)prefix.length(), &size);
        int cursorX = textRect.left + size.cx;
        HPEN hCursorPen = CreatePen(PS_SOLID, 1, darkTheme ? RGB(240,240,240) : RGB(0,0,0));
        HPEN hOldPen2 = (HPEN)SelectObject(hdc, hCursorPen);
        MoveToEx(hdc, cursorX, textRect.top + 2, NULL);
        LineTo(hdc, cursorX, textRect.bottom - 2);
        SelectObject(hdc, hOldPen2);
        DeleteObject(hCursorPen);
    }
    SelectObject(hdc, hOldFont);
}
void DrawMenuItem(HDC hdc, RECT rc, const std::wstring& text, bool isSelected, bool isChecked, bool isDisabled, bool isTrayHideOverlay) {
    bool isTrayMenuItem = isTrayHideOverlay || text == L"Settings" || text == L"Quit" || text == L"Next Image";
    HBRUSH bgBrush = isSelected ? g_hMenuSelectedBrush.get() : g_hMenuBackgroundBrush.get();
    FillRect(hdc, &rc, bgBrush);
    if (isTrayMenuItem && isTrayHideOverlay) {
        int checkSize = 13;
        int checkX = rc.left + 5;
        int checkY = rc.top + (rc.bottom - rc.top - checkSize)/2;
        RECT checkRect = {checkX, checkY, checkX+checkSize, checkY+checkSize};
        HPEN hCheckBorder = CreatePen(PS_SOLID, 1, g_darkTheme ? RGB(100,100,100) : RGB(180,180,180));
        HPEN hOldPen = (HPEN)SelectObject(hdc, hCheckBorder);
        HBRUSH hOldBrush = (HBRUSH)SelectObject(hdc, GetStockObject(NULL_BRUSH));
        Rectangle(hdc, checkRect.left, checkRect.top, checkRect.right, checkRect.bottom);
        SelectObject(hdc, hOldBrush);
        SelectObject(hdc, hOldPen);
        DeleteObject(hCheckBorder);
        HBRUSH hCheckboxBrush = CreateSolidBrush(g_darkTheme ? RGB(60,60,60) : RGB(220,220,220));
        FillRect(hdc, &checkRect, hCheckboxBrush);
        DeleteObject(hCheckboxBrush);
        if (isChecked) {
            HPEN hCheckPen = CreatePen(PS_SOLID, 2, isDisabled ? (g_darkTheme ? RGB(100,100,100) : RGB(180,180,180)) : g_menuTextColor);
            hOldPen = (HPEN)SelectObject(hdc, hCheckPen);
            MoveToEx(hdc, checkX+3, checkY+checkSize/2, NULL);
            LineTo(hdc, checkX+checkSize/2, checkY+checkSize-3);
            LineTo(hdc, checkX+checkSize-3, checkY+2);
            SelectObject(hdc, hOldPen);
            DeleteObject(hCheckPen);
        }
    }
    SetTextColor(hdc, isDisabled ? (g_darkTheme ? RGB(120,120,120) : RGB(150,150,150)) : g_menuTextColor);
    SetBkMode(hdc, TRANSPARENT);
    RECT textRect = rc;
    textRect.left = rc.left + (isTrayMenuItem ? 25 : 10);
    textRect.right = rc.right - 10;
    HFONT hOldFont = (HFONT)SelectObject(hdc, g_hMenuFont);
    DrawText(hdc, text.c_str(), -1, &textRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    SelectObject(hdc, hOldFont);
}
void CreateOwnerDrawMenu(HMENU hMenu, int position, UINT id, const wchar_t* text) {
    MENUITEMINFO mii = {sizeof(mii)};
    mii.fMask = MIIM_ID | MIIM_TYPE | MIIM_STATE | MIIM_DATA;
    mii.fType = MFT_OWNERDRAW;
    mii.wID = id;
    mii.dwTypeData = (LPWSTR)text;
    mii.dwItemData = (ULONG_PTR)text;
    InsertMenuItem(hMenu, position, TRUE, &mii);
}
void UpdateAllButtons() {
    if (g_hTabButton1) 
        InvalidateRect(g_hTabButton1, NULL, TRUE);
    if (g_hTabButton2) 
        InvalidateRect(g_hTabButton2, NULL, TRUE);
    if (g_hTabButton3) 
        InvalidateRect(g_hTabButton3, NULL, TRUE);
    if (g_hBrowseButton) 
        InvalidateRect(g_hBrowseButton, NULL, TRUE);
    if (g_hThemeButton) 
        InvalidateRect(g_hThemeButton, NULL, TRUE);
    if (g_hHideOverlayCheckbox) 
        InvalidateRect(g_hHideOverlayCheckbox, NULL, TRUE);
}
LRESULT HandleOwnerDrawButton(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam, int buttonId) {
    switch (msg) {
        case WM_NCDESTROY:
            g_buttonStates.erase(hwnd);
            break;
        case WM_MOUSEMOVE: {
            TRACKMOUSEEVENT tme = {sizeof(TRACKMOUSEEVENT), TME_LEAVE, hwnd, 0};
            TrackMouseEvent(&tme);
            ButtonState& state = g_buttonStates[hwnd];
            if (!state.hover) {
                state.hover = true;
                InvalidateRect(hwnd, NULL, TRUE);
            }
            break;
        }
        case WM_MOUSELEAVE: {
            ButtonState& state = g_buttonStates[hwnd];
            if (state.hover) {
                state.hover = false;
                InvalidateRect(hwnd, NULL, TRUE);
            }
            break;
        }
        case WM_LBUTTONDOWN: {
            SetFocus(GetParent(hwnd));
            ButtonState& state = g_buttonStates[hwnd];
            state.pressed = true;
            SetCapture(hwnd);
            InvalidateRect(hwnd, NULL, TRUE);
            break;
        }
        case WM_LBUTTONUP: {
            ButtonState& state = g_buttonStates[hwnd];
            if (state.pressed) {
                state.pressed = false;
                ReleaseCapture();
                InvalidateRect(hwnd, NULL, TRUE);
                POINT pt = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
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
            bool isActiveTab = (buttonId == ID_TAB_BUTTON1 && g_activeTab == 1) || (buttonId == ID_TAB_BUTTON2 && g_activeTab == 2) || (buttonId == ID_TAB_BUTTON3 && g_activeTab == 3);
            ButtonState& state = g_buttonStates[hwnd];
            DrawOwnerDrawButton(hdc, hwnd, state.pressed, buffer, isActiveTab, state.hover && !state.pressed && !isActiveTab);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_LBUTTONDBLCLK:
            SetCapture(hwnd);
            g_buttonStates[hwnd].pressed = true;
            InvalidateRect(hwnd, NULL, TRUE);
            return 0;
        case WM_ERASEBKGND:
            return 1;
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}
LRESULT HandleOwnerDrawCheckbox(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    static std::unordered_map<HWND, bool> hoverState;
    switch (msg) {
        case WM_NCDESTROY:
            hoverState.erase(hwnd);
            break;
        case WM_MOUSEMOVE: {
            TRACKMOUSEEVENT tme = {sizeof(TRACKMOUSEEVENT), TME_LEAVE, hwnd, 0};
            TrackMouseEvent(&tme);
            if (!hoverState[hwnd]) {
                hoverState[hwnd] = true;
                InvalidateRect(hwnd, NULL, TRUE);
            }
            break;
        }
        case WM_MOUSELEAVE: {
            if (hoverState[hwnd]) {
                hoverState[hwnd] = false;
                InvalidateRect(hwnd, NULL, TRUE);
            }
            break;
        }
        case WM_LBUTTONDOWN:
            SetFocus(GetParent(hwnd));
            InvalidateRect(hwnd, NULL, TRUE);
            SetCapture(hwnd);
            break;
        case WM_LBUTTONUP:
            InvalidateRect(hwnd, NULL, TRUE);
            ReleaseCapture();
            if (GetDlgCtrlID(hwnd) == ID_HIDE_OVERLAY_CHECKBOX) 
                ToggleHideOverlay();
            else if (GetDlgCtrlID(hwnd) == ID_REVERSE_DIRECTION_CHECKBOX) 
                ToggleReverseDirection();
            break;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);
            wchar_t buffer[256];
            GetWindowText(hwnd, buffer, 256);
            std::wstring text = buffer;
            if (!text.empty() && text[0] == L' ') 
                text = text.substr(1);
            bool isChecked = (GetDlgCtrlID(hwnd) == ID_HIDE_OVERLAY_CHECKBOX) ? g_hideOverlay : g_reverseDirection;
            DrawOwnerDrawCheckbox(hdc, hwnd, isChecked, text, hoverState[hwnd]);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_LBUTTONDBLCLK:
            SetCapture(hwnd);
            InvalidateRect(hwnd, NULL, TRUE);
            return 0;
        case WM_ERASEBKGND:
            return 1;
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}
LRESULT HandleOwnerDrawListBox(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam, int listType) {
    ListBoxState& state = g_listBoxStates[hwnd];
    switch (msg) {
        case WM_NCDESTROY:
            g_listBoxStates.erase(hwnd);
            break;
        case WM_MOUSEMOVE: {
            TRACKMOUSEEVENT tme = {sizeof(TRACKMOUSEEVENT), TME_LEAVE, hwnd, 0};
            TrackMouseEvent(&tme);
            int yPos = GET_Y_LPARAM(lParam);
            int visibleItems = (listType == ID_IMAGE_LIST) ? VISIBLE_ITEMS_IMAGES : VISIBLE_ITEMS_MONITORS;
            int startIdx = (listType == ID_IMAGE_LIST) ? g_imageListScrollPos : g_monitorListScrollPos;
            int itemIndex = startIdx + (yPos - 2) / ITEM_HEIGHT;
            int itemCount = (listType == ID_IMAGE_LIST) ? (int)g_imageFiles.size() : (int)g_monitors.size();
            int newHover = -1;
            if (itemIndex >= 0 && itemIndex < itemCount && itemIndex >= startIdx && itemIndex < startIdx + visibleItems)
                newHover = itemIndex;
            if (state.hoverIndex != newHover && !state.dragging) {
                state.hoverIndex = newHover;
                InvalidateRect(hwnd, NULL, TRUE);
            }
            if (listType == ID_IMAGE_LIST) {
                if (state.dragPending) {
                    POINT pt = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
                    int dx = pt.x - state.dragStart.x;
                    int dy = pt.y - state.dragStart.y;
                    if (abs(dx) > 5 || abs(dy) > 5) {
                        state.dragging = true;
                        state.dragPending = false;
                        g_imageListSelected = -1;
                        InvalidateRect(hwnd, NULL, TRUE);
                    }
                }
                if (state.dragging) {
                    int total = (int)g_imageFiles.size();
                    int yPos = GET_Y_LPARAM(lParam);
                    int visibleItems = VISIBLE_ITEMS_IMAGES;
                    int scrollPos = g_imageListScrollPos;
                    std::vector<int> displayIndices;
                    displayIndices.reserve(total);
                    for (int i = 0; i < total; ++i)
                        if (i != state.dragSource)
                            displayIndices.push_back(i);
                    int insertPos = state.dragTarget;
                    if (insertPos < 0) 
                        insertPos = 0;
                    if (insertPos > (int)displayIndices.size()) 
                        insertPos = (int)displayIndices.size();
                    displayIndices.insert(displayIndices.begin() + insertPos, -1);
                    int line = (yPos - 2) / ITEM_HEIGHT;
                    int newTarget = state.dragTarget;
                    if (yPos < 2) {
                        newTarget = 0;
                    }
                    else if (yPos >= 2 + visibleItems * ITEM_HEIGHT) {
                        newTarget = total;
                    }
                    else {
                        int idx = scrollPos + line;
                        if (idx >= 0 && idx < (int)displayIndices.size()) {
                            int elem = displayIndices[idx];
                            int midY = 2 + line * ITEM_HEIGHT + ITEM_HEIGHT / 2;
                            if (elem == -1) {
                                newTarget = idx;
                            }
                            else {
                                if (yPos < midY) {
                                    newTarget = idx;
                                }
                                else {
                                    newTarget = idx + 1;
                                }
                            }
                        }
                        else {
                            newTarget = total;
                        }
                    }
                    if (newTarget < 0) 
                        newTarget = 0;
                    if (newTarget > total) 
                        newTarget = total;
                    if (newTarget != state.dragTarget) {
                        state.dragTarget = newTarget;
                        InvalidateRect(hwnd, NULL, TRUE);
                    }
                }
            }
            break;
        }
        case WM_MOUSELEAVE: {
            if (state.hoverIndex != -1 && !state.dragging) {
                state.hoverIndex = -1;
                InvalidateRect(hwnd, NULL, TRUE);
            }
            break;
        }
        case WM_LBUTTONDOWN: {
            SetFocus(hwnd);
            int yPos = GET_Y_LPARAM(lParam);
            int visibleItems = (listType == ID_IMAGE_LIST) ? VISIBLE_ITEMS_IMAGES : VISIBLE_ITEMS_MONITORS;
            int startIdx = (listType == ID_IMAGE_LIST) ? g_imageListScrollPos : g_monitorListScrollPos;
            int itemIndex = startIdx + (yPos - 2) / ITEM_HEIGHT;
            int itemCount = (listType == ID_IMAGE_LIST) ? (int)g_imageFiles.size() : (int)g_monitors.size();
            if (itemIndex >= 0 && itemIndex < itemCount && itemIndex >= startIdx && itemIndex < startIdx + visibleItems) {
                if (listType == ID_IMAGE_LIST) {
                    state.dragSource = itemIndex;
                    state.dragTarget = itemIndex;
                    state.dragPending = true;
                    state.dragging = false;
                    state.dragStart = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
                    SetCapture(hwnd);
                    g_imageListSelected = itemIndex;
                }
                else g_monitorListSelected = itemIndex;
                InvalidateRect(hwnd, NULL, TRUE);
            }
            return 0;
        }
        case WM_LBUTTONUP: {
            if (listType == ID_IMAGE_LIST) {
                if (state.dragging) {
                    if (state.dragSource != -1 && state.dragTarget != -1) {
                        if (state.dragSource != state.dragTarget) {
                            int src = state.dragSource;
                            int dst = state.dragTarget;
                            int oldSize = (int)g_imageFiles.size();
                            ImageFile item = g_imageFiles[src];
                            g_imageFiles.erase(g_imageFiles.begin() + src);
                            if (dst == oldSize)
                                g_imageFiles.push_back(item);
                            else
                                g_imageFiles.insert(g_imageFiles.begin() + dst, item);
                            int actualPos = (dst == oldSize) ? oldSize - 1 : dst;
                            if (g_currentImageIndex == src)
                                g_currentImageIndex = actualPos;
                            else if (g_currentImageIndex > src && g_currentImageIndex <= dst)
                                g_currentImageIndex--;
                            else if (g_currentImageIndex < src && g_currentImageIndex >= dst)
                                g_currentImageIndex++;
                            g_imageListSelected = actualPos;
                            g_settingsChanged = true;
                        }
                        else
                            g_imageListSelected = state.dragSource;
                    }
                    state.dragging = false;
                    state.dragPending = false;
                    state.dragSource = -1;
                    state.dragTarget = -1;
                    ReleaseCapture();
                    InvalidateRect(hwnd, NULL, TRUE);
                }
                else if (state.dragPending) {
                    state.dragPending = false;
                    state.dragSource = -1;
                    ReleaseCapture();
                }
            }
            return 0;
        }
        case WM_CAPTURECHANGED: {
            if (listType == ID_IMAGE_LIST) {
                state.dragging = false;
                state.dragPending = false;
                state.dragSource = -1;
                state.dragTarget = -1;
                InvalidateRect(hwnd, NULL, TRUE);
            }
            break;
        }
        case WM_LBUTTONDBLCLK: {
            int yPos = GET_Y_LPARAM(lParam);
            int visibleItems = (listType == ID_IMAGE_LIST) ? VISIBLE_ITEMS_IMAGES : VISIBLE_ITEMS_MONITORS;
            int startIdx = (listType == ID_IMAGE_LIST) ? g_imageListScrollPos : g_monitorListScrollPos;
            int itemIndex = startIdx + (yPos - 2) / ITEM_HEIGHT;
            int itemCount = (listType == ID_IMAGE_LIST) ? (int)g_imageFiles.size() : (int)g_monitors.size();
            if (itemIndex >= 0 && itemIndex < itemCount && itemIndex >= startIdx && itemIndex < startIdx + visibleItems) {
                if (listType == ID_IMAGE_LIST) {
                    g_imageListSelected = itemIndex;
                    g_currentImageIndex = itemIndex;
                    SetCurrentImageSettings(g_currentImageIndex);
                    CreateOverlayWindow(g_imageFiles[g_currentImageIndex].path, true);
                }
                else {
                    g_monitorListSelected = itemIndex;
                    g_selectedMonitor = itemIndex;
                    CorrectOffsetValues();
                    UpdateOffsetBoundsDisplay();
                    if (g_currentOverlay) {
                        UpdateOverlayPosition();
                        RenderOverlay();
                        g_currentMonitor = g_selectedMonitor;
                    }
                    if (g_currentImageIndex != -1)
                        SaveCurrentImageSettings();
                    g_settingsChanged = true;
                }
                InvalidateRect(hwnd, NULL, TRUE);
            }
            return 0;
        }
        case WM_RBUTTONDOWN:
            if (listType == ID_IMAGE_LIST) {
                int yPos = GET_Y_LPARAM(lParam);
                int startIdx = g_imageListScrollPos;
                int itemIndex = startIdx + (yPos - 2) / ITEM_HEIGHT;
                if (itemIndex >= 0 && itemIndex < (int)g_imageFiles.size() && itemIndex >= startIdx && itemIndex < startIdx + VISIBLE_ITEMS_IMAGES) {
                    g_imageListSelected = itemIndex;
                    InvalidateRect(hwnd, NULL, TRUE);
                    UpdateWindow(hwnd);
                    POINT pt = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
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
        case WM_MOUSEWHEEL: {
            int delta = GET_WHEEL_DELTA_WPARAM(wParam);
            int& scrollPos = (listType == ID_IMAGE_LIST) ? g_imageListScrollPos : g_monitorListScrollPos;
            int maxItems = (listType == ID_IMAGE_LIST) ? (int)g_imageFiles.size() : (int)g_monitors.size();
            int visibleItems = (listType == ID_IMAGE_LIST) ? VISIBLE_ITEMS_IMAGES : VISIBLE_ITEMS_MONITORS;
            if (delta > 0)
                scrollPos = std::max(0, scrollPos - 3);
            else scrollPos = std::min(std::max(0, maxItems - visibleItems), scrollPos + 3);
            InvalidateRect(hwnd, NULL, TRUE);
            return 0;
        }
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);
            DrawOwnerDrawListBox(hdc, hwnd, listType, state.hoverIndex);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_SETFOCUS:
            InvalidateRect(hwnd, NULL, TRUE);
            return 0;
        case WM_SETCURSOR:
            SetCursor(LoadCursor(NULL, IDC_ARROW));
            return TRUE;
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}
void ApplyOffsetFromField(EditField* field, bool isX) {
    int value = 0;
    try {
        if (!field->text.empty()) value = std::stoi(field->text); 
    }
    catch (...) {
        value = 0; 
    }
    if (value > 9999) 
        value = 9999;
    if (value < -9999) 
        value = -9999;
    if (isX) 
        g_offsetX = value;
    else g_offsetY = value;
    CorrectOffsetValues();
    if (g_currentImageIndex != -1) 
        SaveCurrentImageSettings();
}
void ApplyScaleFromField(EditField* field) {
    float value = 1.0f;
    try {
        if (!field->text.empty()) value = std::stof(field->text); 
    }
    catch (...) {
        value = 1.0f; 
    }
    if (value < 0.0f) 
        value = 0.0f;
    if (value > 2.0f) 
        value = 2.0f;
    g_imageScale.store(value, std::memory_order_relaxed);
    CorrectScaleValue();
    if (g_currentImageIndex != -1) 
        SaveCurrentImageSettings();
}
bool ResetHotkeyField(int fieldId, wchar_t defaultKey, wchar_t& currentKey, wchar_t& lastValidKey, EditField& field, bool showConflictMessage) {
    wchar_t oldKey = currentKey;
    std::wstring conflictAction;
    if (CheckHotkeyConflict(defaultKey, fieldId, conflictAction)) {
        if (showConflictMessage) {
            std::wstring message = L"Failed to reset hotkey. Key '" + std::wstring(1, defaultKey) + L"' is already used for: " + conflictAction;
            ShowOwnerDrawMessageBox(g_hwnd, message);
        }
        field.text = lastValidKey ? std::wstring(1, lastValidKey) : L"";
        currentKey = lastValidKey;
    }
    else {
        currentKey = defaultKey;
        lastValidKey = defaultKey;
        field.text = std::wstring(1, defaultKey);
    }
    field.cursorPos = (int)field.text.length();
    field.selStart = field.cursorPos;
    return oldKey != currentKey;
}
void ApplyHotkeyFromField(EditField* field, int id) {
    wchar_t oldToggle = g_toggleHotkeyKey, oldSettings = g_settingsHotkeyKey, oldCycle = g_cycleHotkeyKey, oldQuit = g_quitHotkeyKey;
    if (field->text.empty()) {
        if (id == 1) 
            g_toggleHotkeyKey = 0;
        else if (id == 2) 
            g_settingsHotkeyKey = 0;
        else if (id == 3) 
            g_cycleHotkeyKey = 0;
        else if (id == 4) 
            g_quitHotkeyKey = 0;
    }
    else {
        wchar_t key = std::towupper(field->text[0]);
        if (IsValidHotkeyChar(key)) {
            std::wstring conflictAction;
            if (CheckHotkeyConflict(key, id, conflictAction)) {
                std::wstring message = L"Failed to change hotkey. Key '" + std::wstring(1, key) + L"' is already used for: " + conflictAction;
                ShowOwnerDrawMessageBox(g_hwnd, message);
                if (id == 1) {
                    field->text = g_lastValidToggleKey ? std::wstring(1, g_lastValidToggleKey) : L""; 
                    g_toggleHotkeyKey = g_lastValidToggleKey; 
                }
                else if (id == 2) {
                    field->text = g_lastValidSettingsKey ? std::wstring(1, g_lastValidSettingsKey) : L""; 
                    g_settingsHotkeyKey = g_lastValidSettingsKey; 
                }
                else if (id == 3) {
                    field->text = g_lastValidCycleKey ? std::wstring(1, g_lastValidCycleKey) : L""; 
                    g_cycleHotkeyKey = g_lastValidCycleKey; 
                }
                else if (id == 4) {
                    field->text = g_lastValidQuitKey ? std::wstring(1, g_lastValidQuitKey) : L"";
                    g_quitHotkeyKey = g_lastValidQuitKey; 
                }
            }
            else {
                if (id == 1) {
                    g_toggleHotkeyKey = key; 
                    g_lastValidToggleKey = key; 
                }
                else if (id == 2) {
                    g_settingsHotkeyKey = key; 
                    g_lastValidSettingsKey = key; 
                }
                else if (id == 3) {
                    g_cycleHotkeyKey = key; 
                    g_lastValidCycleKey = key; 
                }
                else if (id == 4) {
                    g_quitHotkeyKey = key; 
                    g_lastValidQuitKey = key; 
                }
            }
        }
        else {
            if (id == 1) {
                field->text = g_lastValidToggleKey ? std::wstring(1, g_lastValidToggleKey) : L""; 
                g_toggleHotkeyKey = g_lastValidToggleKey; 
            }
            else if (id == 2) {
                field->text = g_lastValidSettingsKey ? std::wstring(1, g_lastValidSettingsKey) : L"";
                g_settingsHotkeyKey = g_lastValidSettingsKey; 
                }
            else if (id == 3) {
                field->text = g_lastValidCycleKey ? std::wstring(1, g_lastValidCycleKey) : L""; 
                g_cycleHotkeyKey = g_lastValidCycleKey; 
            }
            else if (id == 4) {
                field->text = g_lastValidQuitKey ? std::wstring(1, g_lastValidQuitKey) : L"";
                g_quitHotkeyKey = g_lastValidQuitKey; 
            }
        }
    }
    field->cursorPos = (int)field->text.length();
    field->selStart = field->cursorPos;
}
void UpdateEditFieldText(EditField* field, const std::wstring& newText) {
    field->text = newText;
    field->cursorPos = (int)field->text.length();
    field->selStart = field->cursorPos;
}
LRESULT HandleOwnerDrawEdit(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam, EditField* field) {
    switch (msg) {
        case WM_MOUSEMOVE: {
            TRACKMOUSEEVENT tme = {sizeof(TRACKMOUSEEVENT), TME_LEAVE, hwnd, 0};
            TrackMouseEvent(&tme);
            if (!field->hover) {
                field->hover = true;
                InvalidateRect(hwnd, NULL, TRUE);
            }
            if (field->selecting && (wParam & MK_LBUTTON)) {
                POINT pt = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
                int idx = GetCharIndexFromEdit(hwnd, field->text, pt);
                if (idx != field->cursorPos) {
                    field->cursorPos = idx;
                    InvalidateRect(hwnd, NULL, TRUE);
                }
            }
            break;
        }
        case WM_MOUSELEAVE: {
            if (field->hover) {
                field->hover = false;
                InvalidateRect(hwnd, NULL, TRUE);
            }
            break;
        }
        case WM_LBUTTONDOWN: {
            SetFocus(hwnd);
            POINT pt = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            int idx = GetCharIndexFromEdit(hwnd, field->text, pt);
            field->cursorPos = idx;
            field->selStart = idx;
            field->selecting = true;
            SetCapture(hwnd);
            InvalidateRect(hwnd, NULL, TRUE);
            return 0;
        }
        case WM_LBUTTONDBLCLK: {
            SetFocus(hwnd);
            field->selStart = 0;
            field->cursorPos = (int)field->text.length();
            field->selecting = false;
            InvalidateRect(hwnd, NULL, TRUE);
            return 0;
        }
        case WM_LBUTTONUP: {
            if (field->selecting) {
                field->selecting = false;
                ReleaseCapture();
                InvalidateRect(hwnd, NULL, TRUE);
            }
            return 0;
        }
        case WM_CAPTURECHANGED: {
            field->selecting = false;
            return 0;
        }
        case WM_SETFOCUS:
            field->hasFocus = true;
            InvalidateRect(hwnd, NULL, TRUE);
            return 0;
        case WM_KILLFOCUS:
            field->hasFocus = false;
            field->selecting = false;
            InvalidateRect(hwnd, NULL, TRUE);
            switch (field->id) {
                case ID_OFFSET_X_DISPLAY: 
                    ApplyOffsetFromField(field, true); 
                    g_settingsChanged = true; 
                    break;
                case ID_OFFSET_Y_DISPLAY: 
                    ApplyOffsetFromField(field, false); 
                    g_settingsChanged = true; 
                    break;
                case ID_SIZE_DISPLAY: 
                    ApplyScaleFromField(field); 
                    g_settingsChanged = true; 
                    break;
                case ID_HOTKEY_TOGGLE_DISPLAY: {
                    wchar_t old = g_toggleHotkeyKey; 
                    ApplyHotkeyFromField(field, 1); 
                    if (old != g_toggleHotkeyKey) {
                        RegisterHotkeys(); 
                        g_settingsChanged = true; 
                    }
                    break; 
                }
                case ID_HOTKEY_SETTINGS_DISPLAY: {
                    wchar_t old = g_settingsHotkeyKey;
                    ApplyHotkeyFromField(field, 2); 
                    if (old != g_settingsHotkeyKey) {
                        RegisterHotkeys(); 
                        g_settingsChanged = true; 
                    }
                    break; 
                }
                case ID_HOTKEY_CYCLE_DISPLAY: {
                    wchar_t old = g_cycleHotkeyKey; 
                    ApplyHotkeyFromField(field, 3); 
                    if (old != g_cycleHotkeyKey) {
                        RegisterHotkeys(); 
                        g_settingsChanged = true; 
                    }
                    break; 
                }
                case ID_HOTKEY_QUIT_DISPLAY: {
                    wchar_t old = g_quitHotkeyKey; 
                    ApplyHotkeyFromField(field, 4); 
                    if (old != g_quitHotkeyKey) {
                        RegisterHotkeys(); 
                        g_settingsChanged = true; 
                    }
                    break; }
            }
            return 0;
        case WM_KEYDOWN:
            switch (wParam) {
                case VK_LEFT: if (field->selStart != field->cursorPos) {
                    field->cursorPos = std::min(field->selStart, field->cursorPos); 
                    field->selStart = field->cursorPos; 
                    InvalidateRect(hwnd, NULL, TRUE); 
                }
                else if (field->cursorPos > 0) {
                    field->cursorPos--; 
                    field->selStart = field->cursorPos; 
                    InvalidateRect(hwnd, NULL, TRUE); 
                }
                return 0;
                case VK_RIGHT: if (field->selStart != field->cursorPos) {
                    field->cursorPos = std::max(field->selStart, field->cursorPos); 
                    field->selStart = field->cursorPos; 
                    InvalidateRect(hwnd, NULL, TRUE); 
                }
                else if (field->cursorPos < (int)field->text.length()) {
                    field->cursorPos++; 
                    field->selStart = field->cursorPos; 
                    InvalidateRect(hwnd, NULL, TRUE); 
                }
                return 0;
                case VK_BACK: 
                    if (field->selStart != field->cursorPos) {
                        int start = std::min(field->selStart, field->cursorPos); 
                        int end = std::max(field->selStart, field->cursorPos); 
                        field->text.erase(start, end - start); 
                        field->cursorPos = start; 
                        field->selStart = start; 
                        InvalidateRect(hwnd, NULL, TRUE); 
                    }
                    else if (field->cursorPos > 0) {
                        field->text.erase(field->cursorPos-1, 1); 
                        field->cursorPos--; 
                        field->selStart = field->cursorPos; 
                        InvalidateRect(hwnd, NULL, TRUE); 
                    }
                    return 0;
                case VK_DELETE: 
                    if (field->selStart != field->cursorPos) {
                        int start = std::min(field->selStart, field->cursorPos); 
                        int end = std::max(field->selStart, field->cursorPos); 
                        field->text.erase(start, end - start); 
                        field->cursorPos = start; 
                        field->selStart = start; 
                        InvalidateRect(hwnd, NULL, TRUE); 
                    }
                    else if (field->cursorPos < (int)field->text.length()) {
                        field->text.erase(field->cursorPos, 1); 
                        field->selStart = field->cursorPos; 
                        InvalidateRect(hwnd, NULL, TRUE); 
                    }
                    return 0;
                case VK_ESCAPE: 
                case VK_RETURN: 
                    SetFocus(GetParent(hwnd)); 
                    return 0;
            }
            break;
        case WM_CHAR: {
            wchar_t ch = (wchar_t)wParam;
            if (ch == VK_RETURN || ch == VK_ESCAPE) 
                return 0;
            if (field->id == ID_HOTKEY_TOGGLE_DISPLAY || field->id == ID_HOTKEY_SETTINGS_DISPLAY || field->id == ID_HOTKEY_CYCLE_DISPLAY || field->id == ID_HOTKEY_QUIT_DISPLAY) {
                if (IsValidHotkeyChar(ch)) {
                    field->text = std::wstring(1, std::towupper(ch)); 
                    field->cursorPos = 1; 
                    field->selStart = 1; 
                    InvalidateRect(hwnd, NULL, TRUE); 
                }
                else if (ch == VK_BACK && !field->text.empty()) {
                    field->text.clear(); 
                    field->cursorPos = 0; 
                    field->selStart = 0; 
                    InvalidateRect(hwnd, NULL, TRUE); 
                }
                return 0;
            }
            else {
                bool allowed = false;
                if (field->id == ID_OFFSET_X_DISPLAY || field->id == ID_OFFSET_Y_DISPLAY)
                    allowed = ((ch >= L'0' && ch <= L'9') || ch == L'-');
                else if (field->id == ID_SIZE_DISPLAY) 
                    allowed = ((ch >= L'0' && ch <= L'9') || ch == L'.');
                if (!allowed) 
                    return 0;
                if (field->selStart != field->cursorPos) {
                    int start = std::min(field->selStart, field->cursorPos);
                    int end = std::max(field->selStart, field->cursorPos);
                    field->text.erase(start, end - start);
                    field->cursorPos = start;
                    field->selStart = start;
                }
                if (field->id == ID_OFFSET_X_DISPLAY || field->id == ID_OFFSET_Y_DISPLAY) {
                    if (ch == L'-' && (field->cursorPos != 0 || field->text.find(L'-') != std::wstring::npos)) 
                        return 0;
                    if (field->text.length() < 5) {
                        field->text.insert(field->cursorPos, 1, ch); 
                        field->cursorPos++; 
                        field->selStart = field->cursorPos; 
                        InvalidateRect(hwnd, NULL, TRUE); 
                    }
                }
                else if (field->id == ID_SIZE_DISPLAY) {
                    if (ch == L'.' && field->text.find(L'.') != std::wstring::npos) 
                        return 0;
                    if (field->text.length() < 4) {
                        field->text.insert(field->cursorPos, 1, ch); 
                        field->cursorPos++; 
                        field->selStart = field->cursorPos; 
                        InvalidateRect(hwnd, NULL, TRUE); 
                    }
                }
                return 0;
            }
        }
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);
            DrawOwnerDrawEdit(hdc, hwnd, field, g_darkTheme, field->hover);
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
LRESULT CALLBACK ImageListSubclassProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT rc;
        GetClientRect(hwnd, &rc);
        HDC hdcMem = CreateCompatibleDC(hdc);
        HBITMAP hBitmap = CreateCompatibleBitmap(hdc, rc.right - rc.left, rc.bottom - rc.top);
        HBITMAP hOldBitmap = (HBITMAP)SelectObject(hdcMem, hBitmap);
        DrawOwnerDrawListBox(hdcMem, hwnd, ID_IMAGE_LIST, -1);
        BitBlt(hdc, rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top, hdcMem, 0, 0, SRCCOPY);
        SelectObject(hdcMem, hOldBitmap);
        DeleteObject(hBitmap);
        DeleteDC(hdcMem);
        EndPaint(hwnd, &ps);
        return 0;
    }
    return HandleOwnerDrawListBox(hwnd, msg, wParam, lParam, ID_IMAGE_LIST);
}
LRESULT CALLBACK MonitorListSubclassProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT rc;
        GetClientRect(hwnd, &rc);
        HDC hdcMem = CreateCompatibleDC(hdc);
        HBITMAP hBitmap = CreateCompatibleBitmap(hdc, rc.right - rc.left, rc.bottom - rc.top);
        HBITMAP hOldBitmap = (HBITMAP)SelectObject(hdcMem, hBitmap);
        DrawOwnerDrawListBox(hdcMem, hwnd, ID_MONITOR_LIST, -1);
        BitBlt(hdc, rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top, hdcMem, 0, 0, SRCCOPY);
        SelectObject(hdcMem, hOldBitmap);
        DeleteObject(hBitmap);
        DeleteDC(hdcMem);
        EndPaint(hwnd, &ps);
        return 0;
    }
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
LRESULT CALLBACK HotkeyCycleSubclassProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    return HandleOwnerDrawEdit(hwnd, msg, wParam, lParam, &g_hotkeyCycleField);
}
LRESULT CALLBACK HotkeyQuitSubclassProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    return HandleOwnerDrawEdit(hwnd, msg, wParam, lParam, &g_hotkeyQuitField);
}
LRESULT CALLBACK TabButton1SubclassProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    return HandleOwnerDrawButton(hwnd, msg, wParam, lParam, ID_TAB_BUTTON1);
}
LRESULT CALLBACK TabButton2SubclassProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    return HandleOwnerDrawButton(hwnd, msg, wParam, lParam, ID_TAB_BUTTON2);
}
LRESULT CALLBACK TabButton3SubclassProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    return HandleOwnerDrawButton(hwnd, msg, wParam, lParam, ID_TAB_BUTTON3);
}
LRESULT CALLBACK BrowseButtonSubclassProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    return HandleOwnerDrawButton(hwnd, msg, wParam, lParam, ID_BROWSE_BUTTON);
}
LRESULT CALLBACK ThemeButtonSubclassProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    return HandleOwnerDrawButton(hwnd, msg, wParam, lParam, ID_THEME_BUTTON);
}
LRESULT CALLBACK ResetToggleSubclassProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    return HandleOwnerDrawButton(hwnd, msg, wParam, lParam, ID_RESET_TOGGLE);
}
LRESULT CALLBACK ResetSettingsSubclassProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    return HandleOwnerDrawButton(hwnd, msg, wParam, lParam, ID_RESET_SETTINGS);
}
LRESULT CALLBACK ResetCycleSubclassProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    return HandleOwnerDrawButton(hwnd, msg, wParam, lParam, ID_RESET_CYCLE);
}
LRESULT CALLBACK ResetQuitSubclassProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    return HandleOwnerDrawButton(hwnd, msg, wParam, lParam, ID_RESET_QUIT);
}
LRESULT CALLBACK HideOverlayCheckboxSubclassProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    return HandleOwnerDrawCheckbox(hwnd, msg, wParam, lParam);
}
LRESULT CALLBACK ReverseDirectionCheckboxSubclassProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    return HandleOwnerDrawCheckbox(hwnd, msg, wParam, lParam);
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
    for (const auto& img : g_imageFiles)
        if (img.path == filePath) 
            return;
    ImageFile newImage;
    newImage.path = filePath;
    size_t lastSlash = filePath.find_last_of(L"\\/");
    newImage.filename = (lastSlash != std::wstring::npos) ? filePath.substr(lastSlash + 1) : filePath;
    newImage.settings = ImageSettings();
    g_imageFiles.push_back(newImage);
    if (g_hImageList) 
        InvalidateRect(g_hImageList, NULL, TRUE);
    if (saveToIni) 
        g_settingsChanged = true;
}
void LoadAllSettingsFromIni() {
    std::wstring iniPath = GetIniFilePath();
    DWORD attrs = GetFileAttributesW(iniPath.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES) {
        int sw = GetSystemMetrics(SM_CXSCREEN), sh = GetSystemMetrics(SM_CYSCREEN);
        g_windowX = (sw - 300) / 2;
        g_windowY = (sh - 300) / 2;
        g_toggleHotkeyKey = DEFAULT_TOGGLE_KEY;
        g_settingsHotkeyKey = DEFAULT_SETTINGS_KEY;
        g_cycleHotkeyKey = DEFAULT_CYCLE_KEY;
        g_quitHotkeyKey = DEFAULT_QUIT_KEY;
        g_darkTheme = false;
        g_reverseDirection = false;
        g_lastValidToggleKey = DEFAULT_TOGGLE_KEY;
        g_lastValidSettingsKey = DEFAULT_SETTINGS_KEY;
        g_lastValidCycleKey = DEFAULT_CYCLE_KEY;
        g_lastValidQuitKey = DEFAULT_QUIT_KEY;
        g_imageFiles.clear();
        g_currentImageIndex = -1;
        g_settingsChanged = false;
        return;
    }
    wchar_t themeStr[10];
    GetPrivateProfileStringW(L"Window", L"Theme", L"Light", themeStr, 10, iniPath.c_str());
    g_darkTheme = (wcscmp(themeStr, L"Dark") == 0);
    wchar_t countStr[10];
    GetPrivateProfileStringW(L"Images List", L"Count", L"0", countStr, 10, iniPath.c_str());
    int count = _wtoi(countStr);
    g_imageFiles.clear();
    for (int i = 0; i < count; i++) {
        std::wstring key = L"Image" + std::to_wstring(i);
        wchar_t path[MAX_PATH];
        GetPrivateProfileStringW(L"Images List", key.c_str(), L"", path, MAX_PATH, iniPath.c_str());
        if (wcslen(path) > 0 && GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES) {
            ImageFile img;
            img.path = path;
            size_t lastSlash = img.path.find_last_of(L"\\/");
            img.filename = (lastSlash != std::wstring::npos) ? img.path.substr(lastSlash + 1) : img.path;
            img.settings = ImageSettings();
            std::wstring section = L"Location & Size " + std::to_wstring(i);
            wchar_t monitorStr[10], offsetXStr[32], offsetYStr[32], scaleStr[32];
            GetPrivateProfileStringW(section.c_str(), L"SelectedMonitor", L"0", monitorStr, 10, iniPath.c_str());
            GetPrivateProfileStringW(section.c_str(), L"OffsetX", L"0", offsetXStr, 32, iniPath.c_str());
            GetPrivateProfileStringW(section.c_str(), L"OffsetY", L"0", offsetYStr, 32, iniPath.c_str());
            GetPrivateProfileStringW(section.c_str(), L"ImageScale", L"1.0", scaleStr, 32, iniPath.c_str());
            img.settings.monitor = _wtoi(monitorStr);
            img.settings.offsetX = _wtoi(offsetXStr);
            img.settings.offsetY = _wtoi(offsetYStr);
            float loadedScale = (float)_wtof(scaleStr);
            if (loadedScale < 0.0f) 
                loadedScale = 0.0f;
            if (loadedScale > 2.0f) 
                loadedScale = 2.0f;
            img.settings.scale = loadedScale;
            g_imageFiles.push_back(img);
        }
    }
    wchar_t lastDisplayed[MAX_PATH];
    GetPrivateProfileStringW(L"Images List", L"LastDisplayed", L"", lastDisplayed, MAX_PATH, iniPath.c_str());
    g_lastDisplayedImage = lastDisplayed;
    wchar_t toggleKeyStr[10];
    GetPrivateProfileStringW(L"Hotkeys", L"ToggleOverlayVisibility", std::wstring(1, DEFAULT_TOGGLE_KEY).c_str(), toggleKeyStr, 10, iniPath.c_str());
    if (wcslen(toggleKeyStr) > 0) {
        wchar_t key = std::towupper(toggleKeyStr[0]);
        if (IsValidHotkeyChar(key)) {
            g_toggleHotkeyKey = key; 
            g_lastValidToggleKey = key; 
            UpdateEditFieldText(&g_hotkeyToggleField, std::wstring(1, key)); 
        }
        else {
            g_toggleHotkeyKey = DEFAULT_TOGGLE_KEY; 
            g_lastValidToggleKey = DEFAULT_TOGGLE_KEY; 
            UpdateEditFieldText(&g_hotkeyToggleField, std::wstring(1, DEFAULT_TOGGLE_KEY)); 
            WritePrivateProfileStringW(L"Hotkeys", L"ToggleOverlayVisibility", std::wstring(1, DEFAULT_TOGGLE_KEY).c_str(), iniPath.c_str()); 
        }
    }
    wchar_t settingsKeyStr[10];
    GetPrivateProfileStringW(L"Hotkeys", L"ToggleSettingsWindowVisibility", std::wstring(1, DEFAULT_SETTINGS_KEY).c_str(), settingsKeyStr, 10, iniPath.c_str());
    if (wcslen(settingsKeyStr) > 0) {
        wchar_t key = std::towupper(settingsKeyStr[0]);
        if (IsValidHotkeyChar(key)) {
            g_settingsHotkeyKey = key; 
            g_lastValidSettingsKey = key; 
            UpdateEditFieldText(&g_hotkeySettingsField, std::wstring(1, key)); 
        }
        else {
            g_settingsHotkeyKey = DEFAULT_SETTINGS_KEY; 
            g_lastValidSettingsKey = DEFAULT_SETTINGS_KEY; 
            UpdateEditFieldText(&g_hotkeySettingsField, std::wstring(1, DEFAULT_SETTINGS_KEY)); 
            WritePrivateProfileStringW(L"Hotkeys", L"ToggleSettingsWindowVisibility", std::wstring(1, DEFAULT_SETTINGS_KEY).c_str(), iniPath.c_str()); 
        }
    }
    wchar_t cycleKeyStr[10];
    GetPrivateProfileStringW(L"Hotkeys", L"SwitchToNextImageFromList", std::wstring(1, DEFAULT_CYCLE_KEY).c_str(), cycleKeyStr, 10, iniPath.c_str());
    if (wcslen(cycleKeyStr) > 0) {
        wchar_t key = std::towupper(cycleKeyStr[0]);
        if (IsValidHotkeyChar(key)) {
            g_cycleHotkeyKey = key; 
            g_lastValidCycleKey = key; 
            UpdateEditFieldText(&g_hotkeyCycleField, std::wstring(1, key)); 
        }
        else {
            g_cycleHotkeyKey = DEFAULT_CYCLE_KEY; 
            g_lastValidCycleKey = DEFAULT_CYCLE_KEY; 
            UpdateEditFieldText(&g_hotkeyCycleField, std::wstring(1, DEFAULT_CYCLE_KEY)); 
            WritePrivateProfileStringW(L"Hotkeys", L"SwitchToNextImageFromList", std::wstring(1, DEFAULT_CYCLE_KEY).c_str(), iniPath.c_str()); 
        }
    }
    wchar_t reverseStr[10];
    GetPrivateProfileStringW(L"Hotkeys", L"ReverseDirection", L"0", reverseStr, 10, iniPath.c_str());
    g_reverseDirection = (_wtoi(reverseStr) != 0);
    wchar_t quitKeyStr[10];
    GetPrivateProfileStringW(L"Hotkeys", L"Quit", std::wstring(1, DEFAULT_QUIT_KEY).c_str(), quitKeyStr, 10, iniPath.c_str());
    if (wcslen(quitKeyStr) > 0) {
        wchar_t key = std::towupper(quitKeyStr[0]);
        if (IsValidHotkeyChar(key)) {
            g_quitHotkeyKey = key; 
            g_lastValidQuitKey = key; 
            UpdateEditFieldText(&g_hotkeyQuitField, std::wstring(1, key)); 
        }
        else {
            g_quitHotkeyKey = DEFAULT_QUIT_KEY; 
            g_lastValidQuitKey = DEFAULT_QUIT_KEY; 
            UpdateEditFieldText(&g_hotkeyQuitField, std::wstring(1, DEFAULT_QUIT_KEY)); 
            WritePrivateProfileStringW(L"Hotkeys", L"Quit", std::wstring(1, DEFAULT_QUIT_KEY).c_str(), iniPath.c_str()); 
        }
    }
    wchar_t windowXStr[32], windowYStr[32];
    GetPrivateProfileStringW(L"Window", L"WindowX", L"-32768", windowXStr, 32, iniPath.c_str());
    GetPrivateProfileStringW(L"Window", L"WindowY", L"-32768", windowYStr, 32, iniPath.c_str());
    g_windowX = _wtoi(windowXStr);
    g_windowY = _wtoi(windowYStr);
    if (g_windowX == -32768 || g_windowY == -32768) {
        int sw = GetSystemMetrics(SM_CXSCREEN), sh = GetSystemMetrics(SM_CYSCREEN);
        g_windowX = (sw - 300) / 2;
        g_windowY = (sh - 300) / 2;
    }
    g_currentImageIndex = -1;
    for (size_t i = 0; i < g_imageFiles.size(); ++i) {
        if (g_imageFiles[i].path == g_lastDisplayedImage) {
            g_currentImageIndex = (int)i;
            break;
        }
    }
    if (g_currentImageIndex != -1) 
        SetCurrentImageSettings(g_currentImageIndex);
    g_settingsChanged = false;
}
void SaveAllSettingsToIni() {
    if (!g_hwnd) 
        return;
    std::wstring iniPath = GetIniFilePath();
    std::wstring content;
    const unsigned char bom[] = {0xFF, 0xFE};
    content += L"[Window]\r\n";
    content += L"Theme=" + std::wstring(g_darkTheme ? L"Dark" : L"Light") + L"\r\n";
    if (g_windowVisible) {
        RECT wr;
        GetWindowRect(g_hwnd, &wr);
        content += L"WindowX=" + std::to_wstring(wr.left) + L"\r\n";
        content += L"WindowY=" + std::to_wstring(wr.top) + L"\r\n";
    }
    else {
        content += L"WindowX=" + std::to_wstring(g_windowX) + L"\r\n";
        content += L"WindowY=" + std::to_wstring(g_windowY) + L"\r\n";
    }
    content += L"\r\n";
    content += L"[Hotkeys]\r\n";
    content += L"ToggleOverlayVisibility=" + (g_toggleHotkeyKey ? std::wstring(1, g_toggleHotkeyKey) : L"") + L"\r\n";
    content += L"ToggleSettingsWindowVisibility=" + (g_settingsHotkeyKey ? std::wstring(1, g_settingsHotkeyKey) : L"") + L"\r\n";
    content += L"SwitchToNextImageFromList=" + (g_cycleHotkeyKey ? std::wstring(1, g_cycleHotkeyKey) : L"") + L"\r\n";
    content += L"ReverseDirection=" + std::to_wstring(g_reverseDirection ? 1 : 0) + L"\r\n";
    content += L"Quit=" + (g_quitHotkeyKey ? std::wstring(1, g_quitHotkeyKey) : L"") + L"\r\n";
    content += L"\r\n";
    content += L"[Images List]\r\n";
    content += L"Count=" + std::to_wstring(g_imageFiles.size()) + L"\r\n";
    content += L"LastDisplayed=" + g_lastDisplayedImage + L"\r\n";
    for (size_t i = 0; i < g_imageFiles.size(); ++i) {
        content += L"Image" + std::to_wstring(i) + L"=" + g_imageFiles[i].path + L"\r\n";
    }
    content += L"\r\n";
    for (size_t i = 0; i < g_imageFiles.size(); ++i) {
        content += L"[Location & Size " + std::to_wstring(i) + L"]\r\n";
        content += L"SelectedMonitor=" + std::to_wstring(g_imageFiles[i].settings.monitor) + L"\r\n";
        content += L"OffsetX=" + std::to_wstring(g_imageFiles[i].settings.offsetX) + L"\r\n";
        content += L"OffsetY=" + std::to_wstring(g_imageFiles[i].settings.offsetY) + L"\r\n";
        wchar_t scaleStr[32];
        swprintf_s(scaleStr, 32, L"%.2f", g_imageFiles[i].settings.scale);
        content += L"ImageScale=" + std::wstring(scaleStr) + L"\r\n";
        if (i != g_imageFiles.size() - 1) 
            content += L"\r\n";
    }
    HANDLE hFile = CreateFileW(iniPath.c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile != INVALID_HANDLE_VALUE) {
        DWORD written;
        WriteFile(hFile, bom, sizeof(bom), &written, NULL);
        WriteFile(hFile, content.c_str(), content.size() * sizeof(wchar_t), &written, NULL);
        CloseHandle(hFile);
    }
}
void CorrectOffsetValues() {
    std::shared_lock<std::shared_mutex> lock(g_monitorMutex);
    if (g_selectedMonitor >= 0 && g_selectedMonitor < (int)g_monitors.size()) {
        const auto& m = g_monitors[g_selectedMonitor];
        int maxX = (m.rect.right - m.rect.left) / 2;
        int maxY = (m.rect.bottom - m.rect.top) / 2;
        if (g_offsetX > maxX) 
            g_offsetX = maxX;
        if (g_offsetX < -maxX) 
            g_offsetX = -maxX;
        if (g_offsetY > maxY) 
            g_offsetY = maxY;
        if (g_offsetY < -maxY) 
            g_offsetY = -maxY;
        UpdateEditFieldText(&g_offsetXField, std::to_wstring(g_offsetX));
        UpdateEditFieldText(&g_offsetYField, std::to_wstring(g_offsetY));
        if (g_hOffsetXDisplay) 
            InvalidateRect(g_hOffsetXDisplay, NULL, TRUE);
        if (g_hOffsetYDisplay) 
            InvalidateRect(g_hOffsetYDisplay, NULL, TRUE);
    }
    if (g_currentOverlay) 
        UpdateOverlayPosition();
    if (g_currentImageIndex != -1) 
        SaveCurrentImageSettings();
}
void CorrectScaleValue() {
    float scale = g_imageScale.load();
    if (scale < 0.0f) 
        scale = 0.0f;
    if (scale > 2.0f) 
        scale = 2.0f;
    g_imageScale.store(scale, std::memory_order_relaxed);
    std::wstringstream ss; 
    ss << std::fixed << std::setprecision(2) << scale;
    UpdateEditFieldText(&g_sizeField, ss.str());
    if (g_hSizeDisplay) 
        InvalidateRect(g_hSizeDisplay, NULL, TRUE);
    UpdateScaleControls();
    if (g_currentOverlay) 
        RenderOverlay();
    if (g_currentImageIndex != -1) 
        SaveCurrentImageSettings();
}
void SafeGdiplusShutdown() {
    if (g_gdiplusToken) {
        GdiplusShutdown(g_gdiplusToken);
        g_gdiplusToken = 0;
    }
}
void UpdateOverlayPosition() {
    if (!g_currentOverlay) 
        return;
    int windowWidth = 2 * g_originalImageWidth;
    int windowHeight = 2 * g_originalImageHeight;
    int x = 0, y = 0;
    {
        std::shared_lock<std::shared_mutex> lock(g_monitorMutex);
        if (g_selectedMonitor >= 0 && g_selectedMonitor < (int)g_monitors.size()) {
            const auto& m = g_monitors[g_selectedMonitor];
            int sw = m.rect.right - m.rect.left;
            int sh = m.rect.bottom - m.rect.top;
            x = m.rect.left + (sw - windowWidth) / 2 + g_offsetX;
            y = m.rect.top + (sh - windowHeight) / 2 + g_offsetY;
        }
        else {
            int sw = GetSystemMetrics(SM_CXSCREEN);
            int sh = GetSystemMetrics(SM_CYSCREEN);
            x = (sw - windowWidth) / 2 + g_offsetX;
            y = (sh - windowHeight) / 2 + g_offsetY;
        }
    }
    RECT currentRect;
    GetWindowRect(g_currentOverlay, &currentRect);
    if (currentRect.left == x && currentRect.top == y) 
        return;
    SetWindowPos(g_currentOverlay, NULL, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    GetWindowRect(g_currentOverlay, &currentRect);
    g_overlayX = currentRect.left;
    g_overlayY = currentRect.top;
}
void RenderOverlayInThread() {
    HANDLE hEvent = g_renderEvent;
    while (!g_renderThreadShouldExit) {
        WaitForSingleObject(hEvent, INFINITE);
        if (g_renderThreadShouldExit) 
            break;
        HWND hwnd = g_currentOverlayAtomic.load();
        if (!hwnd || !IsWindow(hwnd)) 
            continue;
        int origWidth = 0, origHeight = 0;
        float scale = 1.0f;
        std::shared_ptr<Bitmap> bitmapCopy;
        std::shared_ptr<AnimatedGIF> gifCopy;
        int overlayX, overlayY;
        {
            std::lock_guard<std::mutex> lock(g_renderDataMutex);
            origWidth = g_originalImageWidth;
            origHeight = g_originalImageHeight;
            scale = g_imageScale.load(std::memory_order_relaxed);
            bitmapCopy = g_cachedBitmapShared;
            gifCopy = g_animatedGIFShared;
            overlayX = g_overlayX.load();
            overlayY = g_overlayY.load();
        }
        if (!IsWindowVisible(hwnd)) 
            continue;
        int windowWidth = 2 * origWidth;
        int windowHeight = 2 * origHeight;
        int drawWidth = (int)(origWidth * scale);
        int drawHeight = (int)(origHeight * scale);
        int drawX = (windowWidth - drawWidth) / 2;
        int drawY = (windowHeight - drawHeight) / 2;
        HDC hdcScreen = GetDC(NULL);
        if (!hdcScreen) 
            continue;
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
        graphics.SetInterpolationMode(InterpolationModeHighQualityBicubic);
        SolidBrush clearBrush(Color(0,0,0,0));
        graphics.FillRectangle(&clearBrush, 0, 0, windowWidth, windowHeight);
        bool success = false;
        if (gifCopy && gifCopy->IsInitialized()) {
            int frameIdx = gifCopy->GetCurrentFrameIndex();
            auto scaledFrame = gifCopy->GetScaledFrame(frameIdx, scale, drawWidth, drawHeight);
            if (scaledFrame) {
                Rect destRect(drawX, drawY, drawWidth, drawHeight);
                graphics.DrawImage(scaledFrame.get(), destRect);
                success = true;
            }
        }
        else if (bitmapCopy) {
            Rect destRect(drawX, drawY, drawWidth, drawHeight);
            graphics.DrawImage(bitmapCopy.get(), destRect);
            success = true;
        }
        if (success) {
            BLENDFUNCTION blend = {AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
            POINT ptPos = {overlayX, overlayY};
            SIZE sizeWnd = {windowWidth, windowHeight};
            POINT ptSrc = {0, 0};
            UpdateLayeredWindow(hwnd, hdcScreen, &ptPos, &sizeWnd, hdcMem, &ptSrc, 0, &blend, ULW_ALPHA);
        }
        SelectObject(hdcMem, hOldBitmap);
        DeleteObject(hBitmap);
        DeleteDC(hdcMem);
        ReleaseDC(NULL, hdcScreen);
    }
}
void StartRenderThread() {
    if (!g_renderThreadRunning) {
        g_renderEvent = CreateEvent(NULL, FALSE, FALSE, NULL);
        g_renderThreadShouldExit = false;
        g_renderThreadRunning = true;
        g_renderThread = std::thread(RenderOverlayInThread);
    }
}
void StopRenderThread() {
    if (g_renderThreadRunning) {
        g_renderThreadShouldExit = true;
        SetEvent(g_renderEvent);
        if (g_renderThread.joinable()) 
            g_renderThread.join();
        g_renderThreadRunning = false;
        if (g_renderEvent) {
            CloseHandle(g_renderEvent); 
            g_renderEvent = NULL; 
        }
    }
}
void RenderOverlay() {
    SetEvent(g_renderEvent);
}
void ApplyScaleChange() {
    if (g_animatedGIFShared) 
        g_animatedGIFShared->ClearCache();
    RenderOverlay();
}
LRESULT CALLBACK OverlayProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    switch (uMsg) {
        case WM_TIMER:
            if (wParam == ID_ANIMATION_TIMER) {
                std::shared_ptr<AnimatedGIF> gif;
                {
                std::lock_guard<std::mutex> lock(g_renderDataMutex); 
                gif = g_animatedGIFShared; 
                }
                if (gif && gif->IsInitialized()) {
                    gif->NextFrame(); 
                    RenderOverlay(); 
                }
            }
            return 0;
        case WM_MOVE: 
        case WM_WINDOWPOSCHANGED: {
            RECT rc; 
            GetWindowRect(hwnd, &rc);
            g_overlayX = rc.left; 
            g_overlayY = rc.top;
            return 0;
        }
        case WM_DESTROY: 
            return 0;
    }
    return DefWindowProc(hwnd, uMsg, wParam, lParam);
}
void RemoveImageFromList(int index) {
    if (index < 0 || index >= (int)g_imageFiles.size()) 
        return;
    if (index == g_currentImageIndex) {
        if (g_animatedGIFShared) 
            g_animatedGIFShared->StopAnimation();
        if (g_currentOverlay) 
            DestroyWindow(g_currentOverlay);
        g_currentOverlay = NULL;
        g_currentOverlayAtomic = nullptr;
        {
        std::lock_guard<std::mutex> lock(g_renderDataMutex); 
        g_cachedBitmapShared.reset(); 
        g_animatedGIFShared.reset(); 
        g_originalImageWidth = 0; 
        g_originalImageHeight = 0; 
        }
        StopRenderThread();
        g_currentImageIndex = -1;
        g_lastDisplayedImage.clear();
    }
    else if (index < g_currentImageIndex) {
        g_currentImageIndex--;
    }
    g_imageFiles.erase(g_imageFiles.begin() + index);
    if (g_imageListSelected >= index) {
        if (g_imageListSelected > 0) 
            g_imageListSelected--;
        else g_imageListSelected = -1;
    }
    if (g_hImageList) 
        InvalidateRect(g_hImageList, NULL, TRUE);
    g_settingsChanged = true;
}
void CreateOverlayWindow(const std::wstring& imagePath, bool saveToIni) {
    if (imagePath.empty()) 
        return;
    if (GetFileAttributesW(imagePath.c_str()) == INVALID_FILE_ATTRIBUTES) {
        int index = -1;
        for (size_t i = 0; i < g_imageFiles.size(); ++i) {
            if (g_imageFiles[i].path == imagePath) {
                index = (int)i;
                break;
            }
        }
        if (index != -1) {
            ShowOwnerDrawMessageBox(g_hwnd, L"File not found. The file is moved or deleted");
            RemoveImageFromList(index);
        }
        return;
    }
    bool destroyOldWindow = (g_currentOverlay != NULL && g_lastDisplayedImage != imagePath);
    if (destroyOldWindow) {
        if (g_animatedGIFShared) 
            g_animatedGIFShared->StopAnimation();
        DestroyWindow(g_currentOverlay);
        g_currentOverlay = NULL;
        g_currentOverlayAtomic = nullptr;
        {
        std::lock_guard<std::mutex> lock(g_renderDataMutex); 
        g_cachedBitmapShared.reset(); 
        g_animatedGIFShared.reset(); 
        g_originalImageWidth = 0; 
        g_originalImageHeight = 0; 
        }
        StopRenderThread();
    }
    bool isGif = false;
    size_t dotPos = imagePath.find_last_of(L".");
    if (dotPos != std::wstring::npos) {
        std::wstring ext = imagePath.substr(dotPos + 1);
        for (wchar_t& c : ext) 
            c = towlower(c);
        isGif = (ext == L"gif");
    }
    if (isGif) {
        auto gif = std::make_shared<AnimatedGIF>();
        if (!gif->Load(imagePath)) 
            return;
        {
        std::lock_guard<std::mutex> lock(g_renderDataMutex); 
        g_animatedGIFShared = gif; 
        g_cachedBitmapShared.reset(); 
        g_originalImageWidth = gif->GetWidth(); 
        g_originalImageHeight = gif->GetHeight(); 
        g_lastDisplayedImage = imagePath; 
        }
    }
    else {
        auto bitmap = std::make_shared<Bitmap>(imagePath.c_str());
        if (!bitmap || bitmap->GetLastStatus() != Ok) 
            return;
        {
        std::lock_guard<std::mutex> lock(g_renderDataMutex); 
        g_cachedBitmapShared = bitmap; 
        g_animatedGIFShared.reset(); 
        g_originalImageWidth = bitmap->GetWidth(); 
        g_originalImageHeight = bitmap->GetHeight(); 
        g_lastDisplayedImage = imagePath; 
        }
    }
    int windowWidth = 2 * g_originalImageWidth;
    int windowHeight = 2 * g_originalImageHeight;
    bool needNewWindow = (g_currentOverlay == NULL);
    if (!needNewWindow) {
        if (g_selectedMonitor != g_currentMonitor) {
            UpdateOverlayPosition(); 
            g_currentMonitor = g_selectedMonitor; 
        }
        else {
            RECT cr; 
            GetWindowRect(g_currentOverlay, &cr);
            int cw = cr.right - cr.left, ch = cr.bottom - cr.top;
            if (cw != windowWidth || ch != windowHeight) {
                SetWindowPos(g_currentOverlay, NULL, 0, 0, windowWidth, windowHeight, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE); 
                UpdateOverlayPosition(); 
            }
        }
    }
    if (needNewWindow) {
        int x = 0, y = 0;
        {
            std::shared_lock<std::shared_mutex> lock(g_monitorMutex);
            if (g_selectedMonitor >= 0 && g_selectedMonitor < (int)g_monitors.size()) {
                const auto& m = g_monitors[g_selectedMonitor];
                int sw = m.rect.right - m.rect.left, sh = m.rect.bottom - m.rect.top;
                x = m.rect.left + (sw - windowWidth) / 2 + g_offsetX;
                y = m.rect.top + (sh - windowHeight) / 2 + g_offsetY;
                g_currentMonitor = g_selectedMonitor;
            }
            else {
                int sw = GetSystemMetrics(SM_CXSCREEN), sh = GetSystemMetrics(SM_CYSCREEN);
                x = (sw - windowWidth) / 2 + g_offsetX;
                y = (sh - windowHeight) / 2 + g_offsetY;
                g_currentMonitor = 0;
            }
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
        if (!g_currentOverlay) 
            return;
        g_currentOverlayAtomic = g_currentOverlay;
        g_overlayX = x; 
        g_overlayY = y;
        if (!g_hideOverlay) {
            ShowWindow(g_currentOverlay, SW_SHOWNA); 
            SetWindowPos(g_currentOverlay, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE); 
        }
    }
    StartRenderThread();
    std::shared_ptr<AnimatedGIF> gif;
    {
    std::lock_guard<std::mutex> lock(g_renderDataMutex); 
    gif = g_animatedGIFShared; 
    }
    if (gif && gif->IsInitialized() && gif->IsAnimated())
        gif->StartAnimation(g_currentOverlay);
    RenderOverlay();
    if (saveToIni) 
        g_settingsChanged = true;
}
void RestoreLastOverlay() {
    if (g_currentImageIndex != -1) 
        CreateOverlayWindow(g_imageFiles[g_currentImageIndex].path, false);
}
void BrowseForImage() {
    OPENFILENAMEW ofn = {sizeof(ofn)};
    wchar_t szFile[MAX_PATH] = {0};
    ofn.hwndOwner = g_hwnd;
    ofn.lpstrFile = szFile;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrFilter = L"Images\0*.png;*.jpg;*.jpeg;*.gif\0All Files\0*.*\0";
    ofn.nFilterIndex = 1;
    ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST;
    if (GetOpenFileNameW(&ofn)) 
        AddImageToList(szFile, true);
}
void OpenFileLocation(const std::wstring& filePath) {
    std::wstring params = L"/select,\"" + filePath + L"\"";
    ShellExecute(NULL, L"open", L"explorer.exe", params.c_str(), NULL, SW_SHOW);
}
void UpdateHideOverlayCheckbox() {
    if (g_hHideOverlayCheckbox) 
        InvalidateRect(g_hHideOverlayCheckbox, NULL, TRUE);
}
void UpdateMonitorSelection() {
    g_monitorListSelected = g_selectedMonitor;
    if (g_hMonitorList) 
        InvalidateRect(g_hMonitorList, NULL, TRUE);
}
void UpdateOffsetEditControls() {
    UpdateEditFieldText(&g_offsetXField, std::to_wstring(g_offsetX));
    UpdateEditFieldText(&g_offsetYField, std::to_wstring(g_offsetY));
    if (g_hOffsetXDisplay) 
        InvalidateRect(g_hOffsetXDisplay, NULL, TRUE);
    if (g_hOffsetYDisplay) 
        InvalidateRect(g_hOffsetYDisplay, NULL, TRUE);
}
void UpdateScaleControls() {
    int sliderValue = (int)(g_imageScale.load() * 100.0f + 0.5f);
    if (sliderValue < 0) 
        sliderValue = 0;
    if (sliderValue > 200) 
        sliderValue = 200;
    if (g_hSizeSlider) 
        InvalidateRect(g_hSizeSlider, NULL, TRUE);
    std::wstringstream ss; 
    ss << std::fixed << std::setprecision(2) << g_imageScale.load();
    UpdateEditFieldText(&g_sizeField, ss.str());
    if (g_hSizeDisplay) 
        InvalidateRect(g_hSizeDisplay, NULL, TRUE);
}
void UpdateHotkeyEdits() {
    UpdateEditFieldText(&g_hotkeyToggleField, std::wstring(1, g_toggleHotkeyKey));
    UpdateEditFieldText(&g_hotkeySettingsField, std::wstring(1, g_settingsHotkeyKey));
    UpdateEditFieldText(&g_hotkeyCycleField, std::wstring(1, g_cycleHotkeyKey));
    UpdateEditFieldText(&g_hotkeyQuitField, std::wstring(1, g_quitHotkeyKey));
    if (g_hHotkeyToggleDisplay) 
        InvalidateRect(g_hHotkeyToggleDisplay, NULL, TRUE);
    if (g_hHotkeySettingsDisplay) 
        InvalidateRect(g_hHotkeySettingsDisplay, NULL, TRUE);
    if (g_hHotkeyCycleDisplay) 
        InvalidateRect(g_hHotkeyCycleDisplay, NULL, TRUE);
    if (g_hHotkeyQuitDisplay) 
        InvalidateRect(g_hHotkeyQuitDisplay, NULL, TRUE);
}
void UnregisterHotkeys() {
    if (g_hwnd) {
        UnregisterHotKey(g_hwnd, 1);
        UnregisterHotKey(g_hwnd, 2);
        UnregisterHotKey(g_hwnd, 3);
        UnregisterHotKey(g_hwnd, 4);
    }
}
bool RegisterHotkeys() {
    UnregisterHotkeys();
    if (!g_hwnd) 
        return false;
    if (g_toggleHotkeyKey && !RegisterHotKey(g_hwnd, 1, MOD_ALT | MOD_NOREPEAT, g_toggleHotkeyKey)) 
        return false;
    if (g_settingsHotkeyKey && !RegisterHotKey(g_hwnd, 2, MOD_ALT | MOD_NOREPEAT, g_settingsHotkeyKey)) 
        return false;
    if (g_cycleHotkeyKey && !RegisterHotKey(g_hwnd, 3, MOD_ALT | MOD_NOREPEAT, g_cycleHotkeyKey)) 
        return false;
    if (g_quitHotkeyKey && !RegisterHotKey(g_hwnd, 4, MOD_ALT | MOD_NOREPEAT, g_quitHotkeyKey)) 
        return false;
    return true;
}
void ToggleHideOverlay() {
    g_hideOverlay = !g_hideOverlay;
    if (g_currentOverlay) {
        ShowWindow(g_currentOverlay, g_hideOverlay ? SW_HIDE : SW_SHOWNA);
        if (!g_hideOverlay) {
            SetWindowPos(g_currentOverlay, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
            std::shared_ptr<AnimatedGIF> gif;
            {
            std::lock_guard<std::mutex> lock(g_renderDataMutex); 
            gif = g_animatedGIFShared; 
            }
            if (gif && gif->IsInitialized() && gif->IsAnimated()) 
                gif->StartAnimation(g_currentOverlay);
            RenderOverlay();
        }
        else {
            std::shared_ptr<AnimatedGIF> gif;
            {
            std::lock_guard<std::mutex> lock(g_renderDataMutex); 
            gif = g_animatedGIFShared; 
            }
            if (gif) 
            gif->StopAnimation();
        }
    }
    UpdateAllButtons();
    g_settingsChanged = true;
}
void ToggleReverseDirection() {
    g_reverseDirection = !g_reverseDirection;
    if 
    (g_hReverseDirectionCheckbox) 
    InvalidateRect(g_hReverseDirectionCheckbox, NULL, TRUE);
    g_settingsChanged = true;
}
void CycleImageSelection() {
    if (g_imageFiles.empty()) 
        return;
    int count = (int)g_imageFiles.size();
    if (g_currentImageIndex == -1) {
        g_currentImageIndex = g_reverseDirection ? count - 1 : 0;
    }
    else {
        if (g_reverseDirection) {
            g_currentImageIndex--;
            if (g_currentImageIndex < 0) 
                g_currentImageIndex = count - 1;
        }
        else {
            g_currentImageIndex++;
            if (g_currentImageIndex >= count) 
                g_currentImageIndex = 0;
        }
    }
    g_imageListSelected = g_currentImageIndex;
    int visibleItems = VISIBLE_ITEMS_IMAGES;
    if (g_currentImageIndex < g_imageListScrollPos)
        g_imageListScrollPos = g_currentImageIndex;
    else if (g_currentImageIndex >= g_imageListScrollPos + visibleItems)
        g_imageListScrollPos = g_currentImageIndex - visibleItems + 1;
    if (g_hImageList) 
        InvalidateRect(g_hImageList, NULL, TRUE);
    SetCurrentImageSettings(g_currentImageIndex);
    CreateOverlayWindow(g_imageFiles[g_currentImageIndex].path, true);
}
LRESULT HandleHotkey(WPARAM wParam) {
    switch (wParam) {
        case 1: 
            ToggleHideOverlay(); 
            return 0;
        case 2: 
        if (!IsWindowVisible(g_hwnd)) {
            ShowWindow(g_hwnd, SW_RESTORE); 
            ForceForegroundWindow(g_hwnd); 
            g_windowVisible = true; 
        }
            else {
                if (IsIconic(g_hwnd)) {
                    ShowWindow(g_hwnd, SW_RESTORE); 
                    ForceForegroundWindow(g_hwnd); 
                    g_windowVisible = true; 
                }
                else {
                    if (GetForegroundWindow() == g_hwnd) {
                        ShowWindow(g_hwnd, SW_HIDE); 
                        g_windowVisible = false; 
                    }
                    else {
                        ForceForegroundWindow(g_hwnd); 
                    }
                }
            }
            return 0;
        case 3: 
            CycleImageSelection(); 
            return 0;
        case 4: 
            if (g_hwnd) 
                PostMessage(g_hwnd, WM_DESTROY, 0, 0); 
            return 0;
    }
    return 0;
}
void DrawCustomSlider(HDC hdc, RECT rc, int value, bool darkTheme, bool enabled) {
    int width = rc.right - rc.left, height = rc.bottom - rc.top;
    int margin = 5;
    int visualThumbHalfWidth = 4;
    int trackStart = rc.left, trackEnd = rc.right;
    int usableTrackStart = trackStart + margin, usableTrackEnd = trackEnd - margin;
    int usableTrackWidth = usableTrackEnd - usableTrackStart;
    int tickStart = trackStart + margin, tickEnd = trackEnd - margin;
    HBRUSH hTrackBrush = CreateSolidBrush(darkTheme ? RGB(25,25,25) : RGB(255,255,255));
    RECT trackRc = {trackStart, rc.top, trackEnd, rc.bottom};
    FillRect(hdc, &trackRc, hTrackBrush);
    DeleteObject(hTrackBrush);
    int thumbPos = usableTrackStart + (value * usableTrackWidth / 200);
    for (int i = 0; i <= 10; i++) {
        int tickPos = tickStart + (i * (tickEnd - tickStart) / 10);
        HPEN hTickPen = CreatePen(PS_SOLID, 1, darkTheme ? RGB(120,120,120) : RGB(180,180,180));
        HPEN hOldPen = (HPEN)SelectObject(hdc, hTickPen);
        MoveToEx(hdc, tickPos, rc.bottom - 6, NULL);
        LineTo(hdc, tickPos, rc.bottom - 3);
        SelectObject(hdc, hOldPen);
        DeleteObject(hTickPen);
    }
    int thumbHeight = 12;
    POINT thumbPoints[12];
    thumbPoints[0] = {thumbPos - visualThumbHalfWidth, rc.top - 1};
    thumbPoints[1] = {thumbPos + visualThumbHalfWidth, rc.top - 1};
    thumbPoints[2] = {thumbPos + visualThumbHalfWidth, rc.top + thumbHeight - 4};
    thumbPoints[3] = {thumbPos + 1, rc.top + thumbHeight - 1};
    thumbPoints[4] = {thumbPos, rc.top + thumbHeight};
    thumbPoints[5] = {thumbPos - 1, rc.top + thumbHeight - 1};
    thumbPoints[6] = {thumbPos - visualThumbHalfWidth, rc.top + thumbHeight - 4};
    HBRUSH hThumbBrush = CreateSolidBrush(enabled ? (darkTheme ? RGB(200,200,200) : RGB(80,80,80)) : (darkTheme ? RGB(100,100,100) : RGB(180,180,180)));
    HPEN hThumbPen = CreatePen(PS_SOLID, 1, enabled ? (darkTheme ? RGB(180,180,180) : RGB(60,60,60)) : (darkTheme ? RGB(80,80,80) : RGB(150,150,150)));
    HPEN hOldPen = (HPEN)SelectObject(hdc, hThumbPen);
    HBRUSH hOldBrush = (HBRUSH)SelectObject(hdc, hThumbBrush);
    Polygon(hdc, thumbPoints, 7);
    SelectObject(hdc, hOldPen);
    SelectObject(hdc, hOldBrush);
    DeleteObject(hThumbBrush);
    DeleteObject(hThumbPen);
    HPEN hBorder = CreatePen(PS_SOLID, 1, darkTheme ? RGB(80,80,80) : RGB(150,150,150));
    hOldPen = (HPEN)SelectObject(hdc, hBorder);
    hOldBrush = (HBRUSH)SelectObject(hdc, GetStockObject(NULL_BRUSH));
    Rectangle(hdc, trackStart, rc.top, trackEnd, rc.bottom);
    SelectObject(hdc, hOldPen);
    SelectObject(hdc, hOldBrush);
    DeleteObject(hBorder);
}
LRESULT CALLBACK SliderSubclassProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_ERASEBKGND: 
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);
            RECT rc;
            GetClientRect(hwnd, &rc);
            int sliderValue = (int)(g_imageScale.load() * 100.0f + 0.5f);
            DrawCustomSlider(hdc, rc, sliderValue, g_darkTheme, IsWindowEnabled(hwnd));
            EndPaint(hwnd, &ps);
            return 0;
        }
        default:
            return DefWindowProc(hwnd, msg, wParam, lParam);
    }
}
void ApplyTheme() {
    if (g_darkTheme) {
        g_textColor = RGB(240,240,240);
        g_buttonTextColor = RGB(240,240,240);
        g_borderColor = RGB(100,100,100);
        g_buttonBorderColor = RGB(80,80,80);
        g_menuBackgroundColor = RGB(40,40,40);
        g_menuTextColor = RGB(240,240,240);
        g_menuSelectedColor = RGB(70,70,70);
        g_hBackgroundBrush.reset(CreateSolidBrush(RGB(25,25,25)));
        g_hButtonBrush.reset(CreateSolidBrush(RGB(45,45,45)));
        g_hButtonPressedBrush.reset(CreateSolidBrush(RGB(35,35,35)));
        g_hButtonHoverBrush.reset(CreateSolidBrush(RGB(60,60,60)));
        g_hCheckboxBrush.reset(CreateSolidBrush(RGB(60,60,60)));
        g_hCheckboxHoverBrush.reset(CreateSolidBrush(RGB(80,80,80)));
        g_hActiveTabBrush.reset(CreateSolidBrush(RGB(70,70,70)));
        g_hMenuBackgroundBrush.reset(CreateSolidBrush(g_menuBackgroundColor));
        g_hMenuSelectedBrush.reset(CreateSolidBrush(g_menuSelectedColor));
        g_hListBoxBackgroundBrush.reset(CreateSolidBrush(RGB(35,35,35)));
        g_hListBoxSelectedBrush.reset(CreateSolidBrush(RGB(80,80,80)));
        g_hListBoxHoverBrush.reset(CreateSolidBrush(RGB(60,60,60)));
        g_hEditHoverBrush.reset(CreateSolidBrush(RGB(50,50,50)));
        if (g_hThemeButton) 
            SetWindowText(g_hThemeButton, L"Light");
    }
    else {
        g_textColor = RGB(40,40,40);
        g_buttonTextColor = RGB(40,40,40);
        g_borderColor = RGB(180,180,180);
        g_buttonBorderColor = RGB(200,200,200);
        g_menuBackgroundColor = RGB(240,240,240);
        g_menuTextColor = RGB(40,40,40);
        g_menuSelectedColor = RGB(210,210,210);
        g_hBackgroundBrush.reset(CreateSolidBrush(RGB(255,255,255)));
        g_hButtonBrush.reset(CreateSolidBrush(RGB(235,235,235)));
        g_hButtonPressedBrush.reset(CreateSolidBrush(RGB(245,245,245)));
        g_hButtonHoverBrush.reset(CreateSolidBrush(RGB(220,220,220)));
        g_hCheckboxBrush.reset(CreateSolidBrush(RGB(220,220,220)));
        g_hCheckboxHoverBrush.reset(CreateSolidBrush(RGB(200,200,200)));
        g_hActiveTabBrush.reset(CreateSolidBrush(RGB(210,210,210)));
        g_hMenuBackgroundBrush.reset(CreateSolidBrush(g_menuBackgroundColor));
        g_hMenuSelectedBrush.reset(CreateSolidBrush(g_menuSelectedColor));
        g_hListBoxBackgroundBrush.reset(CreateSolidBrush(RGB(245,245,245)));
        g_hListBoxSelectedBrush.reset(CreateSolidBrush(RGB(200,200,200)));
        g_hListBoxHoverBrush.reset(CreateSolidBrush(RGB(220,220,220)));
        g_hEditHoverBrush.reset(CreateSolidBrush(RGB(230,230,230)));
        if (g_hThemeButton) 
            SetWindowText(g_hThemeButton, L"Dark");
    }
    if (!g_hMenuFont) 
        g_hMenuFont = CreateFont(14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_OUTLINE_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, VARIABLE_PITCH, L"Segoe UI");
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
    g_settingsChanged = true;
}
bool IsValidHotkeyChar(wchar_t c) {
    wchar_t uc = std::towupper(c);
    return (uc >= L'A' && uc <= L'Z') || (uc >= L'0' && uc <= L'9');
}
bool CheckHotkeyConflict(wchar_t newKey, int excludeId, std::wstring& conflictAction) {
    if (newKey == 0) 
        return false;
    if (excludeId != 1 && newKey == g_toggleHotkeyKey) {
        conflictAction = L"Toggle Overlay Visibility"; 
        return true; 
    }
    if (excludeId != 2 && newKey == g_settingsHotkeyKey) {
        conflictAction = L"Toggle Settings Window Visibility"; 
        return true; 
    }
    if (excludeId != 3 && newKey == g_cycleHotkeyKey) {
        conflictAction = L"Switch To Next Image From List"; 
        return true; 
    }
    if (excludeId != 4 && newKey == g_quitHotkeyKey) {
        conflictAction = L"Quit"; 
        return true; 
    }
    return false;
}
void DrawMessageBoxButton(HDC hdc, HWND hwnd, bool isPressed, bool isFocused) {
    RECT rc; 
    GetClientRect(hwnd, &rc);
    FillRect(hdc, &rc, isPressed ? g_hButtonPressedBrush.get() : g_hButtonBrush.get());
    HPEN hBorderPen = CreatePen(PS_SOLID, 1, isFocused ? (g_darkTheme ? RGB(255,255,255) : RGB(0,0,0)) : g_buttonBorderColor);
    HPEN hOldPen = (HPEN)SelectObject(hdc, hBorderPen);
    HBRUSH hOldBrush = (HBRUSH)SelectObject(hdc, GetStockObject(NULL_BRUSH));
    Rectangle(hdc, rc.left, rc.top, rc.right, rc.bottom);
    SelectObject(hdc, hOldBrush);
    SelectObject(hdc, hOldPen);
    DeleteObject(hBorderPen);
    if (isPressed) 
        OffsetRect(&rc, 1, 1);
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
            if (LOWORD(wParam) != WA_INACTIVE) 
                SetFocus(GetDlgItem(hwnd, 1)); 
            return 0;
        case WM_KEYDOWN: 
            if (wParam == VK_SPACE) {
                buttonPressed = true; 
                InvalidateRect(GetDlgItem(hwnd, 1), NULL, TRUE); 
                ShowWindow(hwnd, SW_HIDE); 
                DestroyWindow(hwnd); 
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
        case WM_CTLCOLORSTATIC: 
            SetBkColor((HDC)wParam, g_darkTheme ? RGB(25,25,25) : RGB(255,255,255)); 
            SetTextColor((HDC)wParam, g_textColor); 
            return (LRESULT)g_hBackgroundBrush.get();
        case WM_CTLCOLORBTN:
            SetBkColor((HDC)wParam, g_darkTheme ? RGB(25,25,25) : RGB(255,255,255)); 
            SetTextColor((HDC)wParam, g_textColor); 
            return (LRESULT)g_hBackgroundBrush.get();
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);
            RECT cr; 
            GetClientRect(hwnd, &cr);
            FillRect(hdc, &cr, g_hBackgroundBrush.get());
            HFONT hOldFont = (HFONT)SelectObject(hdc, g_hTabFont);
            SetTextColor(hdc, g_textColor);
            SetBkMode(hdc, TRANSPARENT);
            RECT textRect = cr;
            textRect.left += 10; 
            textRect.right -= 10;
            textRect.top += 5; 
            textRect.bottom = textRect.top + 40;
            std::wstring* pMessage = (std::wstring*)GetWindowLongPtr(hwnd, GWLP_USERDATA);
            if (pMessage) 
                DrawText(hdc, pMessage->c_str(), -1, &textRect, DT_LEFT | DT_WORDBREAK);
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
            if (pMessage) 
                delete pMessage;
            int remaining = GetOpenMessageBoxCount() - 1;
            if (remaining > 0) 
                ActivateNextMessageBox(hwnd);
            else {
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
    if (hwndParent) 
        EnableWindow(hwndParent, FALSE);
    RECT pr;
    if (hwndParent) 
        GetWindowRect(hwndParent, &pr);
    else GetWindowRect(GetDesktopWindow(), &pr);
    int w = 260, h = 130;
    int x = pr.left + (pr.right - pr.left - w)/2;
    int y = pr.top + (pr.bottom - pr.top - h)/2;
    HWND hMsgBox = CreateWindowEx(WS_EX_DLGMODALFRAME, L"OwnerDrawMessageBox", L"Custom Desktop Overlay", WS_POPUP | WS_CAPTION | WS_SYSMENU, x, y, w, h, hwndParent, NULL, GetModuleHandle(NULL), NULL);
    std::wstring* pMessage = new std::wstring(message);
    SetWindowLongPtr(hMsgBox, GWLP_USERDATA, (LONG_PTR)pMessage);
    HWND hButton = CreateWindowW(L"BUTTON", L"OK", WS_VISIBLE | WS_CHILD | BS_OWNERDRAW, w/2 - 30, h - 60, 60, 25, hMsgBox, (HMENU)1, GetModuleHandle(NULL), NULL);
    SendMessage(hButton, WM_SETFONT, (WPARAM)g_hTabFont, TRUE);
    ShowWindow(hMsgBox, SW_SHOW);
    SetFocus(hButton);
    SetProp(hMsgBox, L"MessageText", (HANDLE)pMessage);
}
void SetCurrentImageSettings(int index) {
    if (index < 0 || index >= (int)g_imageFiles.size()) 
        return;
    g_selectedMonitor = g_imageFiles[index].settings.monitor;
    g_offsetX = g_imageFiles[index].settings.offsetX;
    g_offsetY = g_imageFiles[index].settings.offsetY;
    g_imageScale.store(g_imageFiles[index].settings.scale, std::memory_order_relaxed);
    UpdateOffsetEditControls();
    UpdateScaleControls();
    UpdateMonitorSelection();
    if (g_hMonitorList) 
        InvalidateRect(g_hMonitorList, NULL, TRUE);
    g_currentImageIndex = index;
    g_imageListSelected = index;
    if (g_hImageList) 
        InvalidateRect(g_hImageList, NULL, TRUE);
}
void SaveCurrentImageSettings() {
    if (g_currentImageIndex == -1) 
        return;
    g_imageFiles[g_currentImageIndex].settings.monitor = g_selectedMonitor;
    g_imageFiles[g_currentImageIndex].settings.offsetX = g_offsetX;
    g_imageFiles[g_currentImageIndex].settings.offsetY = g_offsetY;
    g_imageFiles[g_currentImageIndex].settings.scale = g_imageScale.load();
    g_settingsChanged = true;
}
LRESULT CALLBACK WindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    static bool dragging = false;
    switch (uMsg) {
        case WM_CREATE: {
            g_hwnd = hwnd;
            if (!g_hTabFont) 
                g_hTabFont = CreateFont(14,0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_OUTLINE_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,VARIABLE_PITCH,L"Segoe UI");
            g_hMenuFont = CreateFont(14,0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_OUTLINE_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,VARIABLE_PITCH,L"Segoe UI");
            g_hTabButton1 = CreateWindowW(L"BUTTON", L"Select Images", WS_VISIBLE | WS_CHILD | BS_OWNERDRAW, 0, 0, 99, 25, hwnd, (HMENU)ID_TAB_BUTTON1, ((LPCREATESTRUCT)lParam)->hInstance, NULL);
            g_hTabButton2 = CreateWindowW(L"BUTTON", L"Location && Size", WS_VISIBLE | WS_CHILD | BS_OWNERDRAW, 99, 0, 99, 25, hwnd, (HMENU)ID_TAB_BUTTON2, ((LPCREATESTRUCT)lParam)->hInstance, NULL);
            g_hTabButton3 = CreateWindowW(L"BUTTON", L"Hotkeys", WS_VISIBLE | WS_CHILD | BS_OWNERDRAW, 198, 0, 99, 25, hwnd, (HMENU)ID_TAB_BUTTON3, ((LPCREATESTRUCT)lParam)->hInstance, NULL);
            g_hImageList = CreateWindowEx(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE, 7, 47, 280, VISIBLE_ITEMS_IMAGES*ITEM_HEIGHT+2, hwnd, (HMENU)ID_IMAGE_LIST, ((LPCREATESTRUCT)lParam)->hInstance, NULL);
            g_listBoxStates[g_hImageList] = {-1, -1, -1, false, false, {0,0}};
            g_hBrowseButton = CreateWindowW(L"BUTTON", L"Browse Image", WS_VISIBLE | WS_CHILD | BS_OWNERDRAW, 7, 196, 280, 30, hwnd, (HMENU)ID_BROWSE_BUTTON, ((LPCREATESTRUCT)lParam)->hInstance, NULL);
            g_hHideOverlayCheckbox = CreateWindowW(L"BUTTON", L" Hide Overlay", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, 7, 249, 90, 20, hwnd, (HMENU)ID_HIDE_OVERLAY_CHECKBOX, ((LPCREATESTRUCT)lParam)->hInstance, NULL);
            g_hThemeButton = CreateWindowW(L"BUTTON", L"Dark", WS_VISIBLE | WS_CHILD | BS_OWNERDRAW, 237, 249, 50, 20, hwnd, (HMENU)ID_THEME_BUTTON, ((LPCREATESTRUCT)lParam)->hInstance, NULL);
            g_hMonitorList = CreateWindowEx(0, L"STATIC", L"", WS_CHILD, 7, 47, 280, VISIBLE_ITEMS_MONITORS*ITEM_HEIGHT+2, hwnd, (HMENU)ID_MONITOR_LIST, ((LPCREATESTRUCT)lParam)->hInstance, NULL);
            g_listBoxStates[g_hMonitorList] = {-1, -1, -1, false, false, {0,0}};
            g_hOffsetXDisplay = CreateWindowEx(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_OWNERDRAW, 25, 121, 50, 20, hwnd, (HMENU)ID_OFFSET_X_DISPLAY, ((LPCREATESTRUCT)lParam)->hInstance, NULL);
            g_hOffsetYDisplay = CreateWindowEx(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_OWNERDRAW, 25, 144, 50, 20, hwnd, (HMENU)ID_OFFSET_Y_DISPLAY, ((LPCREATESTRUCT)lParam)->hInstance, NULL);
            g_hSizeSlider = CreateWindowEx(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_OWNERDRAW, 7, 188, 280, 20, hwnd, (HMENU)ID_SIZE_SLIDER, ((LPCREATESTRUCT)lParam)->hInstance, NULL);
            g_hSizeDisplay = CreateWindowEx(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_OWNERDRAW, 7, 210, 50, 20, hwnd, (HMENU)ID_SIZE_DISPLAY, ((LPCREATESTRUCT)lParam)->hInstance, NULL);
            g_hHotkeyToggleDisplay = CreateWindowEx(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_OWNERDRAW, 40, 47, 30, 20, hwnd, (HMENU)ID_HOTKEY_TOGGLE_DISPLAY, ((LPCREATESTRUCT)lParam)->hInstance, NULL);
            g_hHotkeySettingsDisplay = CreateWindowEx(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_OWNERDRAW, 40, 89, 30, 20, hwnd, (HMENU)ID_HOTKEY_SETTINGS_DISPLAY, ((LPCREATESTRUCT)lParam)->hInstance, NULL);
            g_hHotkeyCycleDisplay = CreateWindowEx(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_OWNERDRAW, 40, 131, 30, 20, hwnd, (HMENU)ID_HOTKEY_CYCLE_DISPLAY, ((LPCREATESTRUCT)lParam)->hInstance, NULL);
            g_hHotkeyQuitDisplay = CreateWindowEx(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_OWNERDRAW, 40, 186, 30, 20, hwnd, (HMENU)ID_HOTKEY_QUIT_DISPLAY, ((LPCREATESTRUCT)lParam)->hInstance, NULL);
            g_hResetToggle = CreateWindowW(L"BUTTON", L"Reset", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, 75, 47, 60, 20, hwnd, (HMENU)ID_RESET_TOGGLE, ((LPCREATESTRUCT)lParam)->hInstance, NULL);
            g_hResetSettings = CreateWindowW(L"BUTTON", L"Reset", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, 75, 89, 60, 20, hwnd, (HMENU)ID_RESET_SETTINGS, ((LPCREATESTRUCT)lParam)->hInstance, NULL);
            g_hResetCycle = CreateWindowW(L"BUTTON", L"Reset", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, 75, 131, 60, 20, hwnd, (HMENU)ID_RESET_CYCLE, ((LPCREATESTRUCT)lParam)->hInstance, NULL);
            g_hReverseDirectionCheckbox = CreateWindowW(L"BUTTON", L" Reverse Direction", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, 7, 151, 115, 20, hwnd, (HMENU)ID_REVERSE_DIRECTION_CHECKBOX, ((LPCREATESTRUCT)lParam)->hInstance, NULL);
            g_hResetQuit = CreateWindowW(L"BUTTON", L"Reset", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, 75, 186, 60, 20, hwnd, (HMENU)ID_RESET_QUIT, ((LPCREATESTRUCT)lParam)->hInstance, NULL);
            g_offsetXField.hwnd = g_hOffsetXDisplay;
            g_offsetYField.hwnd = g_hOffsetYDisplay;
            g_sizeField.hwnd = g_hSizeDisplay;
            g_hotkeyToggleField.hwnd = g_hHotkeyToggleDisplay;
            g_hotkeySettingsField.hwnd = g_hHotkeySettingsDisplay;
            g_hotkeyCycleField.hwnd = g_hHotkeyCycleDisplay;
            g_hotkeyQuitField.hwnd = g_hHotkeyQuitDisplay;
            g_oldImageListProc = (WNDPROC)SetWindowLongPtr(g_hImageList, GWLP_WNDPROC, (LONG_PTR)ImageListSubclassProc);
            g_oldMonitorListProc = (WNDPROC)SetWindowLongPtr(g_hMonitorList, GWLP_WNDPROC, (LONG_PTR)MonitorListSubclassProc);
            g_oldOffsetXProc = (WNDPROC)SetWindowLongPtr(g_hOffsetXDisplay, GWLP_WNDPROC, (LONG_PTR)OffsetXSubclassProc);
            g_oldOffsetYProc = (WNDPROC)SetWindowLongPtr(g_hOffsetYDisplay, GWLP_WNDPROC, (LONG_PTR)OffsetYSubclassProc);
            g_oldSizeProc = (WNDPROC)SetWindowLongPtr(g_hSizeDisplay, GWLP_WNDPROC, (LONG_PTR)SizeSubclassProc);
            g_oldHotkeyToggleProc = (WNDPROC)SetWindowLongPtr(g_hHotkeyToggleDisplay, GWLP_WNDPROC, (LONG_PTR)HotkeyToggleSubclassProc);
            g_oldHotkeySettingsProc = (WNDPROC)SetWindowLongPtr(g_hHotkeySettingsDisplay, GWLP_WNDPROC, (LONG_PTR)HotkeySettingsSubclassProc);
            g_oldHotkeyCycleProc = (WNDPROC)SetWindowLongPtr(g_hHotkeyCycleDisplay, GWLP_WNDPROC, (LONG_PTR)HotkeyCycleSubclassProc);
            g_oldHotkeyQuitProc = (WNDPROC)SetWindowLongPtr(g_hHotkeyQuitDisplay, GWLP_WNDPROC, (LONG_PTR)HotkeyQuitSubclassProc);
            g_oldTabButton1Proc = (WNDPROC)SetWindowLongPtr(g_hTabButton1, GWLP_WNDPROC, (LONG_PTR)TabButton1SubclassProc);
            g_oldTabButton2Proc = (WNDPROC)SetWindowLongPtr(g_hTabButton2, GWLP_WNDPROC, (LONG_PTR)TabButton2SubclassProc);
            g_oldTabButton3Proc = (WNDPROC)SetWindowLongPtr(g_hTabButton3, GWLP_WNDPROC, (LONG_PTR)TabButton3SubclassProc);
            g_oldBrowseButtonProc = (WNDPROC)SetWindowLongPtr(g_hBrowseButton, GWLP_WNDPROC, (LONG_PTR)BrowseButtonSubclassProc);
            g_oldThemeButtonProc = (WNDPROC)SetWindowLongPtr(g_hThemeButton, GWLP_WNDPROC, (LONG_PTR)ThemeButtonSubclassProc);
            g_oldResetToggleProc = (WNDPROC)SetWindowLongPtr(g_hResetToggle, GWLP_WNDPROC, (LONG_PTR)ResetToggleSubclassProc);
            g_oldResetSettingsProc = (WNDPROC)SetWindowLongPtr(g_hResetSettings, GWLP_WNDPROC, (LONG_PTR)ResetSettingsSubclassProc);
            g_oldResetCycleProc = (WNDPROC)SetWindowLongPtr(g_hResetCycle, GWLP_WNDPROC, (LONG_PTR)ResetCycleSubclassProc);
            g_oldResetQuitProc = (WNDPROC)SetWindowLongPtr(g_hResetQuit, GWLP_WNDPROC, (LONG_PTR)ResetQuitSubclassProc);
            g_oldHideOverlayCheckboxProc = (WNDPROC)SetWindowLongPtr(g_hHideOverlayCheckbox, GWLP_WNDPROC, (LONG_PTR)HideOverlayCheckboxSubclassProc);
            g_oldReverseDirectionCheckboxProc = (WNDPROC)SetWindowLongPtr(g_hReverseDirectionCheckbox, GWLP_WNDPROC, (LONG_PTR)ReverseDirectionCheckboxSubclassProc);
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
                SendMessage(g_hHotkeyCycleDisplay, WM_SETFONT, (WPARAM)g_hTabFont, TRUE);
                SendMessage(g_hHotkeyQuitDisplay, WM_SETFONT, (WPARAM)g_hTabFont, TRUE);
                SendMessage(g_hResetToggle, WM_SETFONT, (WPARAM)g_hTabFont, TRUE);
                SendMessage(g_hResetSettings, WM_SETFONT, (WPARAM)g_hTabFont, TRUE);
                SendMessage(g_hResetCycle, WM_SETFONT, (WPARAM)g_hTabFont, TRUE);
                SendMessage(g_hReverseDirectionCheckbox, WM_SETFONT, (WPARAM)g_hTabFont, TRUE);
                SendMessage(g_hResetQuit, WM_SETFONT, (WPARAM)g_hTabFont, TRUE);
            }
            LoadAllSettingsFromIni();
            ApplyTheme();
            UpdateMonitorList();
            CorrectScaleValue();
            CorrectOffsetValues();
            UpdateMonitorSelection();
            UpdateScaleControls();
            UpdateOffsetEditControls();
            UpdateHotkeyEdits();
            RestoreLastOverlay();
            UpdateHideOverlayCheckbox();
            RegisterHotkeys();
            g_prevMonitorCount = (int)g_monitors.size();
            for (const auto& m : g_monitors) 
                g_prevMonitorRects.push_back(m.rect);
            nid = {sizeof(NOTIFYICONDATA), hwnd, ID_TRAY_APP_ICON, NIF_ICON | NIF_MESSAGE | NIF_TIP, WM_TRAYICON};
            nid.hIcon = g_hMainIcon;
            wcsncpy_s(nid.szTip, L"Custom Desktop Overlay", _countof(nid.szTip)-1);
            Shell_NotifyIcon(NIM_ADD, &nid);
            if (g_windowX != CW_USEDEFAULT && g_windowY != CW_USEDEFAULT) 
                SetWindowPos(hwnd, NULL, g_windowX, g_windowY, 300, 300, SWP_NOZORDER | SWP_NOSIZE);
            ShowWindow(g_hImageList, SW_SHOW);
            ShowWindow(g_hBrowseButton, SW_SHOW);
            ShowWindow(g_hMonitorList, SW_HIDE);
            ShowWindow(g_hOffsetXDisplay, SW_HIDE);
            ShowWindow(g_hOffsetYDisplay, SW_HIDE);
            ShowWindow(g_hSizeSlider, SW_HIDE);
            ShowWindow(g_hSizeDisplay, SW_HIDE);
            ShowWindow(g_hHotkeyToggleDisplay, SW_HIDE);
            ShowWindow(g_hHotkeySettingsDisplay, SW_HIDE);
            ShowWindow(g_hHotkeyCycleDisplay, SW_HIDE);
            ShowWindow(g_hHotkeyQuitDisplay, SW_HIDE);
            ShowWindow(g_hResetToggle, SW_HIDE);
            ShowWindow(g_hResetSettings, SW_HIDE);
            ShowWindow(g_hResetCycle, SW_HIDE);
            ShowWindow(g_hReverseDirectionCheckbox, SW_HIDE);
            ShowWindow(g_hResetQuit, SW_HIDE);
            return 0;
        }
        case WM_DISPLAYCHANGE: {
            UpdateMonitorList();
            CorrectOffsetValues();
            UpdateOffsetBoundsDisplay();
            if (g_currentOverlay) {
                UpdateOverlayPosition(); 
                RenderOverlay(); 
            }
            g_prevMonitorCount = (int)g_monitors.size();
            g_prevMonitorRects.clear();
            {
            std::shared_lock<std::shared_mutex> lock(g_monitorMutex); 
            for (const auto& m : g_monitors) 
                g_prevMonitorRects.push_back(m.rect); 
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
                case ID_TRAY_NEXT_IMAGE: 
                    CycleImageSelection(); 
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
                    ShowWindow(g_hHotkeyCycleDisplay, SW_HIDE);
                    ShowWindow(g_hHotkeyQuitDisplay, SW_HIDE);
                    ShowWindow(g_hResetToggle, SW_HIDE);
                    ShowWindow(g_hResetSettings, SW_HIDE);
                    ShowWindow(g_hResetCycle, SW_HIDE);
                    ShowWindow(g_hReverseDirectionCheckbox, SW_HIDE);
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
                    ShowWindow(g_hHotkeyCycleDisplay, SW_HIDE);
                    ShowWindow(g_hHotkeyQuitDisplay, SW_HIDE);
                    ShowWindow(g_hResetToggle, SW_HIDE);
                    ShowWindow(g_hResetSettings, SW_HIDE);
                    ShowWindow(g_hResetCycle, SW_HIDE);
                    ShowWindow(g_hReverseDirectionCheckbox, SW_HIDE);
                    ShowWindow(g_hResetQuit, SW_HIDE);
                    UpdateOffsetEditControls();
                    UpdateScaleControls();
                    InvalidateRect(g_hMonitorList, NULL, TRUE);
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
                    ShowWindow(g_hHotkeyCycleDisplay, SW_SHOW);
                    ShowWindow(g_hHotkeyQuitDisplay, SW_SHOW);
                    ShowWindow(g_hResetToggle, SW_SHOW);
                    ShowWindow(g_hResetSettings, SW_SHOW);
                    ShowWindow(g_hResetCycle, SW_SHOW);
                    ShowWindow(g_hReverseDirectionCheckbox, SW_SHOW);
                    ShowWindow(g_hResetQuit, SW_SHOW);
                    InvalidateRect(hwnd, NULL, TRUE);
                    break;
                case ID_BROWSE_BUTTON: 
                    BrowseForImage(); 
                    break;
                case ID_CONTEXT_OPEN: if (g_imageListSelected >= 0 && g_imageListSelected < (int)g_imageFiles.size()) 
                    OpenFileLocation(g_imageFiles[g_imageListSelected].path); 
                    break;
                case ID_CONTEXT_REMOVE: 
                    if (g_imageListSelected >= 0) 
                        RemoveImageFromList(g_imageListSelected); 
                    break;
                case ID_HIDE_OVERLAY_CHECKBOX: 
                    if (HIWORD(wParam) == BN_CLICKED) 
                        ToggleHideOverlay(); 
                    break;
                case ID_THEME_BUTTON: 
                    if (HIWORD(wParam) == BN_CLICKED) 
                        ToggleTheme(); 
                    break;
                case ID_RESET_TOGGLE: {
                    wchar_t old = g_toggleHotkeyKey; 
                    if (ResetHotkeyField(1, DEFAULT_TOGGLE_KEY, g_toggleHotkeyKey, g_lastValidToggleKey, g_hotkeyToggleField) && g_hHotkeyToggleDisplay) 
                        InvalidateRect(g_hHotkeyToggleDisplay, NULL, TRUE); 
                    RegisterHotkeys(); 
                    if (old != g_toggleHotkeyKey) 
                        g_settingsChanged = true; 
                    break; 
                }
                case ID_RESET_SETTINGS: {
                    wchar_t old = g_settingsHotkeyKey; 
                    if (ResetHotkeyField(2, DEFAULT_SETTINGS_KEY, g_settingsHotkeyKey, g_lastValidSettingsKey, g_hotkeySettingsField) && g_hHotkeySettingsDisplay) 
                        InvalidateRect(g_hHotkeySettingsDisplay, NULL, TRUE); 
                    RegisterHotkeys(); 
                    if (old != g_settingsHotkeyKey) 
                        g_settingsChanged = true; 
                    break; 
                }
                case ID_RESET_CYCLE: {
                    wchar_t old = g_cycleHotkeyKey; 
                    if (ResetHotkeyField(3, DEFAULT_CYCLE_KEY, g_cycleHotkeyKey, g_lastValidCycleKey, g_hotkeyCycleField) && g_hHotkeyCycleDisplay) 
                        InvalidateRect(g_hHotkeyCycleDisplay, NULL, TRUE); 
                    RegisterHotkeys(); 
                    if (old != g_cycleHotkeyKey) 
                        g_settingsChanged = true; 
                    break; 
                }
                case ID_RESET_QUIT: {
                    wchar_t old = g_quitHotkeyKey; 
                    if (ResetHotkeyField(4, DEFAULT_QUIT_KEY, g_quitHotkeyKey, g_lastValidQuitKey, g_hotkeyQuitField) && g_hHotkeyQuitDisplay) 
                        InvalidateRect(g_hHotkeyQuitDisplay, NULL, TRUE); 
                    RegisterHotkeys(); 
                    if (old != g_quitHotkeyKey) 
                        g_settingsChanged = true; 
                    break; 
                }
                case ID_REVERSE_DIRECTION_CHECKBOX: 
                    if (HIWORD(wParam) == BN_CLICKED) 
                        ToggleReverseDirection(); 
                    break;
            }
            return 0;
        case WM_SYSCOMMAND: {
            UINT nID = wParam & 0xFFF0; 
            if (nID == SC_KEYMENU || nID == SC_MOUSEMENU) 
                return 0; 
            return DefWindowProc(hwnd, uMsg, wParam, lParam); }
        case WM_NCLBUTTONDBLCLK: 
            if (wParam == HTSYSMENU || wParam == HTCAPTION) 
                return 0; 
            return DefWindowProc(hwnd, uMsg, wParam, lParam);
        case WM_CONTEXTMENU:
            if ((HWND)wParam == g_hImageList && g_imageListSelected != -1) {
                POINT pt;
                if (lParam == -1) {
                    RECT rc; 
                    GetWindowRect(g_hImageList, &rc); 
                    pt = {rc.left + 10, rc.top + 10}; 
                }
                else pt = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)}; 
                HMENU hMenu = CreatePopupMenu();
                CreateOwnerDrawMenu(hMenu, 0, ID_CONTEXT_OPEN, L"Open File Location");
                CreateOwnerDrawMenu(hMenu, 1, ID_CONTEXT_REMOVE, L"Remove From Image List");
                SetForegroundWindow(g_hwnd);
                TrackPopupMenu(hMenu, TPM_LEFTALIGN | TPM_LEFTBUTTON, pt.x, pt.y, 0, g_hwnd, NULL);
                DestroyMenu(hMenu);
            }
            return 0;
        case WM_DRAWITEM: {
            DRAWITEMSTRUCT* pDraw = (DRAWITEMSTRUCT*)lParam;
            if (pDraw->CtlID == ID_HIDE_OVERLAY_CHECKBOX || pDraw->CtlID == ID_REVERSE_DIRECTION_CHECKBOX) {
                wchar_t buffer[256]; 
                GetWindowText(pDraw->hwndItem, buffer, 256);
                std::wstring text = buffer; 
                if (!text.empty() && text[0] == L' ') 
                    text = text.substr(1);
                bool isChecked = (pDraw->CtlID == ID_HIDE_OVERLAY_CHECKBOX) ? g_hideOverlay : g_reverseDirection;
                DrawOwnerDrawCheckbox(pDraw->hDC, pDraw->hwndItem, isChecked, text, false);
                return TRUE;
            }
            else if (pDraw->CtlID == ID_TAB_BUTTON1 || pDraw->CtlID == ID_TAB_BUTTON2 || pDraw->CtlID == ID_TAB_BUTTON3 || pDraw->CtlID == ID_BROWSE_BUTTON || pDraw->CtlID == ID_THEME_BUTTON || pDraw->CtlID == ID_RESET_TOGGLE || pDraw->CtlID == ID_RESET_SETTINGS || pDraw->CtlID == ID_RESET_CYCLE || pDraw->CtlID == ID_RESET_QUIT) {
                wchar_t buffer[256]; 
                GetWindowText(pDraw->hwndItem, buffer, 256);
                bool isPressed = (pDraw->itemState & ODS_SELECTED) != 0;
                bool isActiveTab = (pDraw->CtlID == ID_TAB_BUTTON1 && g_activeTab == 1) || (pDraw->CtlID == ID_TAB_BUTTON2 && g_activeTab == 2) || (pDraw->CtlID == ID_TAB_BUTTON3 && g_activeTab == 3);
                DrawOwnerDrawButton(pDraw->hDC, pDraw->hwndItem, isPressed, buffer, isActiveTab, false);
                return TRUE;
            }
            else if (pDraw->CtlID == ID_SIZE_SLIDER) {
                HDC hdcMem = CreateCompatibleDC(pDraw->hDC);
                HBITMAP hBitmap = CreateCompatibleBitmap(pDraw->hDC, pDraw->rcItem.right - pDraw->rcItem.left, pDraw->rcItem.bottom - pDraw->rcItem.top);
                HBITMAP hOldBitmap = (HBITMAP)SelectObject(hdcMem, hBitmap);
                int sliderValue = (int)(g_imageScale.load() * 100.0f + 0.5f);
                DrawCustomSlider(hdcMem, pDraw->rcItem, sliderValue, g_darkTheme, IsWindowEnabled(pDraw->hwndItem));
                BitBlt(pDraw->hDC, pDraw->rcItem.left, pDraw->rcItem.top, pDraw->rcItem.right - pDraw->rcItem.left, pDraw->rcItem.bottom - pDraw->rcItem.top, hdcMem, 0, 0, SRCCOPY);
                SelectObject(hdcMem, hOldBitmap);
                DeleteObject(hBitmap);
                DeleteDC(hdcMem);
                return TRUE;
            }
            else if (pDraw->CtlType == ODT_MENU) {
                std::wstring menuText; 
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
                        break;
                    case ID_TRAY_NEXT_IMAGE: 
                        menuText = L"Next Image"; 
                        break;
                    case ID_TRAY_EXIT: 
                        menuText = L"Quit"; 
                        break;
                    case ID_CONTEXT_OPEN: 
                        menuText = L"Open File Location"; 
                        break;
                    case ID_CONTEXT_REMOVE: 
                        menuText = L"Remove From Image List"; 
                        break;
                    default: 
                        if (pDraw->itemData) 
                            menuText = (const wchar_t*)pDraw->itemData;
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
        case WM_CTLCOLORSTATIC: {
            TCHAR className[256]; 
            GetClassName((HWND)lParam, className, 256);
            SetBkColor((HDC)wParam, g_darkTheme ? RGB(25,25,25) : RGB(255,255,255));
            SetTextColor((HDC)wParam, g_textColor);
            return (LRESULT)g_hBackgroundBrush.get();
        }
        case WM_CTLCOLORBTN:
            if ((g_hHideOverlayCheckbox && (HWND)lParam == g_hHideOverlayCheckbox) || (g_hReverseDirectionCheckbox && (HWND)lParam == g_hReverseDirectionCheckbox))
                return 0;
            SetBkColor((HDC)wParam, g_darkTheme ? RGB(25,25,25) : RGB(255,255,255));
            SetTextColor((HDC)wParam, g_textColor);
            return (LRESULT)g_hBackgroundBrush.get();
        case WM_CTLCOLOREDIT:
            SetBkColor((HDC)wParam, g_darkTheme ? RGB(25,25,25) : RGB(255,255,255));
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
                CreateOwnerDrawMenu(hMenu, 2, ID_TRAY_NEXT_IMAGE, L"Next Image");
                CreateOwnerDrawMenu(hMenu, 3, ID_TRAY_EXIT, L"Quit");
                SetForegroundWindow(g_hwnd);
                TrackPopupMenu(hMenu, TPM_BOTTOMALIGN | TPM_LEFTALIGN, pt.x, pt.y, 0, g_hwnd, NULL);
                DestroyMenu(hMenu);
            }
            return 0;
        case WM_DESTROY:
            ShowWindow(g_hwnd, SW_HIDE);
            if (g_currentOverlay) 
                ShowWindow(g_currentOverlay, SW_HIDE);
            UpdateWindow(g_hwnd);
            g_programExiting = true;
            if (g_monitorNamesThread.joinable()) 
                g_monitorNamesThread.join();
            if (g_currentOverlay) {
                DestroyWindow(g_currentOverlay);
                g_currentOverlay = NULL; 
                g_currentOverlayAtomic = nullptr; 
            }
            if (g_oldImageListProc) 
                SetWindowLongPtr(g_hImageList, GWLP_WNDPROC, (LONG_PTR)g_oldImageListProc);
            if (g_oldMonitorListProc) 
                SetWindowLongPtr(g_hMonitorList, GWLP_WNDPROC, (LONG_PTR)g_oldMonitorListProc);
            if (g_oldOffsetXProc) 
                SetWindowLongPtr(g_hOffsetXDisplay, GWLP_WNDPROC, (LONG_PTR)g_oldOffsetXProc);
            if (g_oldOffsetYProc) 
                SetWindowLongPtr(g_hOffsetYDisplay, GWLP_WNDPROC, (LONG_PTR)g_oldOffsetYProc);
            if (g_oldSizeProc) 
                SetWindowLongPtr(g_hSizeDisplay, GWLP_WNDPROC, (LONG_PTR)g_oldSizeProc);
            if (g_oldHotkeyToggleProc) 
                SetWindowLongPtr(g_hHotkeyToggleDisplay, GWLP_WNDPROC, (LONG_PTR)g_oldHotkeyToggleProc);
            if (g_oldHotkeySettingsProc) 
                SetWindowLongPtr(g_hHotkeySettingsDisplay, GWLP_WNDPROC, (LONG_PTR)g_oldHotkeySettingsProc);
            if (g_oldHotkeyCycleProc) 
                SetWindowLongPtr(g_hHotkeyCycleDisplay, GWLP_WNDPROC, (LONG_PTR)g_oldHotkeyCycleProc);
            if (g_oldHotkeyQuitProc) 
                SetWindowLongPtr(g_hHotkeyQuitDisplay, GWLP_WNDPROC, (LONG_PTR)g_oldHotkeyQuitProc);
            if (g_oldTabButton1Proc) 
                SetWindowLongPtr(g_hTabButton1, GWLP_WNDPROC, (LONG_PTR)g_oldTabButton1Proc);
            if (g_oldTabButton2Proc) 
                SetWindowLongPtr(g_hTabButton2, GWLP_WNDPROC, (LONG_PTR)g_oldTabButton2Proc);
            if (g_oldTabButton3Proc) 
                SetWindowLongPtr(g_hTabButton3, GWLP_WNDPROC, (LONG_PTR)g_oldTabButton3Proc);
            if (g_oldBrowseButtonProc) 
                SetWindowLongPtr(g_hBrowseButton, GWLP_WNDPROC, (LONG_PTR)g_oldBrowseButtonProc);
            if (g_oldThemeButtonProc) 
                SetWindowLongPtr(g_hThemeButton, GWLP_WNDPROC, (LONG_PTR)g_oldThemeButtonProc);
            if (g_oldResetToggleProc) 
                SetWindowLongPtr(g_hResetToggle, GWLP_WNDPROC, (LONG_PTR)g_oldResetToggleProc);
            if (g_oldResetSettingsProc) 
                SetWindowLongPtr(g_hResetSettings, GWLP_WNDPROC, (LONG_PTR)g_oldResetSettingsProc);
            if (g_oldResetCycleProc) 
                SetWindowLongPtr(g_hResetCycle, GWLP_WNDPROC, (LONG_PTR)g_oldResetCycleProc);
            if (g_oldResetQuitProc) 
                SetWindowLongPtr(g_hResetQuit, GWLP_WNDPROC, (LONG_PTR)g_oldResetQuitProc);
            if (g_oldHideOverlayCheckboxProc) 
                SetWindowLongPtr(g_hHideOverlayCheckbox, GWLP_WNDPROC, (LONG_PTR)g_oldHideOverlayCheckboxProc);
            if (g_oldReverseDirectionCheckboxProc) 
                SetWindowLongPtr(g_hReverseDirectionCheckbox, GWLP_WNDPROC, (LONG_PTR)g_oldReverseDirectionCheckboxProc);
            StopRenderThread();
            UnregisterHotkeys();
            if (g_settingsChanged) 
                SaveAllSettingsToIni();
            Shell_NotifyIcon(NIM_DELETE, &nid);
            {
            std::lock_guard<std::mutex> lock(g_renderDataMutex); 
            g_cachedBitmapShared.reset(); 
            g_animatedGIFShared.reset(); 
            }
            SafeGdiplusShutdown();
            if (g_hTabFont) 
                DeleteObject(g_hTabFont);
            if (g_hMenuFont) 
                DeleteObject(g_hMenuFont);
            PostQuitMessage(0);
            return 0;
        case WM_ERASEBKGND: 
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);
            RECT cr; 
            GetClientRect(hwnd, &cr);
            FillRect(hdc, &cr, g_hBackgroundBrush.get());
            HPEN hLinePen = CreatePen(PS_SOLID, 1, g_borderColor);
            HPEN hOldPen = (HPEN)SelectObject(hdc, hLinePen);
            MoveToEx(hdc, 0, 25, NULL); 
            LineTo(hdc, cr.right, 25);
            SelectObject(hdc, hOldPen); 
            DeleteObject(hLinePen);
            HPEN hSepPen = CreatePen(PS_SOLID, 1, g_darkTheme ? RGB(60,60,60) : RGB(220,220,220));
            hOldPen = (HPEN)SelectObject(hdc, hSepPen);
            MoveToEx(hdc, 0, 245, NULL); 
            LineTo(hdc, cr.right, 245);
            SelectObject(hdc, hOldPen); 
            DeleteObject(hSepPen);
            HFONT hOldFont = (HFONT)SelectObject(hdc, g_hTabFont);
            SetTextColor(hdc, g_textColor);
            SetBkColor(hdc, g_darkTheme ? RGB(25,25,25) : RGB(255,255,255));
            SetBkMode(hdc, OPAQUE);
            bool showImageControls = IsWindowVisible(g_hImageList) && IsWindowEnabled(g_hImageList);
            bool showMonitorControls = IsWindowVisible(g_hMonitorList) && IsWindowEnabled(g_hMonitorList);
            bool showHotkeyControls = IsWindowVisible(g_hHotkeyToggleDisplay) && IsWindowEnabled(g_hHotkeyToggleDisplay);
            if (showImageControls) {
                RECT tr = {8, 30, 280, 50}; 
                FillRect(hdc, &tr, g_hBackgroundBrush.get()); 
                DrawText(hdc, L"Images List", -1, &tr, DT_LEFT | DT_TOP);
                RECT fr = {8, 230, 280, 245}; 
                FillRect(hdc, &fr, g_hBackgroundBrush.get()); 
                DrawText(hdc, L"Supported formats: .png, .jpg, .jpeg, .gif", -1, &fr, DT_LEFT | DT_TOP);
            }
            if (showMonitorControls) {
                RECT tr = {8, 30, 280, 50}; 
                FillRect(hdc, &tr, g_hBackgroundBrush.get()); 
                DrawText(hdc, L"Monitors List", -1, &tr, DT_LEFT | DT_TOP);
                RECT otr = {8, 104, 280, 130}; 
                FillRect(hdc, &otr, g_hBackgroundBrush.get()); 
                DrawText(hdc, L"Location Offset", -1, &otr, DT_LEFT | DT_TOP);
                RECT xlr = {8, 124, 20, 155}; 
                FillRect(hdc, &xlr, g_hBackgroundBrush.get()); 
                DrawText(hdc, L"X:", -1, &xlr, DT_LEFT | DT_TOP);
                RECT ylr = {8, 147, 20, 180}; 
                FillRect(hdc, &ylr, g_hBackgroundBrush.get()); 
                DrawText(hdc, L"Y:", -1, &ylr, DT_LEFT | DT_TOP);
                int maxX = 0, maxY = 0;
                {
                std::shared_lock<std::shared_mutex> lock(g_monitorMutex); 
                if (g_selectedMonitor >= 0 && g_selectedMonitor < (int)g_monitors.size()) {
                    const auto& m = g_monitors[g_selectedMonitor]; 
                    maxX = (m.rect.right - m.rect.left) / 2; 
                    maxY = (m.rect.bottom - m.rect.top) / 2; 
                }
                }
                std::wstring xb = L"[" + std::to_wstring(-maxX) + L"; " + std::to_wstring(maxX) + L"]";
                std::wstring yb = L"[" + std::to_wstring(-maxY) + L"; " + std::to_wstring(maxY) + L"]";
                RECT xbr = {85, 124, 220, 155}; 
                FillRect(hdc, &xbr, g_hBackgroundBrush.get()); 
                DrawText(hdc, xb.c_str(), -1, &xbr, DT_LEFT | DT_TOP);
                RECT ybr = {85, 147, 220, 180}; 
                FillRect(hdc, &ybr, g_hBackgroundBrush.get()); 
                DrawText(hdc, yb.c_str(), -1, &ybr, DT_LEFT | DT_TOP);
                RECT str = {8, 171, 280, 185}; 
                FillRect(hdc, &str, g_hBackgroundBrush.get()); 
                DrawText(hdc, L"Size", -1, &str, DT_LEFT | DT_TOP);
                std::wstring szb = L"[0.00; 2.00]";
                RECT szbr = {67, 213, 200, 240}; 
                FillRect(hdc, &szbr, g_hBackgroundBrush.get()); 
                DrawText(hdc, szb.c_str(), -1, &szbr, DT_LEFT | DT_TOP);
            }
            if (showHotkeyControls) {
                RECT ttr = {8, 30, 280, 50}; 
                FillRect(hdc, &ttr, g_hBackgroundBrush.get()); 
                DrawText(hdc, L"Toggle Overlay Visibility:", -1, &ttr, DT_LEFT | DT_TOP);
                RECT klr = {8, 50, 280, 68}; 
                FillRect(hdc, &klr, g_hBackgroundBrush.get()); 
                DrawText(hdc, L"Alt +", -1, &klr, DT_LEFT | DT_TOP);
                RECT str = {8, 72, 280, 90}; 
                FillRect(hdc, &str, g_hBackgroundBrush.get()); 
                DrawText(hdc, L"Toggle Settings Window Visibility:", -1, &str, DT_LEFT | DT_TOP);
                RECT klr2 = {8, 92, 280, 108}; 
                FillRect(hdc, &klr2, g_hBackgroundBrush.get()); 
                DrawText(hdc, L"Alt +", -1, &klr2, DT_LEFT | DT_TOP);
                RECT ctr = {8, 114, 282, 130}; 
                FillRect(hdc, &ctr, g_hBackgroundBrush.get()); 
                DrawText(hdc, L"Switch To Next Image From List:", -1, &ctr, DT_LEFT | DT_TOP);
                RECT klr3 = {8, 134, 282, 148}; 
                FillRect(hdc, &klr3, g_hBackgroundBrush.get()); 
                DrawText(hdc, L"Alt +", -1, &klr3, DT_LEFT | DT_TOP);
                RECT qtr = {8, 169, 280, 184}; 
                FillRect(hdc, &qtr, g_hBackgroundBrush.get()); 
                DrawText(hdc, L"Quit:", -1, &qtr, DT_LEFT | DT_TOP);
                RECT klr4 = {8, 189, 280, 202}; 
                FillRect(hdc, &klr4, g_hBackgroundBrush.get()); 
                DrawText(hdc, L"Alt +", -1, &klr4, DT_LEFT | DT_TOP);
                RECT f1 = {8, 215, 280, 230}; 
                FillRect(hdc, &f1, g_hBackgroundBrush.get()); 
                DrawText(hdc, L"Allowed input: 0-9 and A-Z", -1, &f1, DT_LEFT | DT_TOP);
                RECT f2 = {8, 230, 280, 245}; 
                FillRect(hdc, &f2, g_hBackgroundBrush.get()); 
                DrawText(hdc, L"Null input disables hotkey", -1, &f2, DT_LEFT | DT_TOP);
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
            else
                InvalidateRect(hwnd, NULL, TRUE); 
            return 0;
        case WM_GETMINMAXINFO: {
            MINMAXINFO* mmi = (MINMAXINFO*)lParam; 
            mmi->ptMinTrackSize.x = 300; 
            mmi->ptMinTrackSize.y = 300; 
            mmi->ptMaxTrackSize.x = 300; 
            mmi->ptMaxTrackSize.y = 300; 
            return 0; 
        }
        case WM_SHOW_AND_MESSAGE: {
            std::wstring msgText = L"The program is already running. The settings window has been restored";
            if (IsIconic(hwnd)) 
                ShowWindow(hwnd, SW_RESTORE);
            else if (!IsWindowVisible(hwnd)) 
                ShowWindow(hwnd, SW_SHOWNA);
            if (IsMessageBoxWithTextOpen(msgText)) {
                HWND hMsgBox = FindWindow(L"OwnerDrawMessageBox", NULL); 
                if (hMsgBox) 
                    ForceForegroundWindow(hMsgBox); 
                }
            else {
                ForceForegroundWindow(hwnd); 
                ShowOwnerDrawMessageBox(hwnd, msgText); 
            }
            return 0;
        }
        case WM_LBUTTONDOWN: {
            POINT pt = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            RECT sr; 
            GetWindowRect(g_hSizeSlider, &sr); 
            MapWindowPoints(HWND_DESKTOP, hwnd, (LPPOINT)&sr, 2);
            bool onSlider = PtInRect(&sr, pt) && IsWindowVisible(g_hSizeSlider);
            if (onSlider) {
                SetFocus(hwnd); 
                dragging = true; 
                SetCapture(hwnd);
                int margin = 5; 
                int trackStart = sr.left + margin; 
                int trackEnd = sr.right - margin; 
                int trackWidth = trackEnd - trackStart;
                int clickX = pt.x; 
                if (clickX < trackStart) 
                    clickX = trackStart; 
                if (clickX > trackEnd) 
                    clickX = trackEnd;
                int newValue = ((clickX - trackStart) * 200) / trackWidth;
                if (newValue < 0) 
                    newValue = 0; 
                if (newValue > 200) 
                    newValue = 200;
                float newScale = newValue / 100.0f;
                if (fabs(g_imageScale.load() - newScale) > 0.001f) {
                    g_imageScale.store(newScale, std::memory_order_relaxed);
                    UpdateScaleControls(); 
                    ApplyScaleChange(); 
                    g_settingsChanged = true; 
                }
                else {
                    RenderOverlay(); 
                }
                return 0;
            }
            HWND hChild = ChildWindowFromPoint(hwnd, pt);
            if (hChild != NULL && hChild != hwnd) {
                UINT id = GetDlgCtrlID(hChild); 
                if (id == ID_OFFSET_X_DISPLAY || id == ID_OFFSET_Y_DISPLAY || id == ID_SIZE_DISPLAY || id == ID_HOTKEY_TOGGLE_DISPLAY || id == ID_HOTKEY_SETTINGS_DISPLAY || id == ID_HOTKEY_CYCLE_DISPLAY || id == ID_HOTKEY_QUIT_DISPLAY) {}
                else SetFocus(hwnd); 
            }
            else SetFocus(hwnd);
            return DefWindowProc(hwnd, uMsg, wParam, lParam);
        }
        case WM_MOUSEMOVE:
            if (dragging && (wParam & MK_LBUTTON)) {
                POINT pt = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
                RECT sr; 
                GetWindowRect(g_hSizeSlider, &sr); 
                MapWindowPoints(HWND_DESKTOP, hwnd, (LPPOINT)&sr, 2);
                int margin = 5; 
                int trackStart = sr.left + margin; 
                int trackEnd = sr.right - margin; 
                int trackWidth = trackEnd - trackStart;
                int clickX = pt.x; 
                if (clickX < trackStart) 
                    clickX = trackStart; 
                if (clickX > trackEnd) 
                    clickX = trackEnd;
                int newValue = ((clickX - trackStart) * 200) / trackWidth;
                if (newValue < 0) 
                    newValue = 0; 
                if (newValue > 200) 
                    newValue = 200;
                float newScale = newValue / 100.0f;
                if (fabs(g_imageScale.load() - newScale) > 0.001f) {
                    g_imageScale.store(newScale, std::memory_order_relaxed); 
                    UpdateScaleControls(); 
                    ApplyScaleChange(); }
            }
            break;
        case WM_LBUTTONUP:
            if (dragging) {
                dragging = false; 
                ReleaseCapture(); 
                ApplyScaleChange(); 
                if (g_currentImageIndex != -1) 
                    SaveCurrentImageSettings(); 
                g_settingsChanged = true; 
        }
            break;
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
        if (hMutex) 
            CloseHandle(hMutex);
        HWND hExistingWnd = FindWindowW(L"Custom Desktop Overlay", NULL);
        if (hExistingWnd) 
            SendMessage(hExistingWnd, WM_SHOW_AND_MESSAGE, 0, 0);
        return 0;
    }
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    GdiplusStartupInput gdiplusStartupInput;
    GdiplusStartup(&g_gdiplusToken, &gdiplusStartupInput, NULL);
    g_hMainIcon = LoadIcon(hInstance, MAKEINTRESOURCE(IDI_MYICON));
    if (!g_hMainIcon) 
        g_hMainIcon = LoadIcon(NULL, IDI_APPLICATION);
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
    if (!RegisterClass(&wc)) 
        return 0;
    g_hwnd = CreateWindowEx(0, CLASS_NAME, L"Custom Desktop Overlay", WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, CW_USEDEFAULT, CW_USEDEFAULT, 300, 300, NULL, NULL, hInstance, NULL);
    if (!g_hwnd) 
        return 0;
    ShowWindow(g_hwnd, SW_HIDE);
    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg); 
        DispatchMessage(&msg); 
    }
    StopRenderThread();
    SafeGdiplusShutdown();
    CoUninitialize();
    if (hMutex) {
        ReleaseMutex(hMutex); 
        CloseHandle(hMutex); 
    }
    return 0;
}