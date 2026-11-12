#include "app.h"
#include "common.h"
#include "net.h"
#include "server.h"

#include <windows.h>
#include <windowsx.h>
#include <dwmapi.h>
#include <gdiplus.h>
#include <shellapi.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

using namespace Gdiplus;
using namespace pp;

// ============================ 主题 ============================

namespace th {
const Color bg(255, 22, 23, 26);
const Color panel(255, 32, 34, 38);
const Color panelHi(255, 41, 44, 49);
const Color border(255, 52, 56, 63);
const Color text(255, 233, 235, 238);
const Color muted(255, 139, 145, 155);
const Color accent(255, 76, 141, 255);
const Color ok(255, 62, 212, 140);
const Color warn(255, 255, 189, 82);
const Color bad(255, 255, 108, 108);
} // namespace th

// ============================ 小工具 ============================

static std::wstring Widen(const std::string& s) {
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), NULL, 0);
    if (n <= 0) return std::wstring();
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}

static std::wstring Num(unsigned long long v) {
    std::wstring s = std::to_wstring(v);
    for (int i = (int)s.size() - 3; i > 0; i -= 3) s.insert((size_t)i, L",");
    return s;
}

static std::wstring ClockText(int seconds) {
    if (seconds < 0) seconds = 0;
    int m = seconds / 60;
    int sec = seconds % 60;
    std::wstring r;
    if (m < 10) r += L"0";
    r += std::to_wstring(m);
    r += L":";
    if (sec < 10) r += L"0";
    r += std::to_wstring(sec);
    return r;
}

static void FillBox(Graphics& g, const RectF& r, const Color& c) {
    SolidBrush b(c);
    g.FillRectangle(&b, r);
}

static void StrokeBox(Graphics& g, const RectF& r, const Color& c, float w) {
    Pen p(c, w);
    g.DrawRectangle(&p, r);
}

static void TextAt(Graphics& g, const std::wstring& s, Font* f, const RectF& r, const Color& c,
                   StringAlignment al = StringAlignmentNear) {
    StringFormat fmt;
    fmt.SetAlignment(al);
    fmt.SetLineAlignment(StringAlignmentCenter);
    fmt.SetFormatFlags(StringFormatFlagsNoWrap);
    fmt.SetTrimming(StringTrimmingEllipsisCharacter);
    SolidBrush b(c);
    g.DrawString(s.c_str(), -1, f, r, &fmt, &b);
}

// 文本宽度
static float TextWidth(Graphics& g, const std::wstring& s, Font* f) {
    RectF box;
    g.MeasureString(s.c_str(), -1, f, PointF(0, 0), &box);
    return box.Width;
}

// ============================ 状态 ============================

struct Gui {
    HWND hwnd;
    ProxyPool* pool;
    ProxyServer* server;
    Options* opt;

    std::vector<Proxy> snap;
    int scroll;
    int hover;
    int pressed;

    std::wstring toast;
    ULONGLONG toastUntil;
    ULONGLONG nextRefresh;
    ULONGLONG tickBase;

    float s;  // DPI 缩放

    Font *fTitle, *fSub, *fValue, *fBody, *fSmall, *fBtn, *fMono;
    bool darkTitle;
};

static Gui g_ui;

// ============================ 布局 ============================

struct Layout {
    RectF addr, btnCopy, stats, listCard, listHead, listBody, btnRefresh, chip;
    float s;
    float rowH;
    int visible;
};

