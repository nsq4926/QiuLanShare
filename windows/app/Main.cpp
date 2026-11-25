// P3 exe 客户端主程序：WinMain + 托盘 + 主窗口。
// 核心服务装配见 Core.cpp，界面承载见 WebViewController.cpp。
#include "App.h"
#include "../src/TransferHub.h"
#include "../src/Common.h"
#include "../src/HostBridge.h"

#include <shellapi.h>
#include <windowsx.h>   // GET_X_LPARAM / GET_Y_LPARAM
#include <objbase.h>   // CoInitializeEx / CoUninitialize（LEAN_AND_MEAN 下 windows.h 不含）
#include <string>
#include <cstring>     // std::memcpy（剪贴板复制）
#include <vector>

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "ole32.lib")

namespace
{
    const wchar_t* kWindowClass = L"LanShareAppWindow";
    const wchar_t* kWindowTitle = L"秋秋快传 (QiuLanShare)";
    const UINT kTrayMsg = WM_TRAYICON;
    const UINT kMenuOpen = 1;
    const UINT kMenuCopy = 2;
    const UINT kMenuFolder = 3;
    const UINT kMenuAutostart = 5;   // 勾选式：开机自启
    const UINT kMenuExit = 4;

    const wchar_t* kRunKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
    const wchar_t* kRunName = L"LanShare";

    // ---- DPI ----
    // 本机 150% 缩放（dpi=144）。owner-draw 菜单**不会**自动按 DPI 缩放：
    // WM_MEASUREITEM 的尺寸和 DrawText 的字体都要自己换算，否则菜单字tiny、
    // 行距偏挤（用户实测）。统一走这里。
    UINT g_dpi = 96;

    int Scale(int v) { return ::MulDiv(v, static_cast<int>(g_dpi), 96); }

    // 菜单字体用"点"更稳（负高度=字符高度），CreateFont 的 height 是逻辑单位
    int MenuFontHeight(int pt)
    {
        // pt → 像素：1pt = 1/72 inch，1 inch = dpi 像素
        return -::MulDiv(pt, static_cast<int>(g_dpi), 72);
    }

    // 取系统 DPI（默认 96）。owner-draw 菜单和自绘 toast 都要按它换算尺寸 ——
    // 这类自绘内容**不会**被系统自动缩放（150% 屏下菜单字小、行距挤）。
    UINT QueryDpi()
    {
        HMODULE user32 = ::LoadLibraryW(L"user32.dll");
        if (!user32) return 96;
        typedef UINT (WINAPI *FnSys)(void);
        auto fnSys = reinterpret_cast<FnSys>(::GetProcAddress(user32, "GetDpiForSystem"));
        UINT dpi = 96;
        if (fnSys) dpi = fnSys();
        if (dpi < 96) dpi = 96;
        ::FreeLibrary(user32);
        return dpi;
    }

    HINSTANCE   g_hInst = nullptr;
    HWND        g_hwnd = nullptr;
    NOTIFYICONDATAW g_nid{};
    bool        g_trayReady = false;
    unsigned short g_port = 0;

    // 应用图标资源 ID，**必须与 app\LanShare.rc 里的 #define 一致**。
    // rc 不 #define 数值时资源会存成"名字型"（name = 字符串 IDI_LANSHARE_ICON），
    // 代码按数值找永远找不到 → 静默退回 IDI_APPLICATION（用户实测：换了图不生效）。
    const int kAppIconId = 101;

    // 图标分三种尺寸用途，各自加载一次：
    //   large  任务栏 / Alt+Tab（SM_CXICON）
    //   small  标题栏 / 任务栏小图（SM_CXSMICON）
    //   tray   托盘通知区（SM_CXSMICON，但按 DPI 放大后的实际尺寸）
    // 传 0,0 + LR_DEFAULTSIZE 会固定拿 32x32，托盘再缩到 16/24 会发虚。
    HICON       g_iconLarge = nullptr;
    HICON       g_iconSmall = nullptr;
    HICON       g_iconTray = nullptr;
    std::wstring g_lastPreview;

    // 核心工作线程 → UI 线程 的通知载荷。
    // convId 用来在点击气泡后定位到那条消息所在的会话。
    struct NotifyPayload
    {
        std::wstring preview;
        std::string convId;
    };

    void ShowError(const wchar_t* msg)
    {
        ::MessageBoxW(nullptr, msg, kWindowTitle, MB_OK | MB_ICONERROR);
    }

    // 应用层日志（写到与核心同一个 lanshare-app.log）
    void AppLog(const std::string& line)
    {
        const std::wstring path = tml::JoinPath(tml::GetModuleDir(), L"LanShare\\lanshare-app.log");
        tml::AppendLogLine(path, line);
    }

    std::wstring UiUrl()
    {
        return L"http://127.0.0.1:" + std::to_wstring(g_port) + L"/";
    }

    void CopyText(const std::wstring& text)
    {
        if (!::OpenClipboard(nullptr)) return;
        ::EmptyClipboard();
        const size_t bytes = (text.size() + 1) * sizeof(wchar_t);
        HGLOBAL h = ::GlobalAlloc(GMEM_MOVEABLE, bytes);
        if (h)
        {
            void* dst = ::GlobalLock(h);
            if (dst)
            {
                std::memcpy(dst, text.c_str(), bytes);
                ::GlobalUnlock(h);
                ::SetClipboardData(CF_UNICODETEXT, h);
            }
        }
        ::CloseClipboard();
    }

