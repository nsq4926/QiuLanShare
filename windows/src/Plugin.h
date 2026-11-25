#pragma once

#include "../include/PluginInterface.h"

#include <string>

class CLanSharePlugin;

// 显示项目 1：本机局域网 IP
class CItemIp : public IPluginItem
{
public:
    explicit CItemIp(CLanSharePlugin* owner) : m_owner(owner) {}

    const wchar_t* GetItemName() const override;
    const wchar_t* GetItemId() const override;
    const wchar_t* GetItemLableText() const override;
    const wchar_t* GetItemValueText() const override;
    const wchar_t* GetItemValueSampleText() const override;
    int OnMouseEvent(MouseEventType type, int x, int y, void* hWnd, int flag) override;

private:
    CLanSharePlugin* m_owner;
};

// 显示项目 2：未读消息数
class CItemUnread : public IPluginItem
{
public:
    explicit CItemUnread(CLanSharePlugin* owner) : m_owner(owner) {}

    const wchar_t* GetItemName() const override;
    const wchar_t* GetItemId() const override;
    const wchar_t* GetItemLableText() const override;
    const wchar_t* GetItemValueText() const override;
    const wchar_t* GetItemValueSampleText() const override;
    int OnMouseEvent(MouseEventType type, int x, int y, void* hWnd, int flag) override;

private:
    CLanSharePlugin* m_owner;
};

class CLanSharePlugin : public ITMPlugin
{
public:
    static CLanSharePlugin& Instance();

    // ---- ITMPlugin ----
    IPluginItem* GetItem(int index) override;
    void DataRequired() override;
    const wchar_t* GetInfo(PluginInfoIndex index) override;
    OptionReturn ShowOptionsDialog(void* hParent) override;
    const wchar_t* GetTooltipInfo() override;
    void OnInitialize(ITrafficMonitor* pApp) override;

    int GetCommandCount() override;
    const wchar_t* GetCommandName(int command_index) override;
    void OnPluginCommand(int command_index, void* hWnd, void* para) override;

    // ---- 供显示项目复用 ----
    void OpenWebUi(void* hWnd);
    void OpenShareFolder(void* hWnd);
    void CopyAddress(void* hWnd);
    const std::wstring& IpText() const { return m_ipText; }
    const std::wstring& UnreadText() const { return m_unreadText; }

private:
    CLanSharePlugin();

    void EnsureStarted();
    void RefreshTexts();

    CItemIp    m_itemIp;
    CItemUnread m_itemUnread;

    ITrafficMonitor* m_app = nullptr;

    std::wstring m_ipText = L"--";
    std::wstring m_unreadText = L"0";
    std::wstring m_tooltip;
    std::wstring m_infoName;
    std::wstring m_infoDesc;
    std::wstring m_infoVersion;
    std::wstring m_tipCache;

    bool  m_started = false;
    // 第二实例：取锁失败后只作客户端，不监听、不抢端口。
    // 一旦确定就不再改判（本进程生命周期内保持），避免和第一实例反复争夺。
    bool  m_clientOnly = false;
    std::wstring m_lockReason;   // 取锁结果/降级原因，显示在 tooltip 里排障用
    int   m_tick = 0;
    unsigned long long m_lastRetry = 0;
    unsigned long long m_lastOpen = 0;   // 打开界面的去抖时间戳
};