static Layout MakeLayout(float w, float h, float s, int rowCount) {
    Layout L;
    L.s = s;
    L.rowH = 38.0f * s;
    L.visible = 0;
    float pad = 20.0f * s;
    float headerH = 86.0f * s;

    float cardH = 66.0f * s;
    L.addr = RectF(pad, headerH, w - pad * 2, cardH);
    L.btnCopy = RectF(L.addr.GetRight() - 92.0f * s, L.addr.Y + (cardH - 34.0f * s) / 2,
                      76.0f * s, 34.0f * s);

    L.stats = RectF(pad, L.addr.GetBottom() + 16.0f * s, w - pad * 2, 22.0f * s);

    float listTop = L.stats.GetBottom() + 14.0f * s;
    float listBottom = h - 76.0f * s;
    if (listBottom - listTop < 120.0f * s) listBottom = listTop + 120.0f * s;

    L.listCard = RectF(pad, listTop, w - pad * 2, listBottom - listTop);
    L.listHead = RectF(L.listCard.X + 1, L.listCard.Y + 1, L.listCard.Width - 2, 32.0f * s);
    L.listBody = RectF(L.listCard.X + 1, L.listHead.GetBottom() + 4.0f * s, L.listCard.Width - 2,
                       L.listCard.GetBottom() - L.listHead.GetBottom() - 5.0f * s);

    L.rowH = 38.0f * s;
    L.visible = (int)(L.listBody.Height / L.rowH);
    if (L.visible < 0) L.visible = 0;

    L.btnRefresh = RectF(pad, h - 58.0f * s, 128.0f * s, 38.0f * s);
    L.chip = RectF(pad + 142.0f * s, h - 58.0f * s, 330.0f * s, 38.0f * s);

    (void)rowCount;
    return L;
}

// ============================ 绘制 ============================

static void DrawHeader(Graphics& g, float w, float s) {
    bool busy = g_refresh.isRunning.load();
    size_t avail = g_ui.pool ? g_ui.pool->size() : 0;

    TextAt(g, L"免费代理池", g_ui.fTitle, RectF(20 * s, 18 * s, 400 * s, 34 * s), th::text);
    TextAt(g, L"免费 HTTP 代理 · 两段校验 · 可自动轮换或指定节点", g_ui.fSub,
           RectF(21 * s, 52 * s, 520 * s, 20 * s), th::muted);

    // 右上角状态
    std::wstring st = busy ? L"刷新中" : (avail ? L"运行中" : L"等待节点");
    Color dot = busy ? th::warn : (avail ? th::ok : th::muted);

    float tw = TextWidth(g, st, g_ui.fSub);
    float x = w - 20 * s - tw;
    FillBox(g, RectF(x - 16 * s, 30 * s, 8 * s, 8 * s), dot);
    TextAt(g, st, g_ui.fSub, RectF(x, 24 * s, tw + 2 * s, 20 * s), th::muted);
}

static void DrawAddrCard(Graphics& g, const Layout& L) {
    FillBox(g, L.addr, th::panel);
    StrokeBox(g, L.addr, th::border, 1.0f);

    TextAt(g, L"本地代理地址", g_ui.fSmall,
           RectF(L.addr.X + 18 * L.s, L.addr.Y + 10 * L.s, 300 * L.s, 16 * L.s), th::muted);

    std::wstring addr = L"http://" + Widen(g_ui.opt->host == "0.0.0.0" ? "127.0.0.1" : g_ui.opt->host) +
                        L":" + std::to_wstring(g_ui.opt->port);
    TextAt(g, addr, g_ui.fMono, RectF(L.addr.X + 18 * L.s, L.addr.Y + 28 * L.s, 420 * L.s, 26 * L.s),
           th::text);

    // 复制按钮
    bool hover = (g_ui.hover == 2);
    FillBox(g, L.btnCopy, hover ? th::panelHi : Color(255, 44, 48, 55));
    StrokeBox(g, L.btnCopy, th::border, 1.0f);
    TextAt(g, L"复制", g_ui.fBtn, L.btnCopy, th::text, StringAlignmentCenter);
}