    std::wstring LanAddress()
    {
        return L"http://" + tml::Utf8ToWide(TransferHub::Instance().LocalIp())
             + L":" + std::to_wstring(g_port) + L"/";
    }

    // ---- 开机自启（HKCU Run，绿色版不写系统目录） ----
    // 用当前 exe 的完整路径加引号，路径带空格也不会被截断。
    bool AutostartEnabled(bool& enabled)
    {
        HKEY hk = nullptr;
        if (::RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_READ, &hk) != ERROR_SUCCESS)
        {
            enabled = false;
            return true;   // 键不存在 = 没开自启，这是正常状态
        }
        wchar_t buf[1024]{};
        DWORD len = sizeof(buf);
        const LSTATUS ret = ::RegQueryValueExW(hk, kRunName, nullptr, nullptr,
                                               reinterpret_cast<BYTE*>(buf), &len);
        ::RegCloseKey(hk);
        if (ret == ERROR_FILE_NOT_FOUND)
        {
            enabled = false;
            return true;
        }
        if (ret != ERROR_SUCCESS)
        {
            enabled = false;
            return false;
        }
        // 只认指向**当前 exe** 的值：别的用户装过同名项不能算我们开着
        wchar_t exe[MAX_PATH]{};
        const DWORD n = ::GetModuleFileNameW(nullptr, exe, MAX_PATH);
        if (n == 0 || n >= MAX_PATH)
        {
            enabled = false;
            return false;
        }
        const std::wstring target = std::wstring(L"\"") + exe + L"\"";
        enabled = (std::wstring(buf) == target);
        return true;
    }

    bool AutostartSet(bool enable)
    {
        HKEY hk = nullptr;
        if (::RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_SET_VALUE, &hk) != ERROR_SUCCESS)
        {
            if (!enable) return true;   // 键都不存在还想关 = 已经是关的
            return false;
        }
        bool ok = true;
        if (enable)
        {
            wchar_t exe[MAX_PATH]{};
            const DWORD n = ::GetModuleFileNameW(nullptr, exe, MAX_PATH);
            if (n == 0 || n >= MAX_PATH) ok = false;
            else
            {
                const std::wstring cmd = std::wstring(L"\"") + exe + L"\"";
                ok = ::RegSetValueExW(hk, kRunName, 0, REG_SZ,
                                      reinterpret_cast<const BYTE*>(cmd.c_str()),
                                      static_cast<DWORD>((cmd.size() + 1) * sizeof(wchar_t))) == ERROR_SUCCESS;
            }
        }
        else
        {
            const LSTATUS ret = ::RegDeleteValueW(hk, kRunName);
            ok = (ret == ERROR_SUCCESS || ret == ERROR_FILE_NOT_FOUND);
        }
        ::RegCloseKey(hk);
        return ok;
    }

    // ---- 托盘菜单（系统 HMENU + owner-draw）----
    // **交互全部交给 TrackPopupMenu，我们只负责画**。
    //
    // 上一版自建 WS_POPUP 窗口 + SetCapture + 自己跑模态 GetMessageW 循环，三个毛病：
    //   ① 点菜单外部时鼠标被 capture 抓走，MenuHitTest 返回 -1 却没有对应处理分支
    //      → 菜单关不掉（用户反馈"还关不掉"）；
    //   ② 模态消息循环阻塞 UI 线程，WebView2 也在同一线程上 → 整个界面卡死；
    //   ③ WS_EX_NOACTIVATE 让窗口永不激活 → Esc 键消息进不来。
    // TrackPopupMenu 免费提供：点外部关闭、Esc、上下键、屏幕边缘自动翻转、系统阴影。
    //
    // 之前的乱码 bug（dwItemData 塞 c_str() 裸指针、vector 扩容后悬空）同样根除：
    // dwItemData 只存**数组下标**，向量怎么搬家都不影响。
    struct MenuItem
    {
        UINT id = 0;
        std::wstring label;
        const wchar_t* glyph = nullptr;
        bool checkable = false;
        bool separator = false;
    };

    std::vector<MenuItem> g_menuItems;

    struct MenuPalette
    {
        COLORREF bg, border, text, muted, accent, hover, sep;
    };

    // 网页用 prefers-color-scheme 跟随系统 → 这里读同一个注册表项，
    // 保证菜单与页面永远同一套配色（上一版硬编码暗色，系统亮色时菜单是黑的、
    // 网页是白的 —— 用户说的"风格不统一"就是这个）。
    bool SystemUsesDarkTheme()
    {
        HKEY hk = nullptr;
        if (::RegOpenKeyExW(HKEY_CURRENT_USER,
                            L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                            0, KEY_QUERY_VALUE, &hk) != ERROR_SUCCESS)
            return false;
        DWORD v = 1, sz = sizeof(v), type = 0;
        const bool ok = (::RegQueryValueExW(hk, L"AppsUseLightTheme", nullptr, &type,
                                            reinterpret_cast<LPBYTE>(&v), &sz) == ERROR_SUCCESS);
        ::RegCloseKey(hk);
        return ok && v == 0;   // 0 = 暗色
    }

    // 与网页 CSS 变量一一对应：--card / --text / --muted / --accent / --line
    MenuPalette MenuColors()
    {
        MenuPalette p{};
        if (SystemUsesDarkTheme())
        {
            p.bg     = RGB(21, 27, 39);    // #151b27 --card
            p.border = RGB(52, 60, 74);    // rgba(255,255,255,.10) 叠在 card 上
            p.text   = RGB(233, 238, 248); // #e9eef8 --text
            p.muted  = RGB(141, 151, 171); // #8d97ab --muted
            p.accent = RGB(76, 125, 255);  // #4c7dff --accent
            p.hover  = RGB(33, 45, 70);    // accent 14% 叠在 card 上
            p.sep    = RGB(39, 47, 62);    // rgba(255,255,255,.06)
        }
        else
        {
            p.bg     = RGB(255, 255, 255); // #ffffff
            p.border = RGB(219, 225, 236);// #dbe1ec
            p.text   = RGB(26, 36, 52);    // #1a2434
            p.muted  = RGB(123, 135, 155);// #7b879b
            p.accent = RGB(63, 111, 224);  // #3f6fe0
            p.hover  = RGB(238, 243, 255); // accent 12% 叠在白上
            p.sep    = RGB(233, 237, 244);// #e9edf4
        }
        return p;
    }

    // 菜单排版尺寸（逻辑像素，绘制时按 DPI 换算）
    const int kMenuWidth = 208;
    const int kMenuItemH = 38;
    const int kMenuSepH  = 9;

    const wchar_t* MenuGlyph(UINT id)
    {
        // Segoe MDL2 Assets 字形，无需图片资源
        switch (id)
        {
        case kMenuOpen:      return L"\uE8E9";  // 窗口/打开
        case kMenuCopy:      return L"\uE8C8";  // 复制
        case kMenuFolder:    return L"\uE838";  // 文件夹
        case kMenuAutostart: return L"\uE7B5";  // 电源
        case kMenuExit:      return L"\uE711";  // 关闭
        default:             return L"";
        }
    }

    // WM_MEASUREITEM：owner-draw 菜单**不会**被系统自动按 DPI 缩放，宽高自己算。
    void MenuMeasure(MEASUREITEMSTRUCT* mis)
    {
        const size_t idx = static_cast<size_t>(mis->itemData);
        const bool sep = (idx < g_menuItems.size()) && g_menuItems[idx].separator;
        mis->itemWidth  = Scale(kMenuWidth);
        mis->itemHeight = sep ? Scale(kMenuSepH) : Scale(kMenuItemH);
    }

    // WM_DRAWITEM：dwItemData 是数组下标（**不是指针**）
    void MenuDraw(const DRAWITEMSTRUCT* di)
    {
        const size_t idx = static_cast<size_t>(di->itemData);
        if (idx >= g_menuItems.size()) return;
        const MenuItem& it = g_menuItems[idx];
        const MenuPalette pal = MenuColors();
        HDC dc = di->hDC;
        RECT rc = di->rcItem;

        // 底色（owner-draw 项系统不会帮我们填菜单背景）
        HBRUSH bg = ::CreateSolidBrush(pal.bg);
        ::FillRect(dc, &rc, bg);
        ::DeleteObject(bg);

        if (it.separator)
        {
            RECT line{ rc.left + Scale(12), (rc.top + rc.bottom) / 2,
                       rc.right - Scale(12), (rc.top + rc.bottom) / 2 + 1 };
            HBRUSH sb = ::CreateSolidBrush(pal.sep);
            ::FillRect(dc, &line, sb);
            ::DeleteObject(sb);
            return;
        }

        const bool hot = (di->itemState & ODS_SELECTED) != 0;
        if (hot)
        {
            // 圆角高亮块（网页的 hover 也是圆角）
            RECT hr{ rc.left + Scale(5), rc.top + Scale(2),
                     rc.right - Scale(5), rc.bottom - Scale(2) };
            HRGN hrgn = ::CreateRoundRectRgn(hr.left, hr.top, hr.right + 1, hr.bottom + 1,
                                             Scale(7) * 2, Scale(7) * 2);
            HBRUSH hb = ::CreateSolidBrush(pal.hover);
            ::FillRgn(dc, hrgn, hb);
            ::DeleteObject(hb);
            ::DeleteObject(hrgn);
        }

        ::SetBkMode(dc, TRANSPARENT);

        // 图标（MDL2 字形字体）
        HFONT glyphFont = ::CreateFontW(MenuFontHeight(11), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                        DEFAULT_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
                                        L"Segoe MDL2 Assets");
        HFONT oldFont = reinterpret_cast<HFONT>(::SelectObject(dc, glyphFont));
        ::SetTextColor(dc, hot ? pal.accent : pal.muted);
        RECT iconRc{ rc.left + Scale(12), rc.top, rc.left + Scale(38), rc.bottom };
        ::DrawTextW(dc, it.glyph ? it.glyph : L"", -1, &iconRc,
                    DT_SINGLELINE | DT_VCENTER | DT_CENTER);
        ::SelectObject(dc, oldFont);
        ::DeleteObject(glyphFont);

        // 文本
        HFONT textFont = ::CreateFontW(MenuFontHeight(10), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                       DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                       CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
                                       L"Microsoft YaHei UI");
        oldFont = reinterpret_cast<HFONT>(::SelectObject(dc, textFont));
        ::SetTextColor(dc, pal.text);
        RECT textRc{ rc.left + Scale(46), rc.top, rc.right - Scale(42), rc.bottom };
        ::DrawTextW(dc, it.label.c_str(), -1, &textRc, DT_SINGLELINE | DT_VCENTER | DT_LEFT);
        ::SelectObject(dc, oldFont);
        ::DeleteObject(textFont);

        // 勾选标记（owner-draw 不会画 MFS_CHECKED，自己画 ✔）
        if (it.checkable)
        {
            bool on = false;
            if (AutostartEnabled(on) && on)
            {
                HFONT ckFont = ::CreateFontW(MenuFontHeight(10), 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                                             DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                             DEFAULT_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
                                             L"Segoe UI Symbol");
                HFONT old = reinterpret_cast<HFONT>(::SelectObject(dc, ckFont));
                ::SetTextColor(dc, pal.accent);
                RECT ckRc{ rc.right - Scale(38), rc.top, rc.right - Scale(14), rc.bottom };
                ::DrawTextW(dc, L"\u2714", -1, &ckRc, DT_SINGLELINE | DT_VCENTER | DT_RIGHT);
                ::SelectObject(dc, old);
                ::DeleteObject(ckFont);
            }
        }
    }

    // ---- 菜单前台代理窗口 ----
    // TrackPopupMenu 只在它的 owner 是**前台窗口**时才会在"点菜单外部"时消失。
    // 主窗口关掉后是隐藏的（收进托盘），对隐藏窗口 SetForegroundWindow 无效
    // → 菜单就关不掉（用户实测）。这里准备一个 1×1 全透明、不进任务栏的
    // 常驻代理窗口专门当前台，菜单的 MEASUREITEM / DRAWITEM 也发到它这里。
    const wchar_t* kMenuOwnerClass = L"LanShareMenuOwner";
    HWND g_menuOwner = nullptr;

    LRESULT CALLBACK MenuOwnerProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
    {
        switch (msg)
        {
        case WM_MEASUREITEM:
            if (MEASUREITEMSTRUCT* mis = reinterpret_cast<MEASUREITEMSTRUCT*>(lParam))
                MenuMeasure(mis);
            return TRUE;
        case WM_DRAWITEM:
            if (DRAWITEMSTRUCT* dis = reinterpret_cast<DRAWITEMSTRUCT*>(lParam))
                MenuDraw(dis);
            return TRUE;
        }
        return ::DefWindowProcW(hwnd, msg, wParam, lParam);
    }

    bool EnsureMenuOwner(HINSTANCE hInst)
    {
        if (g_menuOwner && ::IsWindow(g_menuOwner)) return true;

        static bool clsDone = false;
        if (!clsDone)
        {
            WNDCLASSEXW wc{};
            wc.cbSize = sizeof(wc);
            wc.lpfnWndProc = MenuOwnerProc;
            wc.hInstance = hInst;
            wc.lpszClassName = kMenuOwnerClass;
            wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
            if (!::RegisterClassExW(&wc) && ::GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
                return false;
            clsDone = true;
        }

        // 屏幕外 1×1、分层全透明、不进任务栏、不抢焦点：肉眼不可见但能成为前台。
        g_menuOwner = ::CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_LAYERED,
                                        kMenuOwnerClass, L"", WS_POPUP,
                                        -32000, -32000, 1, 1,
                                        nullptr, nullptr, hInst, nullptr);
        if (!g_menuOwner) return false;
        ::SetLayeredWindowAttributes(g_menuOwner, 0, 0, LWA_ALPHA);
        ::ShowWindow(g_menuOwner, SW_SHOWNOACTIVATE);
        return true;
    }

    void OpenTrayMenu(HWND hwnd, POINT pt)
    {
        g_menuItems.clear();
        auto add = [&](UINT id, const wchar_t* text, bool separator = false, bool checkable = false) {
            MenuItem it;
            it.id = id;
            it.label = text ? text : L"";
            it.glyph = separator ? nullptr : MenuGlyph(id);
            it.separator = separator;
            it.checkable = checkable;
            g_menuItems.push_back(it);
        };

        add(kMenuOpen,      L"打开传输界面");
        add(kMenuCopy,      L"复制本机地址");
        add(kMenuFolder,    L"打开保存目录");
        add(0, nullptr, true);
        add(kMenuAutostart, L"开机自启", false, true);
        add(0, nullptr, true);
        add(kMenuExit,      L"退出");

        HMENU menu = ::CreatePopupMenu();
        if (!menu) return;

        // 让系统画的那圈内边距也是卡片底色（否则暗色菜单外面套一圈浅灰）
        const MenuPalette pal = MenuColors();
        HBRUSH menuBg = ::CreateSolidBrush(pal.bg);
        MENUINFO minfo{};
        minfo.cbSize = sizeof(minfo);
        minfo.fMask = MIM_BACKGROUND;
        minfo.hbrBack = menuBg;
        ::SetMenuInfo(menu, &minfo);

        for (size_t i = 0; i < g_menuItems.size(); ++i)
        {
            const MenuItem& it = g_menuItems[i];
            MENUITEMINFOW mii{};
            mii.cbSize = sizeof(mii);
            mii.fMask  = MIIM_ID | MIIM_FTYPE | MIIM_DATA | MIIM_STATE;
            mii.wID    = static_cast<UINT>(i + 1);      // 唯一非零；返回时用它转回下标
            mii.fType  = MFT_OWNERDRAW;
            // 分隔线不能同时挂 MFT_SEPARATOR：那样系统不派发 WM_DRAWITEM。
            // 改用一个 disabled 的普通项，靠 MEASUREITEM 给很矮的高度。
            if (it.separator) mii.fState = MFS_DISABLED;
            mii.dwItemData = static_cast<ULONG_PTR>(i);  // **下标**，不是指针
            ::InsertMenuItemW(menu, static_cast<UINT>(i), TRUE, &mii);
        }

        // **TrackPopupMenu 要求它的 owner 是前台窗口**，否则点菜单外部时菜单
        // 不会消失（用户反馈的"关不掉"）。而主窗口在"关窗口 = 收进托盘"之后
        // 是隐藏的，**对隐藏窗口 SetForegroundWindow 是无效的**。
        // 所以菜单挂在一个 1×1 全透明的代理窗口上，由它当前台。
        if (!EnsureMenuOwner(g_hInst))
        {
            ::DestroyMenu(menu);
            if (menuBg) ::DeleteObject(menuBg);
            AppLog("menu owner window create FAILED");
            return;
        }
        ::SetForegroundWindow(g_menuOwner);

        const UINT cmd = ::TrackPopupMenu(menu,
                                          TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY,
                                          pt.x, pt.y, 0, g_menuOwner, nullptr);

        ::DestroyMenu(menu);
        if (menuBg) ::DeleteObject(menuBg);
        ::PostMessageW(g_menuOwner, WM_NULL, 0, 0);   // 让菜单可靠消失

        if (cmd == 0 || cmd > g_menuItems.size()) return;
        switch (g_menuItems[cmd - 1].id)
        {
        case kMenuOpen:   AppShowMainWindow(); break;
        case kMenuCopy:   CopyText(LanAddress()); break;
        case kMenuFolder:
        {
            const std::wstring dir = TransferHub::Instance().ShareDir();
            ::ShellExecuteW(nullptr, L"open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            break;
        }
        case kMenuAutostart:
        {
            bool enabled = false;
            AutostartEnabled(enabled);
            AutostartSet(!enabled);
            AppLog(std::string("autostart -> ") + (!enabled ? "on" : "off"));
            break;
        }
        case kMenuExit:   AppQuit(); break;
        default: break;
        }
    }


    // ---- 系统通知气泡（Shell NIF_INFO） ----
    // 用系统的通知气泡（Windows 通知中心里那条），不是自绘窗口：
    // 用户要的是"系统弹窗"，样式跟系统一致，带消息预览。
    //
    // **点击气泡为什么没反应**：Win10+ 把 NIF_INFO 气泡接管成 Toast，
    // 点击时系统会去**启动本程序的新实例**（而不是通知已运行的进程），
    // 新实例撞上单实例锁就直接死掉 —— 用户看到的"点了没反应"。
    //
    // 修法（两段）：
    //   ① 主实例收到 WM_APP_ACTIVATE 时显示窗口并定位到那条会话；
    //   ② 新实例发现"已有实例在跑"时，**不报错退出**，而是把
    //      "把我激活"的请求转给主实例（AppActivateExistingInstance）。
    // 这样点气泡 → 系统拉起新实例 → 新实例转发 → 主实例弹窗并定位会话。
    //
    // 气泡本身的正文预览存在 g_nid.szInfo，点击时用它反查会话。
    std::string g_lastNotifyConv;

    void TrayShowBalloon(const std::wstring& title, const std::wstring& body)
    {
        if (!g_trayReady) return;
        NOTIFYICONDATAW nid = g_nid;
        nid.uFlags = NIF_INFO;
        nid.dwInfoFlags = NIIF_INFO | NIIF_RESPECT_QUIET_TIME;
        ::wcsncpy_s(nid.szInfoTitle, _countof(nid.szInfoTitle), title.c_str(), _TRUNCATE);
        ::wcsncpy_s(nid.szInfo, _countof(nid.szInfo), body.c_str(), _TRUNCATE);
        ::Shell_NotifyIconW(NIM_MODIFY, &nid);
    }

    LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
    {
        switch (msg)
        {
        case WM_TRAYICON:
        {
            // 两种回调协议都兼容（NIM_SETVERSION 成功后是 v4）：
            //   v4：wParam = 锚点坐标(LOWORD=x, HIWORD=y)，lParam 低位 = 事件码
            //   v0：wParam = 图标 ID，lParam 低位 = 鼠标消息
            // 所以事件码一律取 LOWORD(lParam)，鼠标消息和 NIN_* 一起判。
            const UINT evt = LOWORD(lParam);

            // v4 把锚点坐标放在 wParam 里，比 GetCursorPos 更准
            // （键盘唤起菜单时 GetCursorPos 是鼠标位置，不是托盘图标位置）。
            POINT anchor{ GET_X_LPARAM(static_cast<DWORD>(wParam)),
                          GET_Y_LPARAM(static_cast<DWORD>(wParam)) };
            if (anchor.x == 0 && anchor.y == 0) ::GetCursorPos(&anchor);

            switch (evt)
            {
            case WM_LBUTTONUP:        // v0 左键
            case NIN_SELECT:          // v4 左键（含单击）
            case NIN_KEYSELECT:       // v4 键盘回车
                AppShowMainWindow();
                break;

            case WM_RBUTTONUP:        // v0 右键
            case WM_CONTEXTMENU:      // v4 右键（也用于键盘菜单键）
                OpenTrayMenu(hwnd, anchor);
                break;

            case NIN_BALLOONUSERCLICK:
            {
                // **气泡被点击** —— 用户点击右下角系统通知。
                // 这才是"点通知打开主窗口"的正确入口：
                // 之前只处理了左右键，漏了这条分支，所以点了完全没反应。
                // （Win10+ 把它接管成 Toast，但点击仍会回调这条消息。）
                AppLog("balloon clicked, conv=" + g_lastNotifyConv);
                AppShowMainWindow();
                if (!g_lastNotifyConv.empty())
                    WebViewSelectConversation(g_lastNotifyConv);
                break;
            }

            case NIN_BALLOONHIDE:
            case NIN_BALLOONTIMEOUT:
                // 气泡被系统收起/超时：只记日志，不做动作
                AppLog("balloon dismissed");
                break;

            default: break;
            }
            return 0;
        }

        case WM_MEASUREITEM:
        {
            MEASUREITEMSTRUCT* mis = reinterpret_cast<MEASUREITEMSTRUCT*>(lParam);
            if (mis) MenuMeasure(mis);
            return TRUE;
        }

        case WM_DRAWITEM:
        {
            DRAWITEMSTRUCT* dis = reinterpret_cast<DRAWITEMSTRUCT*>(lParam);
            if (dis) MenuDraw(dis);
            return TRUE;
        }

        case WM_APP_NOTIFY:
        {
            // lParam 是 new 出来的 NotifyPayload（跨线程所有权移交给 UI 线程）
            NotifyPayload* p = reinterpret_cast<NotifyPayload*>(lParam);
            if (p)
            {
                TrayNotifyMessage(p->preview.c_str(), p->convId);
                delete p;
            }
            return 0;
        }

        case WM_APP_ACTIVATE:
        {
            // 第二实例转发过来的"把我激活"请求（系统气泡点击会走这条路径）。
            // 定位到哪个会话由**主实例自己**决定：它在弹通知时已经把
            // convId 记在 g_lastNotifyConv 里了（不跨进程传裸指针）。
            AppShowMainWindow();
            AppLog("activate requested, conv=" + g_lastNotifyConv);
            if (!g_lastNotifyConv.empty())
                WebViewSelectConversation(g_lastNotifyConv);
            return 0;
        }

        case WM_SIZE:
            WebViewResize();
            return 0;

        case WM_DPICHANGED:
        {
            // 窗口拖到不同缩放的显示器上：更新 DPI 基准并按系统建议调整窗口位置
            const UINT dpi = LOWORD(wParam);
            if (dpi >= 96) g_dpi = dpi;
            const RECT* suggested = reinterpret_cast<const RECT*>(lParam);
            if (suggested)
                ::SetWindowPos(hwnd, nullptr, suggested->left, suggested->top,
                               suggested->right - suggested->left, suggested->bottom - suggested->top,
                               SWP_NOZORDER | SWP_NOACTIVATE);
            WebViewResize();
            return 0;
        }

        case WM_CLOSE:
            // 关窗口 = 隐藏到托盘，不等于退出（服务继续跑，手机还能连）
            ::ShowWindow(hwnd, SW_HIDE);
            return 0;

        case WM_DESTROY:
            ::PostQuitMessage(0);
            return 0;

        default:
            return ::DefWindowProcW(hwnd, msg, wParam, lParam);
        }
    }
}

