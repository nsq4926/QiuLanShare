#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <string>

// 托盘与主窗口的私有消息
#define WM_TRAYICON      (WM_APP + 1)
#define WM_APP_NOTIFY    (WM_APP + 2)   // 核心工作线程 → UI 线程：入站消息通知
#define WM_APP_ACTIVATE  (WM_APP + 3)   // 第二实例 → 主实例：把我激活（系统弹窗点击走这条）

// ---- 生命周期 ----
// 启动核心服务（顺序与插件 EnsureStarted 一致），成功返回实际监听端口；0 = 失败
unsigned short AppStartCore();
// 关闭核心服务 + 释放单实例锁，供退出时使用
void AppStopCore();
// 注册 exe 独有的宿主能力（开机自启 / 入站通知）。AppStartCore 之前调用。
void AppInstallHostHooks();

// ---- 主窗口 ----
HWND  AppCreateMainWindow(HINSTANCE hInst);
void  AppShowMainWindow();     // 显示 + 激活 + 通知 WebView2 重新布局
void  AppQuit();               // 走干净退出流程
// 激活已有实例（第二实例用）：让主实例显示窗口并定位到指定会话。
// convId 为空 = 只显示窗口。成功返回 true。
bool  AppActivateExistingInstance(const std::string& convId);

// ---- WebView2 ----
// 初始化环境并异步创建 WebView2；url 用核心实际端口拼好传入
bool  WebViewInit(HWND host, const wchar_t* url);
void  WebViewResize();         // 按宿主 HWND 当前客户区重新布局（宿主在控制器里缓存）
void  WebViewShutdown();
// 让页面选中指定会话（通知点击后定位到那条消息）。convId 为空 = 什么都不做。
void  WebViewSelectConversation(const std::string& convId);

// ---- 托盘 ----
bool  TrayCreate(HWND hwnd);
void  TrayDestroy();
void  TrayUpdateTooltip(const wchar_t* text);
// 收到新消息：弹**系统气泡**（Shell 通知，带消息预览）。UI 线程调用。
// convId 用于点击气泡后定位会话。
void  TrayNotifyMessage(const wchar_t* preview, const std::string& convId);
void  TrayStopFlash();         // 用户回到窗口时停止闪动