static void DrawStats(Graphics& g, const Layout& L, float w) {
    size_t avail = g_ui.pool ? g_ui.pool->size() : 0;
    unsigned long long ok = g_ui.server ? g_ui.server->served() : 0;
    unsigned long long bad = g_ui.server ? g_ui.server->failed() : 0;

    float x = L.stats.X;
    struct Item { std::wstring label; std::wstring value; Color c; };
    Item items[3] = {
        {L"可用节点", std::to_wstring((unsigned)avail), th::ok},
        {L"已转发", Num(ok), th::text},
        {L"失败", Num(bad), bad ? th::bad : th::muted},
    };

    for (int i = 0; i < 3; ++i) {
        float lw = TextWidth(g, items[i].label, g_ui.fSmall);
        float vw = TextWidth(g, items[i].value, g_ui.fSmall);
        TextAt(g, items[i].label, g_ui.fSmall, RectF(x, L.stats.Y, lw + 4, L.stats.Height), th::muted);
        TextAt(g, items[i].value, g_ui.fSmall, RectF(x + lw + 6, L.stats.Y, vw + 8, L.stats.Height),
               items[i].c);
        x += lw + vw + 34 * L.s;
    }

    // 右侧：刷新进度或倒计时
    std::wstring right;
    if (g_refresh.isRunning.load()) {
        int d = g_refresh.done.load();
        int t = g_refresh.total.load();
        if (t > 0)
            right = Widen(g_refresh.stage()) + L"  " + std::to_wstring(d) + L"/" + std::to_wstring(t);
        else
            right = Widen(g_refresh.stage()) + L"…";
    } else {
        long long left = (long long)(g_ui.nextRefresh - GetTickCount64());
        if (left < 0) left = 0;
        right = L"下次刷新 " + ClockText((int)(left / 1000));
    }
    float rw = TextWidth(g, right, g_ui.fSmall);
    TextAt(g, right, g_ui.fSmall, RectF(w - 20 * L.s - rw, L.stats.Y, rw + 2, L.stats.Height),
           th::muted);
}