void AppInstallHostHooks()
{
    auto& h = tml::host::GetHooks();

    h.getAutostart = [](bool& enabled) -> bool {
        return AutostartEnabled(enabled);
    };
    h.setAutostart = [](bool enable) -> bool {
        return AutostartSet(enable);
    };
    // 核心工作线程回调 → 投递到 UI 线程（Shell_NotifyIcon 必须在 UI 线程调用）
    h.onIncoming = [](const tml::host::IncomingNotice& n) {
        if (!g_hwnd) return;
        NotifyPayload* payload = new NotifyPayload();
        std::wstring preview = tml::Utf8ToWide(n.preview);
        if (preview.size() > 80) preview = preview.substr(0, 80) + L"…";
        payload->preview = n.dev.empty()
            ? preview
            : tml::Utf8ToWide(n.dev) + L"：" + preview;
        payload->convId = n.convId;
        if (!::PostMessageW(g_hwnd, WM_APP_NOTIFY, 0, reinterpret_cast<LPARAM>(payload)))
            delete payload;
    };
}

HWND AppCreateMainWindow(HINSTANCE hInst)
{
    g_hInst = hInst;

    // DPI 基准要先确定：下面加载托盘图标时要用它算实际像素尺寸。
    g_dpi = QueryDpi();

    // ---- 应用图标（app\LanShare.rc 的 IDI_LANSHARE_ICON = 101）----
    // ico 内含 16/20/24/32/40/48/64/128/256 九档，按目标尺寸精确取，不缩放。
    // **不能加 LR_LOADFROMFILE**：那会把资源 ID 当成磁盘文件名去找，必然失败。
    const int cxIcon = ::GetSystemMetrics(SM_CXICON);
    const int cyIcon = ::GetSystemMetrics(SM_CYICON);
    const int cxSm   = ::GetSystemMetrics(SM_CXSMICON);
    const int cySm   = ::GetSystemMetrics(SM_CYSMICON);

    g_iconLarge = static_cast<HICON>(::LoadImageW(hInst, MAKEINTRESOURCEW(kAppIconId),
                                                  IMAGE_ICON, cxIcon, cyIcon, 0));
    g_iconSmall = static_cast<HICON>(::LoadImageW(hInst, MAKEINTRESOURCEW(kAppIconId),
                                                  IMAGE_ICON, cxSm, cySm, 0));
    // 托盘：直接用系统小图标尺寸即可。PerMonitorV2 进程里 SM_CXSMICON 已是
    // 按当前显示器 DPI 缩放后的值（150% 屏上是 24），正好对上通知区。
    g_iconTray  = static_cast<HICON>(::LoadImageW(hInst, MAKEINTRESOURCEW(kAppIconId),
                                                  IMAGE_ICON, cxSm, cySm, 0));

    // 加载失败必须响亮：本项目最大的 bug 形态就是静默失败。
    if (!g_iconLarge || !g_iconSmall || !g_iconTray)
    {
        const DWORD err = ::GetLastError();
        AppLog("icon load FAILED, id=" + std::to_string(kAppIconId) +
               " err=" + std::to_string(err) + " — 检查 LanShare.rc 是否 #define 了数值 ID");
        ShowError(L"加载应用图标失败，将使用系统默认图标。\n"
                  L"（换了图标不生效时先看 lanshare-app.log 里的 icon load FAILED）");
        if (!g_iconLarge) g_iconLarge = ::LoadIconW(nullptr, IDI_APPLICATION);
        if (!g_iconSmall) g_iconSmall = ::LoadIconW(nullptr, IDI_APPLICATION);
        if (!g_iconTray)  g_iconTray  = ::LoadIconW(nullptr, IDI_APPLICATION);
    }

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.lpszClassName = kWindowClass;
    wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    // 类图标：任务栏/Alt+Tab 在窗口没显式设图标时读这两个。
    wc.hIcon = g_iconLarge;
    wc.hIconSm = g_iconSmall;
    if (!::RegisterClassExW(&wc))
    {
        ShowError(L"注册窗口类失败。");
        return nullptr;
    }

    // 进程是 PerMonitorV2 感知时，CreateWindowEx 的尺寸参数是**物理像素**，
    // 不再是逻辑像素。所以这里按 DPI 放大，150% 屏上才有 1100x720 的实际大小
    // （否则窗口只有 733x480，看起来"分辨率不对"）。
    const int winW = Scale(1100);
    const int winH = Scale(760);

    g_hwnd = ::CreateWindowExW(0, kWindowClass, kWindowTitle,
                               WS_OVERLAPPEDWINDOW,
                               CW_USEDEFAULT, CW_USEDEFAULT, winW, winH,
                               nullptr, nullptr, hInst, nullptr);
    if (!g_hwnd)
    {
        ShowError(L"创建主窗口失败。");
        return nullptr;
    }

    // 类图标只对"注册类之后新建的窗口"生效，而且部分 shell 组件只看窗口自己的
    // 图标 → 显式 WM_SETICON 一遍，任务栏图标才稳定。
    ::SendMessageW(g_hwnd, WM_SETICON, ICON_BIG,   reinterpret_cast<LPARAM>(g_iconLarge));
    ::SendMessageW(g_hwnd, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(g_iconSmall));

    return g_hwnd;
}