static void DrawList(Graphics& g, const Layout& L) {
    FillBox(g, L.listCard, th::panel);
    StrokeBox(g, L.listCard, th::border, 1.0f);

    // 表头
    float right = L.listBody.GetRight();
    TextAt(g, L"节点", g_ui.fSmall,
           RectF(L.listBody.X + 16 * L.s, L.listHead.Y, 200 * L.s, L.listHead.Height), th::muted);
    TextAt(g, L"延迟", g_ui.fSmall, RectF(right - 250 * L.s, L.listHead.Y, 70 * L.s, L.listHead.Height),
           th::muted, StringAlignmentFar);
    TextAt(g, L"状态", g_ui.fSmall, RectF(right - 74 * L.s, L.listHead.Y, 56 * L.s, L.listHead.Height),
           th::muted, StringAlignmentFar);

    // 进度条
    float barY = L.listHead.GetBottom();
    SolidBrush line(th::border);
    g.FillRectangle(&line, RectF(L.listCard.X + 1, barY, L.listCard.Width - 2, 1));

    if (g_refresh.isRunning.load()) {
        int d = g_refresh.done.load();
        int t = g_refresh.total.load();
        float frac = (t > 0) ? (float)d / (float)t : 0.0f;
        if (frac > 1.0f) frac = 1.0f;
        if (frac > 0.0f) {
            SolidBrush pb(th::accent);
            g.FillRectangle(&pb, RectF(L.listCard.X + 1, barY, (L.listCard.Width - 2) * frac, 2 * L.s));
        }
    }

    if (g_ui.snap.empty()) {
        std::wstring msg = g_refresh.isRunning.load() ? L"正在抓取可用节点…"
                                                      : L"暂无可用节点，点“立即刷新”重试";
        TextAt(g, msg, g_ui.fBody, L.listBody, th::muted, StringAlignmentCenter);
        return;
    }

    std::string pinned = g_ui.pool ? g_ui.pool->pinned() : std::string();

    GraphicsState st = g.Save();
    g.SetClip(L.listBody);

    int first = g_ui.scroll;
    int last = first + L.visible;
    if (last > (int)g_ui.snap.size()) last = (int)g_ui.snap.size();

    for (int i = first; i < last; ++i) {
        const Proxy& p = g_ui.snap[(size_t)i];
        float y = L.listBody.Y + (i - first) * L.rowH;
        RectF row(L.listBody.X, y, L.listBody.Width, L.rowH);
        bool selected = (!pinned.empty() && p.key() == pinned);
        bool hovered = (g_ui.hover == 1000 + i);

        if (selected) {
            FillBox(g, row, Color(36, 76, 141, 255));
            SolidBrush ab(th::accent);
            g.FillRectangle(&ab, RectF(row.X, row.Y, 3 * L.s, row.Height));
        } else if (hovered) {
            FillBox(g, row, th::panelHi);
        }

        Color keyColor = (p.fails >= 3) ? th::muted : th::text;
        TextAt(g, Widen(p.key()), g_ui.fMono,
               RectF(row.X + 20 * L.s, row.Y, row.Width - 330 * L.s, row.Height), keyColor);

        // 延迟
        std::wstring lat = std::to_wstring(p.latencyMs) + L" ms";
        TextAt(g, lat, g_ui.fSmall, RectF(right - 250 * L.s, row.Y, 70 * L.s, row.Height),
               p.latencyMs < 400 ? th::ok : (p.latencyMs < 1200 ? th::warn : th::muted),
               StringAlignmentFar);

        // 延迟条
        RectF bar(right - 166 * L.s, row.Y + row.Height / 2 - 3 * L.s, 84 * L.s, 6 * L.s);
        FillBox(g, bar, Color(255, 45, 48, 54));
        float frac = p.latencyMs / 2000.0f;
        if (frac < 0.05f) frac = 0.05f;
        if (frac > 1.0f) frac = 1.0f;
        Color bc = p.latencyMs < 400 ? th::ok : (p.latencyMs < 1200 ? th::warn : th::bad);
        FillBox(g, RectF(bar.X, bar.Y, bar.Width * frac, bar.Height), bc);

        // 状态
        std::wstring sta = (p.fails > 0) ? (L"失败 " + std::to_wstring(p.fails)) : L"就绪";
        TextAt(g, sta, g_ui.fSmall, RectF(right - 74 * L.s, row.Y, 56 * L.s, row.Height),
               p.fails > 0 ? th::bad : th::ok, StringAlignmentFar);
    }

    // 右侧滚动条（列表放不下时才显示）
    int totalRows = (int)g_ui.snap.size();
    if (totalRows > L.visible && L.visible > 0) {
        float trackX = L.listBody.GetRight() - 8 * L.s;
        float trackY = L.listBody.Y + 6 * L.s;
        float trackH = L.listBody.Height - 12 * L.s;
        if (trackH > 16 * L.s) {
            float thumbH = trackH * (float)L.visible / (float)totalRows;
            if (thumbH < 28 * L.s) thumbH = 28 * L.s;
            if (thumbH > trackH) thumbH = trackH;
            int maxScroll = totalRows - L.visible;
            float frac = (maxScroll > 0) ? (float)g_ui.scroll / (float)maxScroll : 0.0f;
            if (frac < 0.0f) frac = 0.0f;
            if (frac > 1.0f) frac = 1.0f;
            float thumbY = trackY + (trackH - thumbH) * frac;
            FillBox(g, RectF(trackX, trackY, 4 * L.s, trackH), Color(255, 38, 41, 47));
            FillBox(g, RectF(trackX, thumbY, 4 * L.s, thumbH), Color(255, 78, 84, 96));
        }
    }

    g.Restore(st);
}

static void DrawFooter(Graphics& g, const Layout& L, float w) {
    bool busy = g_refresh.isRunning.load();

    // 刷新按钮
    bool hov = (g_ui.hover == 1);
    Color bgc = busy ? Color(255, 40, 44, 51) : (hov ? Color(255, 96, 156, 255) : th::accent);
    FillBox(g, L.btnRefresh, bgc);
    TextAt(g, busy ? L"刷新中…" : L"立即刷新", g_ui.fBtn, L.btnRefresh,
           busy ? th::muted : Color(255, 255, 255, 255), StringAlignmentCenter);

    // 模式标签
    std::string pinned = g_ui.pool ? g_ui.pool->pinned() : std::string();
    std::wstring label;
    if (pinned.empty())
        label = L"模式：自动轮换（点击下方节点可固定）";
    else
        label = L"已固定：" + Widen(pinned) + L"（点击该行恢复自动）";

    bool hovChip = (g_ui.hover == 3);
    FillBox(g, L.chip, hovChip && !pinned.empty() ? th::panelHi : th::panel);
    StrokeBox(g, L.chip, pinned.empty() ? th::border : Color(255, 64, 104, 168), 1.0f);

    FillBox(g, RectF(L.chip.X + 14 * L.s, L.chip.Y + L.chip.Height / 2 - 3 * L.s, 6 * L.s, 6 * L.s),
            pinned.empty() ? th::ok : th::accent);
    TextAt(g, label, g_ui.fSmall,
           RectF(L.chip.X + 28 * L.s, L.chip.Y, L.chip.Width - 36 * L.s, L.chip.Height), th::text);

    // 底部提示
    TextAt(g, L"提示：免费代理不安全，请勿用于登录账号或支付", g_ui.fSmall,
           RectF(w - 20 * L.s - 340 * L.s, L.btnRefresh.Y, 340 * L.s, 18 * L.s), th::muted,
           StringAlignmentFar);
}

static void DrawToast(Graphics& g, float w, float h, float s) {
    if (g_ui.toast.empty()) return;
    if (GetTickCount64() > g_ui.toastUntil) {
        g_ui.toast.clear();
        return;
    }
    float tw = TextWidth(g, g_ui.toast, g_ui.fBody) + 36 * s;
    RectF r((w - tw) / 2, h - 116 * s, tw, 34 * s);
    FillBox(g, r, Color(240, 48, 52, 60));
    StrokeBox(g, r, th::border, 1.0f);
    TextAt(g, g_ui.toast, g_ui.fBody, r, th::text, StringAlignmentCenter);
}

// ============================ 刷新控制 ============================

static void StartRefresh() {
    if (g_refresh.isRunning.load()) return;
    g_refresh.isRunning = true;  // 先占位，避免定时器重复触发
    Options* opt = g_ui.opt;
    ProxyPool* pool = g_ui.pool;
    std::thread([opt, pool]() { refreshPool(*opt, *pool, NULL); }).detach();
}

// ============================ 交互 ============================

static int HitTest(int x, int y) {
    RECT rc;
    GetClientRect(g_ui.hwnd, &rc);
    float s = g_ui.s;
    Layout L = MakeLayout((float)rc.right, (float)rc.bottom, s, (int)g_ui.snap.size());

    PointF p((float)x, (float)y);
    if (L.btnCopy.Contains(p)) return 2;
    if (L.btnRefresh.Contains(p)) return 1;
    if (L.chip.Contains(p)) return 3;

    if (L.listBody.Contains(p)) {
        int idx = (int)((p.Y - L.listBody.Y) / L.rowH) + g_ui.scroll;
        if (idx >= 0 && idx < (int)g_ui.snap.size()) return 1000 + idx;
    }
    return 0;
}

static void ShowToast(const std::wstring& msg) {
    g_ui.toast = msg;
    g_ui.toastUntil = GetTickCount64() + 2200;
    InvalidateRect(g_ui.hwnd, NULL, FALSE);
}