void AppShowMainWindow()
{
    if (!g_hwnd) return;
    ::ShowWindow(g_hwnd, SW_SHOW);
    ::SetForegroundWindow(g_hwnd);
    WebViewResize();
}

bool AppActivateExistingInstance(const std::string& convId)
{
    // 找到已在运行的主实例，把"激活"请求转给它。
    //
    // 为什么要这条：**点击系统气泡时，Win10+ 会去启动本程序的新实例**
    // （而不是通知已运行的进程）。新实例如果只是报错退出，用户看到的
    // 就是"点了通知没反应"（用户实测）。改成转发 → 主实例弹窗并定位会话。
    //
    // 用窗口类名找：主窗口类名是固定的 kWindowClass，
    // 新旧实例跑的是同一个 exe，类名一致。
    // （TrafficMonitor 插件不建这个窗口，不会被误判成"已有实例"。）
    HWND target = ::FindWindowW(kWindowClass, nullptr);
    if (!target) return false;

    // **不跨进程传裸指针**：SendMessage 的 lParam 在对端处理期间
    // 指向发送方进程的内存，发送方一退出就是悬空指针。
    // 改成只发信号，具体定位哪个会话由主实例自己决定 ——
    // 主实例在收到通知时已经把 convId 记在 g_lastNotifyConv 里了。
    ::SendMessageW(target, WM_APP_ACTIVATE, 0, 0);
    return true;
}