static void CopyAddress() {
    std::string host = g_ui.opt->host == "0.0.0.0" ? "127.0.0.1" : g_ui.opt->host;
    std::wstring text = L"http://" + Widen(host) + L":" + std::to_wstring(g_ui.opt->port);
    if (OpenClipboard(g_ui.hwnd)) {
        EmptyClipboard();
        size_t bytes = (text.size() + 1) * sizeof(wchar_t);
        HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, bytes);
        if (h) {
            void* dst = GlobalLock(h);
            memcpy(dst, text.c_str(), bytes);
            GlobalUnlock(h);
            SetClipboardData(CF_UNICODETEXT, h);
        }
        CloseClipboard();
    }
    ShowToast(L"代理地址已复制到剪贴板");
}

static void Activate(int id) {
    if (id == 1) {
        if (g_refresh.isRunning.load()) return;
        g_ui.nextRefresh = GetTickCount64() + (ULONGLONG)g_ui.opt->refreshSec * 1000;
        StartRefresh();
        ShowToast(L"开始刷新代理池");
    } else if (id == 2) {
        CopyAddress();
    } else if (id == 3) {
        if (!g_ui.pool->pinned().empty()) {
            g_ui.pool->pin("");
            ShowToast(L"已恢复自动轮换");
        }
    } else if (id >= 1000) {
        int i = id - 1000;
        if (i < (int)g_ui.snap.size()) {
            std::string key = g_ui.snap[(size_t)i].key();
            if (g_ui.pool->pinned() == key) {
                g_ui.pool->pin("");
                ShowToast(L"已恢复自动轮换");
            } else {
                g_ui.pool->pin(key);
                ShowToast(L"已固定使用 " + Widen(key));
            }
        }
    }
    InvalidateRect(g_ui.hwnd, NULL, FALSE);
}

// ============================ 窗口过程 ============================

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_CREATE: {
            g_ui.hwnd = hwnd;
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);
            RECT rc;
            GetClientRect(hwnd, &rc);
            int w = rc.right, h = rc.bottom;
            if (w > 0 && h > 0) {
                Bitmap back(w, h, PixelFormat32bppPARGB);
                Graphics g(&back);
                g.SetSmoothingMode(SmoothingModeAntiAlias);
                g.SetPixelOffsetMode(PixelOffsetModeHalf);
                g.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);

                SolidBrush bg(th::bg);
                g.FillRectangle(&bg, 0, 0, w, h);

                float s = g_ui.s;
                Layout L = MakeLayout((float)w, (float)h, s, (int)g_ui.snap.size());

                DrawHeader(g, (float)w, s);
                DrawAddrCard(g, L);
                DrawStats(g, L, (float)w);
                DrawList(g, L);
                DrawFooter(g, L, (float)w);
                DrawToast(g, (float)w, (float)h, s);

                Graphics gd(hdc);
                gd.DrawImage(&back, 0, 0);
            }
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_MOUSEMOVE: {
            int id = HitTest(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
            if (id != g_ui.hover) {
                g_ui.hover = id;
                TRACKMOUSEEVENT tme;
                tme.cbSize = sizeof(tme);
                tme.dwFlags = TME_LEAVE;
                tme.hwndTrack = hwnd;
                TrackMouseEvent(&tme);
                InvalidateRect(hwnd, NULL, FALSE);
            }
            SetCursor(LoadCursor(NULL, id ? IDC_HAND : IDC_ARROW));
            return 0;
        }
        case WM_MOUSELEAVE:
            g_ui.hover = 0;
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;
        case WM_LBUTTONDOWN:
            g_ui.pressed = HitTest(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
            return 0;
        case WM_LBUTTONUP: {
            int id = HitTest(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
            if (id == g_ui.pressed && id != 0) Activate(id);
            g_ui.pressed = 0;
            return 0;
        }
        case WM_MOUSEWHEEL: {
            RECT rc;
            GetClientRect(hwnd, &rc);
            Layout L = MakeLayout((float)rc.right, (float)rc.bottom, g_ui.s, (int)g_ui.snap.size());
            int lines = ((int)g_ui.snap.size() > L.visible) ? (int)g_ui.snap.size() - L.visible : 0;
            int delta = GET_WHEEL_DELTA_WPARAM(wp);
            g_ui.scroll -= delta / 120 * 3;
            if (g_ui.scroll > lines) g_ui.scroll = lines;
            if (g_ui.scroll < 0) g_ui.scroll = 0;
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;
        }
        case WM_TIMER: {
            if (g_ui.pool) g_ui.snap = g_ui.pool->snapshot();

            // 自动刷新调度
            if (!g_refresh.isRunning.load()) {
                ULONGLONG now = GetTickCount64();
                if (now >= g_ui.nextRefresh) {
                    g_ui.nextRefresh = now + (ULONGLONG)g_ui.opt->refreshSec * 1000;
                    StartRefresh();
                }
            }

            // 滚动越界修正（刷新后条目变少）
            RECT rc;
            GetClientRect(hwnd, &rc);
            Layout L = MakeLayout((float)rc.right, (float)rc.bottom, g_ui.s, (int)g_ui.snap.size());
            int maxScroll = (int)g_ui.snap.size() - L.visible;
            if (maxScroll < 0) maxScroll = 0;
            if (g_ui.scroll > maxScroll) g_ui.scroll = maxScroll;

            InvalidateRect(hwnd, NULL, FALSE);
            return 0;
        }
        case WM_SIZE: {
            RECT rc;
            GetClientRect(hwnd, &rc);
            Layout L = MakeLayout((float)rc.right, (float)rc.bottom, g_ui.s, (int)g_ui.snap.size());
            int maxScroll = (int)g_ui.snap.size() - L.visible;
            if (maxScroll < 0) maxScroll = 0;
            if (g_ui.scroll > maxScroll) g_ui.scroll = maxScroll;
            return 0;
        }
        case WM_GETMINMAXINFO: {
            MINMAXINFO* mmi = (MINMAXINFO*)lp;
            mmi->ptMinTrackSize.x = (LONG)(700 * g_ui.s);
            mmi->ptMinTrackSize.y = (LONG)(520 * g_ui.s);
            return 0;
        }
        case WM_DESTROY:
            g_stop = true;
            if (g_ui.server) g_ui.server->stop();
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ============================ 入口 ============================

static FontFamily* PickFamily(const WCHAR* a, const WCHAR* b) {
    FontFamily* f = new FontFamily(a);
    if (f->IsAvailable()) return f;
    delete f;
    return new FontFamily(b);
}

static void InitFonts(float s) {
    FontFamily* yahei = PickFamily(L"Microsoft YaHei UI", L"Segoe UI");
    FontFamily* mono = PickFamily(L"Consolas", L"Courier New");

    g_ui.fTitle = new Font(yahei, 19 * s, FontStyleBold, UnitPixel);
    g_ui.fSub = new Font(yahei, 12 * s, FontStyleRegular, UnitPixel);
    g_ui.fBody = new Font(yahei, 13 * s, FontStyleRegular, UnitPixel);
    g_ui.fSmall = new Font(yahei, 12 * s, FontStyleRegular, UnitPixel);
    g_ui.fBtn = new Font(yahei, 13 * s, FontStyleBold, UnitPixel);
    g_ui.fMono = new Font(mono, 13 * s, FontStyleBold, UnitPixel);
}

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPSTR, int) {
    // 旧 mingw 头文件里没有声明，运行时动态取
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (user32) {
        typedef BOOL(WINAPI * DpiFn)();
        DpiFn fn = (DpiFn)GetProcAddress(user32, "SetProcessDPIAware");
        if (fn) fn();
    }

    int argc = 0;
    LPWSTR* wargv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::vector<std::string> args;
    for (int i = 0; i < argc; ++i) {
        int n = WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, NULL, 0, NULL, NULL);
        std::string s;
        if (n > 1) {
            s.resize((size_t)n - 1);
            WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, &s[0], n, NULL, NULL);
        }
        args.push_back(s);
    }
    LocalFree(wargv);

    Options* opt = new Options();
    bool help = false;
    std::string err;
    if (!parseArgv(args, *opt, help, err)) {
        MessageBoxW(NULL, Widen(err).c_str(), L"参数错误", MB_ICONERROR | MB_OK);
        return 2;
    }
    if (help) {
        MessageBoxW(NULL,
                    L"用法: proxypool-gui.exe [--port N] [--host IP] [--sources FILE]\n"
                    L"      [--threads N] [--timeout MS] [--refresh SEC] [--max N]",
                    L"proxypool", MB_ICONINFORMATION | MB_OK);
        return 0;
    }

    SetConsoleOutputCP(CP_UTF8);
    netInit();

    g_ui.pool = new ProxyPool();
    g_ui.opt = opt;
    g_ui.scroll = 0;
    g_ui.hover = 0;
    g_ui.pressed = 0;
    g_ui.toastUntil = 0;
    g_ui.tickBase = GetTickCount64();

    // 本地代理服务（先绑定端口，节点稍后填充）
    g_ui.server = new ProxyServer(*g_ui.pool, opt->host, opt->port, opt->timeoutMs, opt->ctlPort);
    std::string serr;
    if (!g_ui.server->start(serr)) {
        MessageBoxW(NULL, Widen("本地代理启动失败：" + serr).c_str(), L"proxypool",
                    MB_ICONERROR | MB_OK);
        return 1;
    }
    std::thread([]() { g_ui.server->runUntilStopped(); }).detach();

    HDC screen = GetDC(NULL);
    g_ui.s = (float)GetDeviceCaps(screen, LOGPIXELSX) / 96.0f;
    if (g_ui.s < 1.0f) g_ui.s = 1.0f;
    ReleaseDC(NULL, screen);

    GdiplusStartupInput gsi;
    ULONG_PTR token = 0;
    if (GdiplusStartup(&token, &gsi, NULL) != Ok) {
        MessageBoxW(NULL, L"GDI+ 初始化失败", L"proxypool", MB_ICONERROR | MB_OK);
        return 1;
    }

    WNDCLASSEXW wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = NULL;
    wc.lpszClassName = L"ProxyPoolWindow";
    RegisterClassExW(&wc);

    int w = (int)(840 * g_ui.s);
    int h = (int)(640 * g_ui.s);
    int x = (GetSystemMetrics(SM_CXSCREEN) - w) / 2;
    int y = (GetSystemMetrics(SM_CYSCREEN) - h) / 2;

    HWND hwnd = CreateWindowExW(0, L"ProxyPoolWindow", L"免费代理池", WS_OVERLAPPEDWINDOW,
                                x, y, w, h, NULL, NULL, hInst, NULL);
    if (!hwnd) {
        MessageBoxW(NULL, L"窗口创建失败", L"proxypool", MB_ICONERROR | MB_OK);
        return 1;
    }

    BOOL dark = TRUE;
    DwmSetWindowAttribute(hwnd, 20, &dark, sizeof(dark));  // DWMWA_USE_IMMERSIVE_DARK_MODE
    DwmSetWindowAttribute(hwnd, 19, &dark, sizeof(dark));  // 旧版本号

    // Win11 默认给窗口加圆角，这里显式关掉（Win10 上该属性无效，返回值忽略即可）
    int corner = 1;  // DWMWCP_DONOTROUND
    DwmSetWindowAttribute(hwnd, 33 /* DWMWA_WINDOW_CORNER_PREFERENCE */, &corner, sizeof(corner));

    InitFonts(g_ui.s);

    g_ui.snap = g_ui.pool->snapshot();
    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);

    SetTimer(hwnd, 1, 250, NULL);

    // 首次刷新
    g_ui.nextRefresh = GetTickCount64();
    StartRefresh();

    MSG m;
    while (GetMessageW(&m, NULL, 0, 0)) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }

    KillTimer(hwnd, 1);
    GdiplusShutdown(token);
    return 0;
}