void AppQuit()
{
    WebViewShutdown();
    TrayDestroy();
    AppStopCore();
    if (g_hwnd) ::DestroyWindow(g_hwnd);
}

bool TrayCreate(HWND hwnd)
{
    g_nid = {};
    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = hwnd;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    g_nid.uCallbackMessage = kTrayMsg;
    g_nid.hIcon = g_iconTray ? g_iconTray : ::LoadIconW(nullptr, IDI_APPLICATION);

    const std::wstring tip = std::wstring(kWindowTitle) + L"  " + LanAddress();
    ::wcsncpy_s(g_nid.szTip, _countof(g_nid.szTip), tip.c_str(), _TRUNCATE);

    g_trayReady = ::Shell_NotifyIconW(NIM_ADD, &g_nid) != FALSE;

    // 声明用 v4 回调协议：气泡被点击时会发 NIN_BALLOONUSERCLICK，
    // 且 wParam 带锚点坐标（下面 OpenTrayMenu 要用它定位菜单）。
    if (g_trayReady)
    {
        NOTIFYICONDATAW ver = g_nid;
        ver.uVersion = NOTIFYICON_VERSION_4;
        ::Shell_NotifyIconW(NIM_SETVERSION, &ver);
    }

    if (!g_trayReady)
        AppLog("tray add FAILED, err=" + std::to_string(::GetLastError()));
    return g_trayReady;
}

void TrayDestroy()
{
    if (g_trayReady)
    {
        ::Shell_NotifyIconW(NIM_DELETE, &g_nid);
        g_trayReady = false;
    }
}

void TrayUpdateTooltip(const wchar_t* text)
{
    if (!g_trayReady) return;
    ::wcsncpy_s(g_nid.szTip, _countof(g_nid.szTip), text, _TRUNCATE);
    g_nid.uFlags = NIF_TIP;
    ::Shell_NotifyIconW(NIM_MODIFY, &g_nid);
}

    void TrayNotifyMessage(const wchar_t* preview, const std::string& convId)
    {
        if (!g_trayReady || !g_hwnd) return;
        g_lastPreview = preview ? preview : L"";
        g_lastNotifyConv = convId;

        // **系统气泡**（Windows 通知中心里那条），带消息预览。
        // 不用自绘窗口：用户要的是系统原生弹窗，样式跟系统一致。
        //
        // 点击气泡后的链路（Win10+ 系统接管成 Toast）：
        //   系统拉起本程序新实例 → 新实例发现已有实例在跑 →
        //   AppActivateExistingInstance() 把"激活"请求转给主实例 →
        //   主实例显示窗口并定位到 g_lastNotifyConv 那个会话。
        TrayShowBalloon(L"收到新消息", g_lastPreview);
        AppLog("balloon shown: conv=" + convId + " preview=" + tml::WideToUtf8(g_lastPreview));
    }

void TrayStopFlash()
{
    // 已移除：用户取消托盘图标闪动功能
}

// 让 DPI 感知在.CreateWindow 之前生效。
// 主路径是 exe 内嵌的 manifest（app\LanShare.manifest）声明 PerMonitorV2 ——
// 系统以 manifest 为准，SetProcessDpiAwarenessContext 会返回 FALSE（无害）。
// 这里的动态调用只作兜底：manifest 缺失时仍能拿到 DPI 感知。
static void EnableDpiAwareness()
{
    HMODULE user32 = ::LoadLibraryW(L"user32.dll");
    if (!user32) return;
    typedef BOOL (WINAPI *FnCtx)(DPI_AWARENESS_CONTEXT);
    auto setCtx = reinterpret_cast<FnCtx>(::GetProcAddress(user32, "SetProcessDpiAwarenessContext"));
    if (setCtx)
    {
        // PER_MONITOR_AWARE_V2：Win10 1703+，多显示器缩放也正确
        setCtx(reinterpret_cast<DPI_AWARENESS_CONTEXT>(-4));
    }
    else
    {
        typedef BOOL (WINAPI *FnOld)();
        auto setOld = reinterpret_cast<FnOld>(::GetProcAddress(user32, "SetProcessDPIAware"));
        if (setOld) setOld();
    }
    ::FreeLibrary(user32);
}

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR, int nCmdShow)
{
    EnableDpiAwareness();

    // COM 必须在 WebView2 之前初始化为 STA
    ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);

    // 核心所有相对路径的根：exe 自己（不是宿主 DLL）
    tml::SetSelfModule(hInst);

    AppInstallHostHooks();

    // ---- 已有实例在跑？→ 把"激活"请求转给它，自己安静退出 ----
    // 这条路径专治"点了系统通知没反应"：
    // Win10+ 的系统气泡被点时，系统去**启动本程序的新实例**，
    // 新实例撞上单实例锁就死了 → 用户点了没反应。
    // 改成：新实例发现已有实例 → 转发"激活 + 定位到那条消息" → 退出。
    //
    // **必须在 AppStartCore 之前判**：取锁失败发生在 AppStartCore 内部，
    // 那时已经晚了一步（而且报错弹窗会盖在通知上）。
    if (AppActivateExistingInstance(std::string()))
    {
        ::CoUninitialize();
        return 0;
    }

    g_port = AppStartCore();
    if (g_port == 0)
    {
        ShowError(L"服务启动失败：可能已有 LanShare 实例在运行，或无可用端口。\n"
                  L"详见 LanShare\\lanshare-app.log。");
        ::CoUninitialize();
        return 1;
    }

    HWND hwnd = AppCreateMainWindow(hInst);
    if (!hwnd)
    {
        AppStopCore();
        ::CoUninitialize();
        return 1;
    }

    TrayCreate(hwnd);
    TrayUpdateTooltip((std::wstring(kWindowTitle) + L"\n" + LanAddress()).c_str());

    // 预创建菜单的前台代理窗口：首次右键不必现创建（少一次卡顿），
    // 创建失败也能立刻在日志里暴露，而不是等用户右键才发现菜单弹不出来。
    if (!EnsureMenuOwner(hInst))
        AppLog("menu owner window create FAILED");
    else
        AppLog("menu owner ready");

    const std::wstring url = UiUrl();
    WebViewInit(hwnd, url.c_str());

    // 默认显示窗口（托盘左键/菜单可再次唤出）
    ::ShowWindow(hwnd, nCmdShow);
    ::UpdateWindow(hwnd);

    MSG msg{};
    while (::GetMessageW(&msg, nullptr, 0, 0) > 0)
    {
        ::TranslateMessage(&msg);
        ::DispatchMessageW(&msg);
    }

    // 正常退出路径（WM_QUIT）
    WebViewShutdown();
    TrayDestroy();
    AppStopCore();

    ::CoUninitialize();
    return static_cast<int>(msg.wParam);
}
