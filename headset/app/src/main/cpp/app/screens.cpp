// The headset's windows: the main panel (computers, games, settings), dock, keyboard,
// statistics, notices, the VR launch panel and the in-VR overlay.
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>

#include "../gfx/sky.h"
#include "../xr/thumbstick_deadzone.h"
#include "media/music_tracks.h"
#include "../platform/jni_bridge.h"
#include "app.h"
#include "rc/project.h"
#include "strings.h"

namespace rv::screens {

using namespace theme;

namespace {

App* g_app = nullptr;

constexpr const char* kVersion = RINCAST_VERSION_STRING;

// ---- shared drawing helpers -------------------------------------------------

void Background(Ui& ui, const Rect& r) { ui.R().RectBorder(r.Inset(1), kPanel, kPanelRadius, kPanelBorder, 2.f); }

// Brand mark: ring, core and the broadcast arc.
void Logo(Ui& ui, float cx, float cy, float s) {
    UiRenderer& R = ui.R();
    R.Arc(cx, cy, s * 0.30f, s * 0.085f, 0.f, 6.2832f, kAccent);
    R.Circle(cx, cy, s * 0.095f, kAccent);
    R.Arc(cx, cy, s * 0.43f, s * 0.07f, -1.35f, 1.13f, kAccent);
}

std::string FormatMs(double v) {
    char b[32];
    snprintf(b, sizeof(b), v >= 10 ? "%.0f" : "%.1f", v);
    return b;
}

std::string Percent(float v) { return std::to_string(int(std::round(v * 100))) + "%"; }

std::string PixelSize(int w, int h) { return std::to_string(w) + " × " + std::to_string(h); }

// Height of wrapped text, without drawing it.
float WrapHeight(Ui& ui, const std::string& text, float w, float size, float lineHeight) {
    return ui.R().WrapText(text, {0, 0, w, 0}, size, Color(0, 0, 0, 0), Face::Medium, lineHeight);
}

// A button as wide as its label needs.
float ButtonWidth(Ui& ui, const std::string& label) { return std::max(150.f, ui.R().Measure(label, 21, Face::SemiBold) + 56); }

// Lays out buttons from the left, on one line.
struct ButtonRow {
    float x, y;
    bool Add(Ui& ui, const char* id, const std::string& label, ButtonStyle style = ButtonStyle::Secondary, bool enabled = true) {
        const float w = ButtonWidth(ui, label);
        const bool clicked = ui.Button(id, {x, y, w, 54}, label, nullptr, style, enabled);
        x += w + 10;
        return clicked;
    }
};

// A column of settings, laid out from the top down.
struct Column {
    float x, y, w;
    bool started = false;
};

// Starts a group of settings.
void Section(Ui& ui, Column& c, const std::string& text) {
    if (c.started) c.y += 24;
    c.started = true;
    ui.R().Text(text, c.x, c.y, 28, kText, Face::SemiBold);
    c.y += 46;
}

// Names the control under it.
void Label(Ui& ui, Column& c, const std::string& text) {
    ui.R().Text(text, c.x, c.y, 21, kMuted, Face::Medium);
    c.y += 30;
}

// hint only where the name can't say it: a cost, or something it turns off.
bool CheckRow(Ui& ui, Column& c, const char* id, const std::string& label, bool& value, const std::string& hint = {}, bool enabled = true) {
    const float textW = c.w - 44;
    const float hintH = hint.empty() ? 0 : WrapHeight(ui, hint, textW, 20, 26) + 4;
    ui.NextLabel(hint.empty() ? label : label + ", " + hint);
    // The box lines up with the text above it; the hover area reaches a little past it.
    const bool changed = ui.Check(id, {c.x - 8, c.y, c.w + 8, 48 + hintH}, label, value, enabled);
    if (!hint.empty()) ui.R().WrapText(hint, {c.x + 44, c.y + 42, textW, hintH}, 20, kMuted, Face::Medium, 26);
    c.y += 50 + hintH;
    return changed;
}

// Options in a tidy grid, for when they don't fit on one line.
bool RadioGrid(Ui& ui, Column& c, const char* id, const std::string& name, const std::vector<std::string>& options, int& selected, int cols) {
    bool changed = false;
    const float w = c.w / cols;
    for (size_t i = 0; i < options.size(); i++) {
        const std::string cid = std::string(id) + "#" + std::to_string(i);
        ui.NextLabel(name);
        if (ui.Radio(cid.c_str(), {c.x - 8 + (i % cols) * w, c.y + (i / cols) * 46, w - 8, 46}, options[i], selected == int(i))) {
            selected = int(i);
            changed = true;
        }
    }
    c.y += ((options.size() + cols - 1) / cols) * 46 + 6;
    return changed;
}

// Options side by side, wrapping to the next line.
bool RadioLine(Ui& ui, Column& c, const char* id, const std::string& name, const std::vector<std::string>& options, int& selected) {
    bool changed = false;
    float x = c.x;
    for (size_t i = 0; i < options.size(); i++) {
        const float w = 50 + ui.R().Measure(options[i], 21, Face::Medium) + 28;
        if (x + w - 28 > c.x + c.w) {
            x = c.x;
            c.y += 48;
        }
        const std::string cid = std::string(id) + "#" + std::to_string(i);
        ui.NextLabel(name);
        if (ui.Radio(cid.c_str(), {x - 8, c.y, w - 20, 46}, options[i], selected == int(i))) {
            selected = int(i);
            changed = true;
        }
        x += w;
    }
    c.y += 52;
    return changed;
}

bool BarRow(Ui& ui, Column& c, const char* id, const std::string& name, float& value, float min, float max, float step, const std::string& text, bool enabled = true, float mark = -1) {
    ui.R().Text(name, c.x, c.y, 21, kMuted, Face::Medium);
    ui.R().Text(text, c.x + c.w, c.y, 21, enabled ? kText : kMuted, Face::SemiBold, Align::Right);
    ui.NextLabel(name);
    const bool changed = ui.Bar(id, {c.x, c.y + 22, c.w, 50}, value, min, max, step, text, enabled, mark);
    c.y += 84;
    return changed;
}

// A pair of lines: a small label over its value.
void Stat(Ui& ui, float x, float y, const std::string& label, const std::string& value, Align align = Align::Left, bool poor = false) {
    ui.R().Text(label, x, y, 20, kMuted, Face::Medium, align);
    ui.R().Text(value, x, y + 26, 21, poor ? kWarn : kText, Face::SemiBold, align);
}

// The same line the PC draws (threshold unchanged); A22: judged on the panel's EMA
// (LatencyDisplayMs), not the raw 1 s window average the diagnostics keep.
bool PoorLatency(double latencyMs) { return latencyMs >= 80; }
bool PoorFrameRate(const App::Stats& s, float wanted) {
    const double shown = s.fps + s.generatedFps;
    return shown > 0 && wanted > 0 && shown < wanted * 0.9f;
}

// ---- Pages of the main panel ----------------------------------------------------

enum Page { kComputers, kGames, kVrGames, kDesktop, kEnvironment, kControllers, kGeneral, kAccount, kHelp, kPageCount };

int g_page = kComputers;

bool Connected(App& app) { return app.CurrentMode() == Mode::Desktop || app.CurrentMode() == Mode::LaunchingVr || app.CurrentMode() == Mode::Vr; }

void ShowPage(App& app, int page) {
    if (page == kGames && Connected(app)) app.RequestGames();
    g_page = page;
}

struct PcEntry {
    std::string id;
    std::string name;
    std::string detail;
    bool lan = false;
    bool online = false;
    bool busy = false;
    DiscoveredPc lanInfo;
    CloudPc cloudInfo;
};

// One line of big type, then what to do about it, at the top left of the page. Returns where
// the page can go on.
float Message(Ui& ui, const Rect& b, const std::string& title, const std::string& text = {}, const Color& titleColor = kText, bool busy = false) {
    UiRenderer& R = ui.R();
    const float top = b.y;
    R.Text(title, b.x, top, 28, titleColor, Face::SemiBold);
    if (busy) ui.Spinner(b.x + R.Measure(title, 28, Face::SemiBold) + 34, top + 16, 16, kText);
    float y = top + 48;
    // A line is kept for the text either way, so a button below doesn't move when it changes.
    y += text.empty() ? 30 : R.WrapText(text, {b.x, y, std::min(b.w, 800.f), 200}, 21, kMuted, Face::Medium, 30);
    return y + 20;
}

// One computer in a list: every state uses the same positions, so nothing jumps between them.
constexpr float kPcRowH = 104;
Rect PcAction(const Rect& row, float w = 170) { return {row.Right() - 8 - w, row.y + 25, w, 54}; }

void PcText(Ui& ui, const Rect& row, const char* icon, const std::string& name, const std::string& detail, const Color& detailColor, bool enabled = true) {
    UiRenderer& R = ui.R();
    R.Icon(icon, row.x + 30, row.y + kPcRowH / 2, 56, enabled ? kText : kFaint);
    R.TextIn(name, {row.x + 76, row.y + 18, row.w - 300, 36}, 26, enabled ? kText : kMuted, Face::SemiBold);
    R.TextIn(detail, {row.x + 76, row.y + 56, row.w - 300, 30}, 21, detailColor);
}

float g_disconnectArmed = -100;

void PageComputers(Ui& ui, const Rect& body) {
    App& app = *g_app;
    UiRenderer& R = ui.R();
    const auto state = app.Account().State();
    const auto clientState = app.Client().GetState();

    // ---- connected ----
    if (Connected(app)) {
        const Rect row{body.x - 8, body.y - 8, body.w + 16, kPcRowH};
        const bool slowLink = PoorLatency(app.LatencyDisplayMs());  // A22: the smoothed value
        PcText(ui, row, "desktop_windows", app.Client().ServerName(),
               slowLink ? (app.Client().IsVpn() ? Tr("net.slowVpn") : app.Client().IsRemote() ? Tr("net.slowInternet") : Tr("net.slowLocal"))
                        : app.Client().IsVpn() ? Tr("net.throughVpn") : app.Client().IsRemote() ? Tr("net.overInternet") : Tr("net.connected"),
               slowLink ? kWarn : kGood);
        const bool armed = ui.Time() - g_disconnectArmed < 4.f;
        if (ui.Button("pc-disconnect", PcAction(row, armed ? 320 : 170), armed ? Tr("btn.disconnectAgain") : Tr("btn.disconnect"), nullptr, armed ? ButtonStyle::Danger : ButtonStyle::Secondary)) {
            if (armed) app.Disconnect();
            else g_disconnectArmed = ui.Time();
        }
        ButtonRow b{body.x, row.Bottom() + 16};
        if (b.Add(ui, "pc-games", Tr("btn.chooseGame"), ButtonStyle::Primary)) ShowPage(app, kGames);
        if (b.Add(ui, "pc-steamvr", app.VrRunning() ? Tr("btn.backToVr") : Tr("btn.startSteamvr"))) app.LaunchSteamVr();
        const auto st = app.CurrentStats();
        Column c{body.x, row.Bottom() + 100, body.w};
        struct Line {
            const char* name;
            std::string value;
        };
        const Line lines[] = {{Tr("stat.network"), st.rttMs > 0 ? Trf("unit.roundTrip", {{"ms", FormatMs(st.rttMs)}}) : "–"},
                              {Tr("stat.decoding"), st.decodeMs > 0 ? FormatMs(st.decodeMs) + " ms" : "–"},
                              {Tr("stat.video"), st.codec.empty() ? "–" : st.codec + ", " + PixelSize(st.width, st.height)}};
        for (const Line& l : lines) {
            ui.Divider(c.x, c.y, c.w);
            R.Text(l.name, c.x, c.y + 12, 21, kMuted);
            R.Text(l.value, c.x + 200, c.y + 12, 21, kText, Face::Medium);
            c.y += 52;
        }
        ui.Divider(c.x, c.y, c.w);
        return;
    }

    // ---- connecting / approval ----
    if (app.CurrentMode() == Mode::Connecting || app.Reconnecting()) {
        const bool approval = clientState == StreamClient::State::WaitingApproval;
        const std::string title = approval ? Tr("computers.waitingPc")
                                  : app.Reconnecting() ? Tr("computers.reconnecting")
                                                       : Trf("computers.connectingTo", {{"name", app.Client().ServerName()}});
        const std::string text = approval ? Tr("computers.approvalHint")
                                 : app.Reconnecting() ? Tr("computers.reconnectingText")
                                                      : app.Client().StatusText();
        const float y = Message(ui, body, title, text, kText, true);
        ButtonRow b{body.x, y};
        if (b.Add(ui, "cancel", Tr("btn.cancel"))) {
            app.DismissProblem();
            app.Disconnect();
        }
        return;
    }

    Rect list = body;
    // What went wrong with the last connection stays until it's dealt with.
    if (!app.ConnectionProblem().empty()) {
        R.Text(Tr("computers.couldntConnect"), body.x, body.y, 28, kDanger, Face::SemiBold);
        const float h = R.WrapText(app.ConnectionProblem(), {body.x, body.y + 44, body.w - 330, 60}, 21, kText, Face::Medium, 28);
        const float right = body.Right();
        if (ui.Button("dismiss", {right - 130, body.y + 6, 130, 54}, Tr("btn.dismiss"))) app.DismissProblem();
        if (app.CanRetry() && ui.Button("retry", {right - 130 - 10 - 160, body.y + 6, 160, 54}, Tr("btn.tryAgain"), nullptr, ButtonStyle::Primary)) app.RetryConnection();
        const float used = std::max(70.f, 44 + h) + 28;
        ui.Divider(body.x, body.y + used - 14, body.w);
        list.y += used;
        list.h -= used;
    }

    // ---- PC list ----
    std::vector<PcEntry> entries;
    for (const auto& pc : app.Lan().List()) {
        PcEntry e;
        e.id = pc.serverId;
        e.name = pc.name;
        e.lan = true;
        e.online = true;
        e.busy = pc.busy;
        e.detail = pc.viaUsb ? Tr("pcDetail.usb") : pc.viaVpn ? Tr("pcDetail.vpn") : Tr("pcDetail.lan");
        e.lanInfo = pc;
        entries.push_back(e);
    }
    for (const auto& pc : app.Account().Pcs()) {
        bool dup = false;
        for (auto& e : entries)
            if (e.id == pc.id) dup = true;
        if (dup) continue;
        PcEntry e;
        e.id = pc.id;
        e.name = pc.name;
        e.online = pc.online;
        e.busy = pc.busy;
        e.detail = pc.online ? Tr("pcDetail.remote") : Tr("pcDetail.offlineHint");
        e.cloudInfo = pc;
        entries.push_back(e);
    }

    const float searching = app.TimeOnHomeWithoutPcs(entries.empty());
    static bool enteringAddress = false, addressValid = true;
    static std::string address;
    static std::string lookingAt;
    static float lookingSince = 0;
    static size_t entriesWhenLooking = 0;
    auto openAddress = [&] {
        enteringAddress = true;
        addressValid = true;
        address = app.Settings().manualPc;
    };
    if (enteringAddress) {
        // Number pad for networks that block discovery (guest Wi-Fi, some mesh systems).
        const float y = Message(ui, list, Tr("computers.enterAddressTitle"), Tr("computers.enterAddressText"));
        const float kw = 124, kh = 56, padW = 3 * kw + 16;
        const Rect field{list.x, y, padW, 58};
        R.RectBorder(field, Color(0, 0, 0, 1), kRadius, kEdge, 2.f);
        R.TextIn(address.empty() ? " " : address, field.Inset(16, 0), 28, kText, Face::SemiBold);
        static const char* keys[4][3] = {{"1", "2", "3"}, {"4", "5", "6"}, {"7", "8", "9"}, {".", "0", "<"}};
        for (int row = 0; row < 4; row++) {
            for (int col = 0; col < 3; col++) {
                const std::string k = keys[row][col];
                const Rect kr{list.x + col * (kw + 8), y + 68 + row * (kh + 6), kw, kh};
                const std::string id = "pad-" + k;
                const bool back = k == "<";
                if (back) ui.NextLabel(Tr("a11y.backspace"));
                if (ui.Button(id.c_str(), kr, back ? "" : k, back ? "backspace" : nullptr, ButtonStyle::Secondary, true, 30)) {
                    if (back) { if (!address.empty()) address.pop_back(); }
                    else if (address.size() < 15) address += k;
                }
            }
        }
        // The actions stay beside the pad, where they fit whatever came above it.
        const float ax = list.x + padW + 24;
        if (ui.Button("pad-ok", {ax, y, 240, 58}, Tr("btn.lookForPc"), nullptr, ButtonStyle::Primary, !address.empty())) {
            // Four numbers from 0 to 255; anything else can't be a PC on this network.
            int a = -1, b = -1, c = -1, d = -1;
            char rest = 0;
            const int parts = std::sscanf(address.c_str(), "%d.%d.%d.%d%c", &a, &b, &c, &d, &rest);
            addressValid = parts == 4 && a >= 0 && a <= 255 && b >= 0 && b <= 255 && c >= 0 && c <= 255 && d >= 0 && d <= 255;
            if (addressValid) {
                app.SetManualPc(address);
                enteringAddress = false;
                lookingAt = address;
                lookingSince = app.Time();
                entriesWhenLooking = entries.size();
            }
        }
        if (ui.Button("pad-cancel", {ax, y + 68, 240, 56}, Tr("btn.cancel"))) {
            enteringAddress = false;
            addressValid = true;
        }
        if (!addressValid) R.WrapText(Tr("computers.badAddress"), {ax, y + 140, list.Right() - ax, 90}, 20, kWarn, Face::Medium, 27);
        return;
    }
    if (entries.empty() && !lookingAt.empty()) {
        if (app.Time() - lookingSince < 8.f) {
            Message(ui, list, Trf("computers.lookingAt", {{"address", lookingAt}}), {}, kText, true);
        } else {
            const float y = Message(ui, list, Trf("computers.noAnswer", {{"address", lookingAt}}), Tr("computers.noAnswerHint"), kWarn);
            ButtonRow b{list.x, y};
            if (b.Add(ui, "manual-again", Tr("btn.changeAddress"))) {
                openAddress();
                lookingAt.clear();
            }
            if (b.Add(ui, "manual-done", Tr("btn.back"))) lookingAt.clear();
        }
        return;
    }
    if (entries.size() > entriesWhenLooking) lookingAt.clear();  // it turned up
    if (entries.empty()) {
        float y;
        if (searching > 15.f) {
            y = Message(ui, list, Tr("computers.stillLooking"), {}, kText, true) - 12;
            const std::string getIt = Trf("computers.getIt", {{"site", rc::kDownloadSite}});
            const std::string checks[] = {getIt, Tr("computers.check1"), Tr("computers.check2")};
            for (int i = 0; i < 3; i++) {
                R.Text(std::to_string(i + 1), list.x, y, 21, kText, Face::SemiBold);
                y += R.WrapText(checks[i], {list.x + 34, y, std::min(list.w - 34, 660.f), 90}, 21, kMuted, Face::Medium, 30) + 12;
            }
            y += 10;
        } else {
            y = Message(ui, list, Tr("computers.looking"), Tr("computers.lookingHint"), kText, true);
        }
        ButtonRow b{list.x, y};
        if (b.Add(ui, "manual", Tr("btn.enterAddress"))) openAddress();
        if (state != Cloud::SignInState::SignedIn && b.Add(ui, "signin-empty", Tr("btn.signinOtherNetworks"))) ShowPage(app, kAccount);
        return;
    }

    // The address someone typed gets its answer here, above what's already listed.
    if (!lookingAt.empty()) {
        const bool still = app.Time() - lookingSince < 8.f;
        const std::string lookTitle = still ? Trf("computers.lookingAt", {{"address", lookingAt}})
                                            : Trf("computers.noAnswer", {{"address", lookingAt}});
        R.TextIn(lookTitle, {list.x, list.y, list.w - 330, 54}, 22, still ? kText : kWarn, Face::SemiBold);
        if (still) {
            ui.Spinner(list.x + R.Measure(lookTitle, 22, Face::SemiBold) + 30, list.y + 27, 13, kText);
        } else {
            if (ui.Button("look-again", {list.Right() - 240, list.y, 180 + 60, 54}, Tr("btn.changeAddress"))) {
                openAddress();
                lookingAt.clear();
            }
        }
        ui.Divider(list.x, list.y + 66, list.w);
        list.y += 80;
        list.h -= 80;
    }
    // The one you'd most likely want stands out: the last one used, or else the first free one.
    std::string likeliest;
    for (const auto& e : entries) {
        if (!e.online || e.busy) continue;
        if (likeliest.empty() || e.id == app.Settings().lastPc) likeliest = e.id;
    }
    auto connect = [&](const PcEntry& e) {
        if (e.lan) app.ConnectLan(e.lanInfo);
        else app.ConnectCloud(e.cloudInfo);
    };
    // Auto connect happens once, when RinCast opens: never after you've disconnected yourself.
    static bool triedAutoConnect = false;
    if (!triedAutoConnect && app.Settings().autoConnect && app.ConnectionProblem().empty()) {
        for (const auto& e : entries) {
            if (e.id != app.Settings().lastPc || !e.online || e.busy) continue;
            triedAutoConnect = true;
            connect(e);
            return;
        }
    }
    // Someone's using a PC: connecting ends their session, so it takes a second press.
    static std::string takeOverId;
    static float takeOverAt = -100;
    const float scroll = ui.BeginScroll("pcs", {list.x - 8, list.y - 8, list.w + 16, list.h + 8 - 70}, entries.size() * (kPcRowH + 8));
    float y = list.y - 8 - scroll;
    for (const auto& e : entries) {
        const Rect row{list.x - 8, y, list.w + 16, kPcRowH};
        const std::string id = "pc-" + e.id;
        const bool armed = e.busy && takeOverId == e.id && ui.Time() - takeOverAt < 4.f;
        const std::string detail = e.busy ? Tr("pcDetail.busy") : e.detail;
        ui.NextLabel(e.name + ", " + detail);
        if (ui.Hit(id.c_str(), row, e.online)) {
            if (e.busy && !armed) {
                takeOverId = e.id;
                takeOverAt = ui.Time();
            } else {
                connect(e);
            }
        }
        ui.HoverFill(row, ui.IsHot(id.c_str()) ? 1.f : 0.f);
        PcText(ui, row, "desktop_windows", e.name, detail, e.busy ? kWarn : kMuted, e.online);
        // Looks like a button; the whole row takes the click, which is easier to hit.
        const Rect action = PcAction(row, armed ? 220 : 170);
        if (e.online) {
            // While Tr("btn.tryAgain") is on show, that's the main action.
            const bool primary = !armed && e.id == likeliest && app.ConnectionProblem().empty();
            const Color fill = armed ? kDangerFill : primary ? kAccent : kControl;
            R.RectFill(action, fill, kRadius);
            R.TextIn(armed ? Tr("btn.pressTakeOver") : e.busy ? Tr("btn.takeOver") : Tr("btn.connect"), action, 21, primary ? kOnAccent : kText, Face::SemiBold, Align::Center);
        } else {
            R.TextIn(Tr("pcDetail.offline"), action, 21, kMuted, Face::SemiBold, Align::Right);
        }
        y += kPcRowH + 8;
    }
    ui.EndScroll();
    // A network that blocks discovery can hide the one you're after.
    ButtonRow more{list.x, list.Bottom() - 54};
    if (more.Add(ui, "manual", Tr("btn.enterAddress"))) openAddress();
}

// Up to two lines, then an ellipsis.
void TwoLines(Ui& ui, const std::string& text, const Rect& r, float size, float lineHeight) {
    UiRenderer& R = ui.R();
    if (R.Measure(text, size, Face::SemiBold) <= r.w) {
        R.Text(text, r.x, r.y, size, kText, Face::SemiBold);
        return;
    }
    size_t cut = 0;
    for (size_t i = text.find(' '); i != std::string::npos; i = text.find(' ', i + 1)) {
        if (R.Measure(text.substr(0, i), size, Face::SemiBold) > r.w) break;
        cut = i;
    }
    if (cut == 0) {
        R.Text(R.Ellipsize(text, r.w, size, Face::SemiBold), r.x, r.y, size, kText, Face::SemiBold);
        return;
    }
    R.Text(text.substr(0, cut), r.x, r.y, size, kText, Face::SemiBold);
    R.Text(R.Ellipsize(text.substr(cut + 1), r.w, size, Face::SemiBold), r.x, r.y + lineHeight, size, kText, Face::SemiBold);
}

void PageGames(Ui& ui, const Rect& body) {
    App& app = *g_app;
    UiRenderer& R = ui.R();
    if (!Connected(app)) {
        const float y = Message(ui, body, Tr("games.connectFirst"), Tr("games.connectFirstHint"));
        ButtonRow b{body.x, y};
        if (b.Add(ui, "to-computers", Tr("btn.choosePc"), ButtonStyle::Primary)) ShowPage(app, kComputers);
        return;
    }
    auto& games = app.Games();
    if (games.empty() && !app.GamesAnswered() && app.GamesRequestedAgo() >= 5.f) {
        const float y = Message(ui, body, Tr("games.noAnswer"), Tr("games.noAnswerHint"), kWarn);
        ButtonRow b{body.x, y};
        if (b.Add(ui, "refresh", Tr("btn.tryAgain"), ButtonStyle::Primary)) app.RequestGames();
        return;
    }
    if (games.empty() && !app.GamesAnswered()) {
        Message(ui, body, Tr("games.looking"), {}, kText, true);
        return;
    }
    if (games.empty()) {
        const float y = Message(ui, body, Tr("games.none"), Tr("games.noneHint"));
        ButtonRow b{body.x, y};
        if (b.Add(ui, "refresh", Tr("btn.lookAgain"))) app.RequestGames();
        return;
    }
    const int cols = 5;
    const float gap = 16;
    const float cardW = (body.w - gap * (cols - 1)) / cols;
    const float cardH = cardW * 1.5f + 66;
    const int rows = int((games.size() + cols - 1) / cols);
    const float scroll = ui.BeginScroll("games", {body.x - 6, body.y, body.w + 16, body.h}, rows * (cardH + gap));
    for (size_t i = 0; i < games.size(); i++) {
        GameInfo& g = games[i];
        const Rect card{body.x + (i % cols) * (cardW + gap), body.y + (i / cols) * (cardH + gap) - scroll, cardW, cardH};
        if (card.Bottom() < body.y || card.y > body.Bottom()) continue;
        app.RequestGameImage(g);
        const std::string id = "game-" + g.id;
        const Rect art{card.x, card.y, cardW, cardW * 1.5f};
        ui.NextLabel(g.name);
        if (ui.Hit(id.c_str(), card)) app.LaunchGame(g.id);
        if (g.image.id) {
            R.Image(g.image.id, art, kRadius);
        } else {
            R.RectFill(art, kControl, kRadius);
            R.Icon("sports_esports", art.x + art.w / 2, art.y + art.h / 2, 56, kFaint);
        }
        if (ui.IsHeld(id.c_str())) R.RectFill(art, Color(0, 0, 0, 0.4f), kRadius);
        ui.HoverLighten(art, ui.IsHot(id.c_str()) ? 1.f : 0.f);
        if (app.VrRunning() && app.VrStateText() == g.name) {
            const Rect tag{art.x + 8, art.y + 8, 104, 34};
            R.RectFill(tag, kAccent, kRadius);
            R.TextIn(Tr("games.running"), tag, 20, kOnAccent, Face::SemiBold, Align::Center);
        }
        TwoLines(ui, g.name, {card.x, art.Bottom() + 8, cardW, 54}, 20, 25);
    }
    ui.EndScroll();
}

bool PageVrQuality(Ui& ui, const Rect& body) {
    App& app = *g_app;
    HeadsetSettings& s = app.Settings();
    bool changed = false;
    const float gap = 44, w = (body.w - gap) / 2;
    // This page grew with the quality-profile and enhancement rows and no longer
    // fits on one screen - the lower controls were unreachable with the remote.
    // Same stick-only scroll as the General page (the trigger pull cancels a drag).
    static float contentBottom = 0;
    const float scroll = ui.BeginScroll("vr-quality", body, std::max(0.f, contentBottom - body.y), false);
    Column l{body.x, body.y - scroll, w};
    // A86 (user, 2026-11-23): the "responsive <-> sharp" slider that used to sit at the
    // top of this page is GONE from both ends of the product. It had already stopped
    // being a choice in A84 (the PC read it as the floor the tuner worked above), and
    // the user's call was to stop asking at all: "the tuner handles it". The picture
    // tier is now session state on the PC, the headset neither shows nor sends it, and
    // streamProfile is no longer a synced key either way (pc/streamer/src/settings.cpp
    // stopped persisting it). Nothing replaced the row - there is no control here any
    // more, only the image controls below it.
    Section(ui, l, Tr("vr.imageSection"));
    // Pixels per eye, against the headset's panel. Each change restarts the stream (and may ask
    // for a SteamVR restart), so it's applied when the handle is let go.
    static float dragScale = -1;
    float scale = dragScale >= 0 ? dragScale : s.vrScale;
    uint32_t fullW = 0, fullH = 0;
    app.Device().FullResolution(app.Xr(), fullW, fullH);
    const std::string pixels = PixelSize(int(fullW * scale), int(fullH * scale));
    if (BarRow(ui, l, "resolution", Tr("vr.resolution"), scale, kMinVrScale, kMaxVrScale, 0.05f,
               s.matchSteamVr ? std::string(Tr("vr.sameAsSteamVr")) : Percent(scale) + ", " + pixels, !s.matchSteamVr, 1.f))
        dragScale = scale;
    if (dragScale >= 0 && !ui.IsHeld("resolution")) {
        const bool resChanged = std::fabs(s.vrScale - dragScale) > 0.001f;
        s.SetVrScale(dragScale);
        dragScale = -1;
        changed = true;
        if (resChanged && app.VrRunning()) app.PostNotice(Tr("notice.restartSteamVrForRes"));
    }
    changed |= CheckRow(ui, l, "match-steamvr", Tr("vr.matchSteamVr"), s.matchSteamVr, Tr("vr.matchSteamVrHint"));
    l.y += 14;
    Label(ui, l, Tr("vr.fpsLabel"));
    std::vector<std::string> rates;
    int ri = 0;
    const auto available = app.Xr().RefreshRates();
    for (size_t i = 0; i < available.size(); i++) {
        rates.push_back(std::to_string(int(std::round(available[i]))));
        if (std::fabs(available[i] - s.refreshRate) < 1) ri = int(i);
    }
    if (rates.size() > 1) {
        if (RadioLine(ui, l, "rate", Tr("vr.fpsA11y"), rates, ri)) {
            s.refreshRate = available[size_t(ri)];
            changed = true;
        }
    } else if (!rates.empty()) {
        ui.R().Text(rates[0] + " fps", l.x, l.y + 6, 21, kText);
        l.y += 46;
    }
    // A97: the app already reports this device's decode wall to the PC (media/video_decoder.cpp:100),
    // the PC turns it into a WARN at stream start, and none of it ever came back to the page that
    // picks the pixels - so "it stutters, why" had no answer on screen. Same figure, same condition as
    // the PC's (client_session.cpp:1458 checks the rate against the hint and nothing else), shown
    // where the choice is made. It is a known value for one model, not a live measurement, so the
    // wording says "known" and the line is advice, never a lock: the slider still goes where it wants.
    // `ask` is the rate the stream really gets: with one entry in the list the PC clamps to it.
    static const int wallFps = VideoDecoder::Capabilities().maxFpsAtMaxRes;
    const float ask = rates.empty() ? s.refreshRate : available[size_t(ri)];
    if (wallFps > 0 && ask > float(wallFps)) {
        const std::string wall = Trf("vr.decodeWall", {{"fps", std::to_string(wallFps)}});
        ui.R().WrapText(wall, {l.x, l.y + 2, l.w, 64}, 20, kWarn, Face::Medium, 26);
        l.y += WrapHeight(ui, wall, l.w, 20, 26) + 12;
    }
    l.y += 14;
    Label(ui, l, Tr("vr.codecLabel"));
    // Not static: a static initializer would freeze the translated label in whatever
    // language was active the first time this page opened.
    const std::vector<std::string> codecs = {Tr("codec.auto"), "HEVC", "AV1", "H.264"};
    static const char* ck[] = {"auto", "hevc", "av1", "h264"};
    int ci = 0;
    for (int i = 0; i < 4; i++)
        if (s.codec == ck[i]) ci = i;
    if (RadioGrid(ui, l, "codec", Tr("vr.codecLabel"), codecs, ci, 2)) {
        s.codec = ck[ci];
        changed = true;
    }
    changed |= CheckRow(ui, l, "10bit", Tr("vr.tenBit"), s.tenBit, ci == 3 ? Tr("vr.needsHevcAv1") : "", ci != 3);
    Section(ui, l, Tr("vr.enhancements"));
    changed |= CheckRow(ui, l, "upscaling", Tr("vr.upscaling"), s.upscaling, Tr("vr.upscalingHint"));
    static bool sharpenDirty = false;
    float sharpenPct = std::round(s.vrSharpen * 100.0f);
    const std::string sharpenText = s.vrSharpen <= 0.001f ? Tr("stat.frameGen.off") : std::to_string(int(sharpenPct)) + "%";
    if (BarRow(ui, l, "vr-sharpen", Tr("vr.sharpen"), sharpenPct, 0, 100, 5, sharpenText)) {
        s.vrSharpen = std::clamp(sharpenPct / 100.0f, 0.0f, 1.0f);
        sharpenDirty = true;
    }
    if (sharpenDirty && !ui.IsHeld("vr-sharpen")) {
        sharpenDirty = false;
        app.SaveSettings();
    }
    static bool vibranceDirty = false;
    float vibrancePct = std::round((s.vrVibrance / 0.45f) * 100.0f);
    const std::string vibranceText = vibrancePct <= 0.5f ? Tr("stat.frameGen.off") : std::to_string(int(vibrancePct)) + "%";
    if (BarRow(ui, l, "vr-vibrance", Tr("vr.vibrance"), vibrancePct, 0, 100, 5, vibranceText)) {
        s.vrVibrance = std::clamp((vibrancePct / 100.0f) * 0.45f, 0.0f, 0.5f);
        vibranceDirty = true;
    }
    if (vibranceDirty && !ui.IsHeld("vr-vibrance")) {
        vibranceDirty = false;
        app.SaveSettings();
    }
    static bool darkDirty = false;
    float darkPct = std::round((s.vrDarkGamma / 0.30f) * 100.0f);
    const std::string darkText = darkPct <= 0.5f ? Tr("stat.frameGen.off") : std::to_string(int(darkPct)) + "%";
    if (BarRow(ui, l, "vr-dark-gamma", Tr("vr.darkGamma"), darkPct, 0, 100, 5, darkText)) {
        s.vrDarkGamma = std::clamp((darkPct / 100.0f) * 0.30f, 0.0f, 0.35f);
        darkDirty = true;
    }
    if (darkDirty && !ui.IsHeld("vr-dark-gamma")) {
        darkDirty = false;
        app.SaveSettings();
    }
    changed |= CheckRow(ui, l, "framegen", Tr("vr.frameGen"), s.frameGeneration, Tr("vr.frameGenHint"));

    Column c{body.x + w + gap, body.y - scroll, w};
    Section(ui, c, Tr("vr.bitrateSection"));
    const bool usePc = s.vrBitrateMbps == 0;
    // The PC tells what its own limit is once connected.
    const int pcLimit = app.ServerInfo().value("vrBitrateMbps", 0);
    bool followPc = usePc;
    if (CheckRow(ui, c, "bitrate-pc", Tr("vr.usePcLimit"), followPc)) {
        s.vrBitrateMbps = followPc ? 0 : pcLimit > 0 ? pcLimit : 150;
        changed = true;
    }
    float bitrate = usePc ? float(std::max(20, pcLimit)) : float(s.vrBitrateMbps);
    const std::string bitrateText = !usePc ? std::to_string(int(bitrate)) + " Mbps" : pcLimit > 0 ? std::to_string(pcLimit) + " Mbps" : Tr("vr.setOnPc");
    if (BarRow(ui, c, "bitrate", Tr("vr.bitrateLabel"), bitrate, 20, 500, 20, bitrateText, !usePc)) {
        s.vrBitrateMbps = int(bitrate);
        changed = true;
    }
    changed |= CheckRow(ui, c, "adaptive", Tr("vr.adaptive"), s.adaptiveBitrate);
    Section(ui, c, Tr("vr.tracking"));
    // The Quest can't run body and finger tracking together, so there each turns the other off.
    Headset& device = app.Device();
    const bool exclusive = device.BodyExcludesFingers();
    if (device.BodySupported(app.Xr())) {
        const std::string status = device.BodyStatus();
        const char* hint = device.BodyHint();
        if (CheckRow(ui, c, "body", Tr("vr.bodyTracking"), s.bodyTracking, !status.empty() ? status : hint ? hint : "")) {
            changed = true;
            if (s.bodyTracking && exclusive) s.fingersWithControllers = false;
        }
        if (const char* action = device.BodyAction()) {
            ButtonRow b{c.x + 44, c.y};
            if (b.Add(ui, "body-action", action)) device.RunBodyAction(app.Xr());
            c.y += 62;
        }
    }
    if (app.Input().SimultaneousSupported() &&
        CheckRow(ui, c, "fingers", Tr("vr.fingerTracking"), s.fingersWithControllers, exclusive ? Tr("vr.fingerExclusive") : "")) {
        changed = true;
        if (s.fingersWithControllers && exclusive) s.bodyTracking = false;
    }
    changed |= CheckRow(ui, c, "handsgames", Tr("vr.handsInGames"), s.handsInGames);
    if (device.FaceSupported(app.Xr()))
        changed |= CheckRow(ui, c, "face", Tr("vr.faceTracking"), s.faceTracking, device.FaceHint());
    changed |= CheckRow(ui, c, "foveated", Tr("vr.foveated"), s.gazeFoveation,
                        Tr(app.GazeSupported() ? "vr.foveatedHint" : "vr.foveatedFixedHint"));
    changed |= CheckRow(ui, c, "foveadebug", Tr("vr.foveaDebug"), s.foveaDebug,
                        Tr("vr.foveaDebugHint"));
    // Hand the measured content height to the next frame for the scroll range.
    contentBottom = std::max(l.y, c.y) + scroll;
    ui.EndScroll();
    return changed;
}

// The keyboard's settings (BetterFrame Keyboard's, from Steam's Settings > Keyboard on the Frame).
// They apply straight away and are kept with the keyboard's own.
void KeyboardSettings(Ui& ui, Column& l, Column& c) {
    App& app = *g_app;
    kb::Keyboard& kbd = app.Kb();
    kb::Config& k = kbd.Conf();
    const kb::Data& data = kb::Data::Get();
    bool changed = false;
    Section(ui, l, Tr("kbd.section"));
    Label(ui, l, Tr("kbd.theme"));
    const auto& themes = kb::Keyboard::ThemeNames();
    std::vector<std::string> themeNames;
    int theme = 0;
    for (size_t i = 0; i < themes.size(); i++) {
        themeNames.push_back(Tr(("kbd.theme." + themes[i]).c_str()));
        if (themes[i] == k.theme) theme = int(i);
    }
    if (RadioGrid(ui, l, "kbtheme", Tr("kbd.theme"), themeNames, theme, 2)) {
        k.theme = themes[theme];
        changed = true;
    }
    l.y += 8;
    float backdrop = k.backdrop / 100.f;
    if (BarRow(ui, l, "kbbackdrop", Tr("kbd.backdrop"), backdrop, 0, 1, 0.1f, Percent(backdrop))) {
        k.backdrop = int(std::round(backdrop * 100));
        changed = true;
    }
    l.y += 8;
    Label(ui, l, Tr("kbd.bar"));
    static const char* bars[] = {"top", "bottom", "off"};
    int bar = k.bar == "bottom" ? 1 : k.bar == "off" ? 2 : 0;
    if (RadioLine(ui, l, "kbbar", Tr("kbd.bar"), {Tr("kbd.barTop"), Tr("kbd.barBottom"), Tr("kbd.off")}, bar)) {
        k.bar = bars[bar];
        changed = true;
    }
    Label(ui, l, Tr("kbd.bottom"));
    int bottom = k.bottom == "laptop" ? 1 : 0;
    if (RadioLine(ui, l, "kbbottom", Tr("kbd.bottom"), {".?123, Fn, Ctrl", Tr("kbd.bottomLaptop")}, bottom)) {
        k.bottom = bottom ? "laptop" : "default";
        changed = true;
    }
    changed |= CheckRow(ui, l, "kbtips", Tr("kbd.tips"), k.tips, Tr("kbd.tipsHint"));
    ButtonRow rb{l.x, l.y};
    if (rb.Add(ui, "kbreset", Tr("kbd.reset"))) kbd.ResetPlace();
    l.y += 64;

    Section(ui, c, Tr("kbd.typingSection"));
    auto picker = [&](const char* id, const char* label, const std::string& value, const char* page) {
        Label(ui, c, Tr(label));
        ui.R().Text(value, c.x, c.y + 14, 24, kText, Face::SemiBold);
        const float bw = ButtonWidth(ui, Tr("kbd.change"));
        ui.NextLabel(std::string(Tr(label)) + ", " + value);
        if (ui.Button(id, {c.x + c.w - bw, c.y, bw, 54}, Tr("kbd.change"))) {
            app.ToggleWindow("keyboard", true);
            kbd.OpenPicker(page);
        }
        c.y += 66;
    };
    const kb::Lang& lang = data.Language(k.lang);
    picker("kblang", "kbd.language", lang.name, "lang");
    picker("kbfmt", "kbd.layout", k.fmt == "default" || !lang.reformat ? lang.native : data.FormatName(k.fmt), "fmt");
    changed |= CheckRow(ui, c, "kbwords", Tr("kbd.words"), k.words);
    changed |= CheckRow(ui, c, "kbpreview", Tr("kbd.preview"), k.preview);
    c.y += 8;
    Label(ui, c, Tr("kbd.hold"));
    int hold = k.hold == "shift" ? 1 : 0;
    if (RadioLine(ui, c, "kbhold", Tr("kbd.hold"), {Tr("kbd.holdRepeat"), Tr("kbd.holdShift")}, hold)) {
        k.hold = hold ? "shift" : "repeat";
        changed = true;
    }
    Label(ui, c, Tr("kbd.sound"));
    static const char* sounds[] = {"deep", "light", "off"};
    int sound = k.sound == "light" ? 1 : k.sound == "off" ? 2 : 0;
    if (RadioLine(ui, c, "kbsound", Tr("kbd.sound"), {Tr("kbd.soundDeep"), Tr("kbd.soundLight"), Tr("kbd.off")}, sound)) {
        k.sound = sounds[sound];
        changed = true;
    }
    if (changed) {
        kbd.ConfChanged();
        app.Settings().keyboard = k;
        app.SaveSettings();
    }
}

bool PageDesktop(Ui& ui, const Rect& body) {
    App& app = *g_app;
    HeadsetSettings& s = app.Settings();
    bool changed = false;
    const float gap = 44, w = (body.w - gap) / 2;
    const float distanceBefore = s.screenDistance;
    // The keyboard's settings made the page longer than it is: it scrolls, with the stick (see PageGeneral).
    static float contentBottom = 0;
    const float scroll = ui.BeginScroll("desktop", body, std::max(0.f, contentBottom - body.y), false);
    Column l{body.x, body.y - scroll, w};
    Section(ui, l, Tr("desktop.screensSection"));
    float curve = s.curvature;
    if (BarRow(ui, l, "curve", Tr("desktop.curve"), curve, 0, 1, 0.05f, s.curvature < 0.03f ? Tr("desktop.flat") : Percent(s.curvature))) {
        s.curvature = curve;
        changed = true;
    }
    l.y += 8;
    float dist = s.screenDistance;
    char hint[32];
    snprintf(hint, sizeof(hint), "%.2f m", s.screenDistance);
    if (BarRow(ui, l, "distance", Tr("desktop.distance"), dist, 0.8f, 3.f, 0.05f, hint)) {
        s.screenDistance = dist;
        changed = true;
    }
    l.y += 8;
    ButtonRow b{l.x, l.y};
    if (b.Add(ui, "arrange", Tr("btn.recenter"))) app.Recenter();
    l.y += 64;

    Column c{body.x + w + gap, body.y - scroll, w};
    Section(ui, c, Tr("desktop.qualitySection"));
    Label(ui, c, Tr("desktop.fpsLabel"));
    int fpsI = s.desktopFps == 0 ? 0 : s.desktopFps <= 30 ? 1 : s.desktopFps <= 60 ? 2 : 3;
    const std::vector<std::string> fps = {Tr("fps.auto"), "30", "60", "90"};
    if (RadioGrid(ui, c, "dfps", Tr("vr.fpsA11y"), fps, fpsI, 2)) {
        static const int v[] = {0, 30, 60, 90};
        s.desktopFps = v[fpsI];
        changed = true;
    }
    c.y += 8;
    changed |= CheckRow(ui, c, "sharp", Tr("desktop.sharpText"), s.sharpLayers);
    c.y += 8;
    changed |= CheckRow(ui, c, "desktop-input", Tr("desktop.input"), s.desktopInput);
    c.y += 8;
    changed |= CheckRow(ui, c, "use-pc-keyboard", Tr("desktop.usePcKeyboard"), s.usePcKeyboard);
    l.y = c.y = std::max(l.y, c.y);
    KeyboardSettings(ui, l, c);
    contentBottom = std::max(l.y, c.y) + scroll;
    ui.EndScroll();
    if (changed) app.UpdateScreenShape(distanceBefore);
    return changed;
}

bool PageEnvironment(Ui& ui, const Rect& body) {
    App& app = *g_app;
    HeadsetSettings& s = app.Settings();
    UiRenderer& R = ui.R();
    bool changed = false;
    const auto& envs = Environments();
    const float gap = 16, cardW = (body.w - 2 * gap) / 3, cardH = 186;
    int shown = 0;
    float y = body.y;
    for (const EnvironmentInfo& e : envs) {
        if (e.id == Environment::Passthrough && !app.Xr().PassthroughSupported()) continue;
        const Rect card{body.x + (shown % 3) * (cardW + gap), body.y + (shown / 3) * (cardH + gap), cardW, cardH};
        y = card.Bottom();
        shown++;
        const std::string id = std::string("env-") + e.key;
        const bool sel = s.environment == e.key;
        ui.NextLabel(Tr((std::string("env.") + e.key).c_str()));
        if (ui.Hit(id.c_str(), card)) {
            s.environment = e.key;
            changed = true;
        }
        const Rect pic{card.x, card.y, card.w, 146};
        const Texture& thumb = app.EnvironmentThumbnail(e.key);
        if (thumb.id) {
            R.Image(thumb.id, pic, kRadius);
            R.RectBorder(pic, Color(0, 0, 0, 0), kRadius, kLine, 2.f);
        } else {
            // Your own room: no picture to show. The tile stays plain.
            R.RectFill(pic, kControl, kRadius);
        }
        if (sel) {
            R.RectBorder(pic, Color(0, 0, 0, 0), kRadius, kAccent, 4.f);
            R.RectFill({pic.Right() - 44, pic.y + 12, 32, 32}, kAccent, 3);
            R.Icon("check", pic.Right() - 28, pic.y + 28, 28, kOnAccent);
        }
        ui.HoverLighten(pic, ui.IsHot(id.c_str()) ? 1.f : 0.f);
        R.TextIn(e.name, {card.x, pic.Bottom() + 2, card.w, 38}, 20, kText, sel ? Face::SemiBold : Face::Medium);
    }
    Column c{body.x, y + 14, cardW * 2 + gap};
    float b = s.skyBrightness;
    if (BarRow(ui, c, "brightness", Tr("env.brightness"), b, 0.3f, 1.6f, 0.05f, Percent(s.skyBrightness), true, 1.f)) {
        s.skyBrightness = b;
        changed = true;
    }
    return changed;
}

void DrawStickCard(Ui& ui, const Rect& card, const std::string& title, const HandInput& hand, float deadzone,
                   bool calibrating = false, float progress = 0.f) {
    UiRenderer& R = ui.R();
    constexpr float kTwoPi = 6.2831853f;
    R.RectBorder(card, Color::Hex(0x0E0E0E), kPanelRadius, kLine, 1.5f);

    const bool connected = hand.thumbstickActive && !hand.isHand;
    const Vec2 raw = connected && std::isfinite(hand.thumbstick.x) && std::isfinite(hand.thumbstick.y) ? hand.thumbstick : Vec2{};
    const Vec2 out = ApplyThumbstickDeadzone(raw, deadzone);
    const double rawMagnitude = StickMagnitude(raw);
    const float rawMag = static_cast<float>(std::min(rawMagnitude, 1.0));
    const float outMag = static_cast<float>(std::min(StickMagnitude(out), 1.0));
    const bool caughtDrift = connected && rawMag > 0.005f && outMag <= 1e-5f;
    const bool outputting = connected && outMag > 1e-5f;

    R.Text(title, card.x + 22, card.y + 16, 24, kText, Face::SemiBold);
    const std::string statusText = !connected ? Tr("ctrl.noStick") : outputting ? Tr("ctrl.active")
                                       : caughtDrift  ? Tr("ctrl.filtered")
                                                      : Tr("ctrl.centered");
    const Color statusColor = !connected ? kFaint : outputting ? kGood : caughtDrift ? kWarn : kMuted;
    R.Text(statusText, card.Right() - 22, card.y + 19, 19, statusColor, Face::SemiBold, Align::Right);

    constexpr float radius = 92.f;
    const float cx = card.x + card.w * 0.5f;
    const float cy = card.y + 154.f;
    R.Circle(cx, cy, radius, Color::Hex(0x161616));
    R.Arc(cx, cy, radius, 2.5f, 0.f, kTwoPi, connected ? kEdge : kLine);
    const Color axisColor = kLine.WithAlpha(0.55f);
    R.RectFill({cx - radius + 6.f, cy - 0.75f, (radius - 6.f) * 2.f, 1.5f}, axisColor);
    R.RectFill({cx - 0.75f, cy - radius + 6.f, 1.5f, (radius - 6.f) * 2.f}, axisColor);
    R.Circle(cx, cy, 3.f, kFaint);

    const float dz = ClampStickDeadzone(deadzone);
    if (dz > 0.001f) {
        const float dzRadius = dz * radius;
        R.Circle(cx, cy, dzRadius, kAccent.WithAlpha(0.18f));
        R.Arc(cx, cy, dzRadius, 2.2f, 0.f, kTwoPi, kAccent);
    }

    if (connected) {
        const float rawScale = rawMagnitude > 1.0 ? static_cast<float>(1.0 / rawMagnitude) : 1.f;
        const float rawX = cx + raw.x * rawScale * radius;
        const float rawY = cy - raw.y * rawScale * radius;
        if (outputting) {
            R.Circle(rawX, rawY, 5.f, kFaint.WithAlpha(0.55f));
            const float outX = cx + out.x * radius;
            const float outY = cy - out.y * radius;
            R.Circle(outX, outY, 8.5f, kGood);
            R.Circle(outX, outY, 3.2f, kText);
        } else if (caughtDrift) {
            R.Circle(rawX, rawY, 7.5f, kWarn);
            R.Circle(rawX, rawY, 2.8f, kPanel);
        } else {
            R.Circle(rawX, rawY, 5.5f, kMuted);
        }
    }

    char rawBuf[160];
    char outBuf[160];
    snprintf(rawBuf, sizeof(rawBuf), "%s  %+.2f, %+.2f (%d%%)", Tr("ctrl.raw"), raw.x, raw.y,
             int(std::round(rawMag * 100.f)));
    if (calibrating) snprintf(outBuf, sizeof(outBuf), "%s  %.0f%%", Tr("ctrl.calibrating"), progress * 100.f);
    else snprintf(outBuf, sizeof(outBuf), "%s  %+.2f, %+.2f", Tr("ctrl.output"), out.x, out.y);
    const float infoY = cy + radius + 16.f;
    R.Text(rawBuf, cx, infoY, 18, caughtDrift ? kWarn : kMuted, Face::Medium, Align::Center);
    R.Text(outBuf, cx, infoY + 24.f, 18, calibrating ? kAccent : outputting ? kGood : kFaint, Face::Medium, Align::Center);
}

void PageControllers(Ui& ui, const Rect& body) {
    App& app = *g_app;
    HeadsetSettings& s = app.Settings();
    UiRenderer& R = ui.R();
    R.Text(Tr("ctrl.title"), body.x, body.y, 28, kText, Face::SemiBold);
    R.WrapText(Tr("ctrl.subtitle"), {body.x, body.y + 40, body.w, 40}, 19, kMuted, Face::Medium, 26);

    const float gap = 24.f;
    const float cardW = (body.w - gap) * 0.5f;
    const Rect card{body.x, body.y + 80, cardW, 314};
    const HandInput& leftHand = app.Input().Hand(0);
    const HandInput& rightHand = app.Input().Hand(1);
    const bool calibrating = app.StickCalibrationActive();
    DrawStickCard(ui, card, Tr("ctrl.leftStick"), leftHand, s.leftStickDeadzone, app.StickCalibrationActive(0),
                  app.StickCalibrationProgress(0));
    DrawStickCard(ui, {card.x + cardW + gap, card.y, cardW, card.h}, Tr("ctrl.rightStick"), rightHand,
                  s.rightStickDeadzone, app.StickCalibrationActive(1), app.StickCalibrationProgress(1));

    Column c{body.x, card.Bottom() + 16, body.w, true};
    bool dirty = false;
    if (CheckRow(ui, c, "ctrl-link", Tr("ctrl.linkBoth"), s.linkStickDeadzone, {}, !calibrating)) {
        app.CancelStickCalibration();
        if (s.linkStickDeadzone) {
            const float linked = std::max(s.leftStickDeadzone, s.rightStickDeadzone);
            s.leftStickDeadzone = s.rightStickDeadzone = linked;
        }
        dirty = true;
    }
    c.y += 6;
    const bool leftAvailable = leftHand.thumbstickActive && !leftHand.isHand;
    const bool rightAvailable = rightHand.thumbstickActive && !rightHand.isHand;
    const bool canCalibrate = s.linkStickDeadzone ? leftAvailable && rightAvailable : leftAvailable || rightAvailable;
    ButtonRow b{c.x, c.y};
    if (b.Add(ui, "ctrl-auto", calibrating ? Tr("ctrl.calibrating") : Tr("ctrl.autoCalibrate"), ButtonStyle::Primary,
              !calibrating && canCalibrate)) {
        app.StartStickCalibration();
    }
    if (b.Add(ui, "ctrl-reset", Tr("ctrl.reset"), ButtonStyle::Quiet)) {
        app.CancelStickCalibration();
        s.leftStickDeadzone = s.rightStickDeadzone = 0.f;
        dirty = true;
    }
    c.y += 62;

    if (s.linkStickDeadzone) {
        float dz = s.leftStickDeadzone;
        if (BarRow(ui, c, "ctrl-dz-both", Tr("ctrl.deadzoneBoth"), dz, 0.f, kMaxStickDeadzone, 0.01f, Percent(dz),
                   !calibrating)) {
            app.CancelStickCalibration();
            s.leftStickDeadzone = s.rightStickDeadzone = dz;
            dirty = true;
        }
    } else {
        if (BarRow(ui, c, "ctrl-dz-left", Tr("ctrl.deadzoneLeft"), s.leftStickDeadzone, 0.f, kMaxStickDeadzone, 0.01f,
                   Percent(s.leftStickDeadzone), !calibrating)) {
            app.CancelStickCalibration();
            dirty = true;
        }
        if (BarRow(ui, c, "ctrl-dz-right", Tr("ctrl.deadzoneRight"), s.rightStickDeadzone, 0.f, kMaxStickDeadzone, 0.01f,
                   Percent(s.rightStickDeadzone), !calibrating)) {
            app.CancelStickCalibration();
            dirty = true;
        }
    }
    if (dirty) app.SaveSettings();
}

bool PageGeneral(Ui& ui, const Rect& body) {
    App& app = *g_app;
    HeadsetSettings& s = app.Settings();
    bool changed = false;
    const float gap = 44, w = (body.w - gap) / 2;
    // The page fit exactly until the language row arrived; a longer language scrolls now.
    // The content height comes from the frame before: layout only moves when the language
    // does. No drag to scroll: the trigger pull alone moves the ray past the drag
    // threshold, which would cancel every press on the page. Stick scrolling is enough.
    static float contentBottom = 0;
    const float scroll = ui.BeginScroll("general", body, std::max(0.f, contentBottom - body.y), false);
    Column l{body.x, body.y - scroll, w};
    Section(ui, l, Tr("general.audioSection"));
    changed |= CheckRow(ui, l, "audio", Tr("general.playSound"), s.streamAudio);
    l.y += 8;
    float v = s.volume;
    if (BarRow(ui, l, "volume", Tr("general.volume"), v, 0, 1, 0.05f, Percent(s.volume), s.streamAudio)) {
        s.volume = v;
        changed = true;
    }
    const bool micAllowed = Platform::HasMicPermission();
    // Without the permission the box shows off and can't be changed.
    bool mic = s.micPassthrough && micAllowed;
    // The connected PC reports which virtual microphone it routes to.
    std::string micName = "RinCast Microphone";
    const json& info = app.ServerInfo();
    if (info.contains("audioDriver") && info["audioDriver"].value("devices", "") == "steam")
        micName = "Steam Streaming Microphone";
    if (CheckRow(ui, l, "mic", Tr("general.useMic"), mic,
                 micAllowed ? Trf("general.micOnPc", {{"mic", micName}})
                            : Trf("general.micPermission", {{"where", std::string(app.Device().SettingsName())}}),
                 micAllowed)) {
        s.micPassthrough = mic;
        changed = true;
    }
    l.y += 8;
    float mv = s.micVolume;
    if (BarRow(ui, l, "micvolume", Tr("general.micVolume"), mv, 0, 2, 0.05f, Percent(s.micVolume), mic, 1.f)) {
        s.micVolume = mv;
        changed = true;
    }
    Section(ui, l, Tr("general.controllersSection"));
    changed |= CheckRow(ui, l, "haptics", Tr("general.haptics"), s.haptics);
    l.y += 8;
    float sp = s.scrollSpeed;
    char spHint[16];
    snprintf(spHint, sizeof(spHint), "%.1f×", s.scrollSpeed);
    if (BarRow(ui, l, "scroll", Tr("general.scrollSpeed"), sp, 0.3f, 3, 0.1f, spHint, true, 1.f)) {
        s.scrollSpeed = sp;
        changed = true;
    }
    // SteamVR's play space; on the VR quality page there's no room. (PICO always uses the
    // stage, where its trackers are.)
    if (!app.Device().PreferStageSpace()) {
        Section(ui, l, Tr("vr.tracking"));
        changed |= CheckRow(ui, l, "fixed-space", Tr("vr.fixedSpace"), s.fixedPlaySpace, Tr("vr.fixedSpaceHint"));
    }

    Column c{body.x + w + gap, body.y - scroll, w};
    Section(ui, c, Tr("general.connectingSection"));
    changed |= CheckRow(ui, c, "autoconnect", Tr("general.autoConnect"), s.autoConnect, Tr("general.autoConnectHint"));
    changed |= CheckRow(ui, c, "autosteamvr", Tr("general.autoSteamVr"), s.autoLaunchSteamVr, Tr("general.autoSteamVrHint"));
    {
        // The headset asks once whether RinCast may use the cable.
        using U = Platform::UsbNetwork;
        const U& u = app.UsbNetwork();
        const std::string hint = !s.usbCable ? Tr("general.usbHint.wifi")
                                 : u.state == U::Connected ? (u.kbps >= 1000000 ? Tr("general.usbHint.usb3") : Tr("general.usbHint.usb2"))
                                 : u.state == U::NotAllowed ? Tr("general.usbHint.notAllowed")
                                                            : Tr("general.usbHint.waiting");
        bool usb = s.usbCable;
        if (CheckRow(ui, c, "usbcable", Tr("general.usbCable"), usb, hint)) app.SetUsbCable(usb);
    }
    Section(ui, c, Tr("general.interfaceSection"));
    Label(ui, c, Tr("general.sizeLabel"));
    // Larger would no longer fit in view.
    static const std::vector<std::string> sizes = {"100%", "115%", "130%"};
    int sizeI = s.uiScale > 1.2f ? 2 : s.uiScale > 1.05f ? 1 : 0;
    if (RadioLine(ui, c, "uisize", Tr("general.sizeLabel"), sizes, sizeI)) {
        static const float sv[] = {1.f, 1.15f, 1.3f};
        s.uiScale = sv[sizeI];
        app.ApplyAccessibility();
        changed = true;
    }
    c.y += 4;
    if (CheckRow(ui, c, "contrast", Tr("general.highContrast"), s.highContrast)) {
        app.ApplyAccessibility();
        changed = true;
    }
    changed |= CheckRow(ui, c, "sounds", Tr("general.uiSounds"), s.uiSounds);
    changed |= CheckRow(ui, c, "music", Tr("general.music"), s.music, Tr("general.musicHint"));
    changed |= CheckRow(ui, c, "readaloud", Tr("general.readAloud"), s.readAloud, Tr("general.readAloudHint"));
    if (CheckRow(ui, c, "stats", Tr("general.perfOverlay"), s.showStats)) {
        app.ToggleWindow("stats", s.showStats && app.CurrentMode() != Mode::Home);
        changed = true;
    }
    if (CheckRow(ui, c, "compat-plain-xr", Tr("general.compatPlainXr"), s.compatPlainOpenXr,
                 Tr("general.compatPlainXrHint"))) {
        changed = true;
        app.PostNotice(Tr("general.compatPlainXrHint"));
    }
    c.y += 4;
    // The language, named in its own words so it's findable in any UI. What's selected is
    // the stored choice: with "auto", the radio stays on "auto" even though the resolved
    // language is a concrete one.
    Label(ui, c, Tr("general.language"));
    size_t langCount = 0;
    const auto* langs = LanguageChoices(langCount);
    std::vector<std::string> langNames;
    int langI = 0;
    for (size_t i = 0; i < langCount; i++) {
        langNames.push_back(langs[i].name);
        if (s.language == langs[i].code) langI = int(i) + 1;
    }
    // Index 0 is "auto", which follows the headset's system language.
    langNames.insert(langNames.begin(), Tr("language.auto"));
    if (RadioLine(ui, c, "language", Tr("general.language"), langNames, langI)) {
        s.language = langI == 0 ? "auto" : langs[size_t(langI) - 1].code;
        SetLanguage(s.language, Platform::SystemLocale());
        app.SaveSettings();
        changed = true;
    }
    // Hand the measured content height to the next frame, for the scroll range.
    // The columns start scrolled, so their end positions come back up by the offset.
    contentBottom = std::max(l.y, c.y) + scroll;
    ui.EndScroll();
    return changed;
}

void PageAccount(Ui& ui, const Rect& body) {
    App& app = *g_app;
    HeadsetSettings& s = app.Settings();
    UiRenderer& R = ui.R();
    const auto st = app.Account().State();
    switch (st) {
        case Cloud::SignInState::SignedOut:
        case Cloud::SignInState::Error: {
            float y = Message(ui, body, Tr("account.reachTitle"), Tr("account.reachHint"));
            if (st == Cloud::SignInState::Error) y += R.WrapText(app.Account().Error(), {body.x, y, std::min(body.w, 680.f), 90}, 21, kDanger, Face::Medium, 30) + 14;
            ButtonRow b{body.x, y};
            if (b.Add(ui, "getcode", Tr("btn.signIn"), ButtonStyle::Primary)) app.Account().BeginSignIn();
            break;
        }
        case Cloud::SignInState::RequestingCode:
            Message(ui, body, Tr("account.gettingCode"), {}, kText, true);
            break;
        case Cloud::SignInState::WaitingForApproval: {
            const std::string code = app.Account().UserCode();
            static std::string announced;
            if (code != announced) {
                announced = code;
                std::string spaced;
                for (char c : code) if (c != '-') { spaced += c; spaced += ' '; }
                ui.Announce(Trf("account.codeAnnounce", {{"code", spaced}}));
            }
            R.Text(Tr("account.enterCode"), body.x, body.y, 28, kText, Face::SemiBold);
            R.Text(code, body.x, body.y + 54, 70, kText, Face::SemiBold);
            std::string url = app.Account().VerificationUrl();
            const size_t scheme = url.find("://");
            if (scheme != std::string::npos) url = url.substr(scheme + 3);
            const float top = body.y + 158;
            const float h = R.WrapText(Trf("account.goTo", {{"url", url}}),
                                       {body.x, top, std::min(body.w, 680.f), 90}, 21, kMuted, Face::Medium, 30);
            const float y = top + h + 20;
            R.Text(Tr("account.waitingConfirm"), body.x, y, 21, kText);
            ui.Spinner(body.x + R.Measure(Tr("account.waitingConfirm"), 21, Face::Medium) + 24, y + 13, 12, kText);
            ButtonRow b{body.x, y + 46};
            if (b.Add(ui, "cancel-signin", Tr("btn.cancel"))) app.Account().CancelSignIn();
            break;
        }
        case Cloud::SignInState::SignedIn: {
            const float y = Message(ui, body, Trf("account.signedInAs", {{"name", app.Account().UserName()}}));
            static float signOutArmed = -100;
            const bool armed = ui.Time() - signOutArmed < 4.f;
            ButtonRow b{body.x, y};
            if (b.Add(ui, "signout", armed ? Tr("btn.signOutAgain") : Tr("btn.signOut"), armed ? ButtonStyle::Danger : ButtonStyle::Secondary)) {
                if (armed) app.Account().SignOut();
                else signOutArmed = ui.Time();
            }
            break;
        }
    }
    if (s.cloudServer != HeadsetSettings{}.cloudServer) R.TextIn(Trf("account.serverLine", {{"server", s.cloudServer}}), {body.x, body.Bottom() - 30, body.w, 30}, 18, kMuted);
}

void PageHelp(Ui& ui, const Rect& body) {
    App& app = *g_app;
    UiRenderer& R = ui.R();
    Column l{body.x, body.y, body.w};
    Section(ui, l, Tr("help.section"));
    struct Line {
        const char* input;
        const char* action;
    };
    static const Line lines[] = {{"help.input1", "help.action1"},
                                 {"help.input2", "help.action2"},
                                 {"help.input3", "help.action3"},
                                 {"help.input4", "help.action4"},
                                 {"help.input5", "help.action5"},
                                 {"help.input6", "help.action6"},
                                 {"help.input7", "help.action7"},
                                 {"help.input8", "help.action8"}};
    for (const Line& line : lines) {
        ui.Divider(l.x, l.y, l.w);
        R.Text(Tr(line.action), l.x, l.y + 10, 21, kText);
        const float h = R.WrapText(Tr(line.input), {l.x + 270, l.y + 10, l.w - 270, 60}, 21, kMuted, Face::Medium, 28);
        l.y += h + 19;
    }
    ui.Divider(l.x, l.y, l.w);
    l.y += 6;
    Section(ui, l, std::string("RinCast ") + kVersion);
    const json& info = app.ServerInfo();
    if (!info.empty()) {
        R.Text(info.value("pcName", "") + ": Streamer " + info.value("version", "") + ", " + info.value("os", "") + ", " + info.value("gpu", ""), l.x, l.y, 20, kMuted);
        l.y += 34;
    }
    l.y += R.WrapText(Tr("help.license"),
                      {l.x, l.y, l.w, 90}, 20, kMuted, Face::Medium, 28) + 12;
    // CC-BY: title, author, license and where to find them, for each track.
    for (const MusicTrack& t : kMusicTracks) {
        R.Text(std::string("'") + t.title + "' by Scott Buckley - released under CC-BY 4.0. www.scottbuckley.com.au", l.x, l.y, 18, kMuted);
        l.y += 26;
    }
}

void BuildMain(Ui& ui, const Rect& r) {
    App& app = *g_app;
    UiRenderer& R = ui.R();
    Background(ui, r);
    const bool connected = Connected(app);
    const float navW = 248, statusH = connected ? 82 : 2;
    const float statusY = r.Bottom() - statusH;
    R.RectFill({r.x + navW, r.y + 2, 2, statusY - r.y - 2}, kLine);

    Logo(ui, r.x + 44, r.y + 44, 44);
    R.Text("RinCast", r.x + 76, r.y + 28, 27, kText, Face::SemiBold);

    static const char* names[kPageCount] = {"nav.pcs", "nav.games", "nav.vrQuality", "nav.desktop",
                                            "nav.environment", "nav.controllers", "nav.general", "nav.account", "nav.help"};
    float ny = r.y + 84;
    for (int i = 0; i < kPageCount; i++) {
        const Rect row{r.x + 12, ny, navW - 24, 52};
        const std::string id = std::string("nav-") + names[i];
        ui.NextLabel(Tr(names[i]));
        if (ui.Row(id.c_str(), row, g_page == i)) ShowPage(app, i);
        const Color fg = g_page == i || ui.IsHot(id.c_str()) ? kText : kMuted;
        R.TextIn(Tr(names[i]), {row.x + 16, row.y, row.w - 24, row.h}, 22, fg, g_page == i ? Face::SemiBold : Face::Medium);
        ny += 54;
    }
    if (connected) {
        R.RectFill({r.x + 12, statusY - 14 - 54 - 12, navW - 24, 2}, kLine);
        if (ui.Button("nav-close", {r.x + 12, statusY - 14 - 54, navW - 24, 54}, Tr("btn.hideMenu"), nullptr, ButtonStyle::Quiet)) app.ToggleWindow("home", false);
        // Live numbers along the bottom, on every page.
        R.RectFill({r.x + 2, statusY, r.w - 4, 2}, kLine);
        const auto st = app.CurrentStats();
        const double latencyEma = app.LatencyDisplayMs();  // A22: the panels show the smoothed value
        const float sx = r.Right() - 36;
        if (g_page != kComputers) Stat(ui, r.x + 28, statusY + 14, Tr("stat.connectedTo"), app.Client().ServerName());
        Stat(ui, sx, statusY + 14, Tr("stat.bitrate"), st.mbps > 0 ? FormatMs(st.mbps) + " Mbps" : "–", Align::Right);
        Stat(ui, sx - 180, statusY + 14, Tr("stat.fps"), st.fps > 0 ? FormatMs(st.fps) + " fps" : "–", Align::Right, app.CurrentMode() == Mode::Vr && PoorFrameRate(st, app.Settings().refreshRate));
        Stat(ui, sx - 360, statusY + 14, Tr("stat.latency"), latencyEma >= 0 ? FormatMs(latencyEma) + " ms" : "–", Align::Right, PoorLatency(latencyEma));
    }

    // A lost connection, a retry or a failure is news on any page.
    static bool wasTrouble = false;
    const bool trouble = app.Reconnecting() || !app.ConnectionProblem().empty();
    if (trouble && !wasTrouble) g_page = kComputers;
    wasTrouble = trouble;

    const Rect body{r.x + navW + 36, r.y + 30, r.w - navW - 72, statusY - r.y - 54};
    bool changed = false;
    switch (g_page) {
        case kComputers: PageComputers(ui, body); break;
        case kGames: PageGames(ui, body); break;
        case kVrGames: changed = PageVrQuality(ui, body); break;
        case kDesktop: changed = PageDesktop(ui, body); break;
        case kEnvironment: changed = PageEnvironment(ui, body); break;
        case kControllers: PageControllers(ui, body); break;
        case kGeneral: changed = PageGeneral(ui, body); break;
        case kAccount: PageAccount(ui, body); break;
        case kHelp: PageHelp(ui, body); break;
    }
    if (changed) app.ApplyStreamSettings();
}

// ---- Dock ---------------------------------------------------------------------

void BuildDock(Ui& ui, const Rect& r) {
    App& app = *g_app;
    UiRenderer& R = ui.R();
    Background(ui, r);
    // Connection summary.
    const double latencyEma = app.LatencyDisplayMs();  // A22: the panels show the smoothed value
    const bool slow = latencyEma >= 80;  // the same line the PC draws
    R.TextIn(app.Client().ServerName(), {r.x + 24, r.y + 18, 176, 36}, 21, kText, Face::SemiBold);
    const std::string sub = latencyEma >= 0 ? Trf("dock.latency", {{"ms", FormatMs(latencyEma)}}) + (slow ? Tr("net.slowSuffix") : "") : app.Client().IsVpn() ? Tr("pcDetail.vpn") : app.Client().IsRemote() ? Tr("net.overInternet") : Tr("net.connected");
    R.TextIn(sub, {r.x + 24, r.y + 54, 176, 30}, 19, slow ? kWarn : kMuted);
    R.RectFill({r.x + 212, r.y + 14, 2, r.h - 28}, kLine);

    struct Btn {
        const char* id;
        const char* icon;
        const char* label;
    };
    // The labels are i18n keys; "SteamVR" isn't one and Tr passes it through.
    const Btn btns[] = {{"monitors", "desktop_windows", "nav.desktop"}, {"games", "sports_esports", "nav.games"},
                        {"keyboard", "keyboard", "dock.keyboard"}, {"menu", "apps", "dock.menu"},
                        {"steamvr", "rocket_launch", app.VrRunning() ? "btn.backToVr" : "dock.steamvr"}, {"recenter", "center_focus_strong", "btn.recenter"},
                        {"disconnect", "logout", "btn.disconnect"}};
    const float bw = 100, gap = 4;
    float x = r.x + 224;
    int index = 0;
    for (const auto& b : btns) {
        // Toggles, then one-shot actions, then the way out.
        if (index == 4 || index == 6) {
            R.RectFill({x + 4, r.y + 14, 2, r.h - 28}, kLine);
            x += 14;
        }
        index++;
        const Rect br{x, r.y + 8, bw, r.h - 16};
        const std::string id = b.id;
        const bool panel = app.IsVisible("home");
        bool active = false;
        if (id == "monitors") active = app.MonitorsVisible();
        if (id == "games") active = panel && g_page == kGames;
        if (id == "keyboard") active = app.IsVisible("keyboard");
        if (id == "menu") active = panel;
        // Disconnecting takes a second press within a few seconds: it ends what you're doing.
        const bool armed = id == "disconnect" && ui.Time() - g_disconnectArmed < 4.f;
        if (armed) R.RectFill(br, kDangerFill, kRadius);
        if (ui.IconButton(b.id, br, armed ? "warning" : b.icon, active, armed ? Tr("btn.pressAgain") : Tr(b.label))) {
            if (id == "monitors") app.ShowMonitors(!app.MonitorsVisible());
            if (id == "games") {
                if (panel && g_page == kGames) {
                    app.ToggleWindow("home", false);
                } else {
                    ShowPage(app, kGames);
                    app.ToggleWindow("home", true);
                }
            }
            if (id == "steamvr") app.LaunchSteamVr();
            if (id == "keyboard") {
                if (app.Settings().usePcKeyboard && app.Client().GetState() == StreamClient::State::Connected) app.TogglePcKeyboard();
                else app.ToggleWindow("keyboard");
            }
            if (id == "recenter") app.Recenter();
            // Back to wherever you were.
            if (id == "menu") {
                if (!panel && g_page == kGames) app.RequestGames();
                app.ToggleWindow("home", !panel);
            }
            if (id == "disconnect") {
                if (armed) app.Disconnect();
                else g_disconnectArmed = ui.Time();
            }
        }
        x += bw + gap;
    }
}


// ---- Stats ---------------------------------------------------------------------------

// The row reports actual output in the last stats window, not the pace policy's intent.
std::string FrameGenStatus(const App::Stats& s) {
    if (s.generatedFps > 0) {
        std::string v = Trf("stat.frameGen.active", {{"fps", FormatMs(s.generatedFps)}});
        if (s.generatedRejectedFps > 0)
            v += Trf("stat.frameGen.rejected", {{"fps", FormatMs(s.generatedRejectedFps)}});
        return v;
    }
    if (s.generatedRejectedFps > 0)
        return Trf("stat.frameGen.rejectedOnly", {{"fps", FormatMs(s.generatedRejectedFps)}});
    return Tr("stat.frameGen.standby");
}

void BuildStats(Ui& ui, const Rect& r) {
    App& app = *g_app;
    UiRenderer& R = ui.R();
    Background(ui, r);
    const auto s = app.CurrentStats();
    const double latencyEma = app.LatencyDisplayMs();  // A22: the panels show the smoothed value
    struct Item {
        const char* label;
        std::string value;
    };
    std::string fps = s.fps > 0 ? FormatMs(s.fps) + " fps" : "–";
    std::string frameGen = Tr("stat.frameGen.off");
    if (app.Settings().frameGeneration) {
        frameGen = app.CurrentMode() == Mode::Vr ? FrameGenStatus(s) : "–";
        if (app.CurrentMode() == Mode::Vr && (s.fps > 0 || s.generatedFps > 0))
            fps = Trf("stat.fpsBreakdown", {{"source", FormatMs(s.fps)},
                                            {"total", FormatMs(s.fps + s.generatedFps)}});
    }
    const Item items[] = {{Tr("stat.latency.total"), latencyEma >= 0 ? FormatMs(latencyEma) + " ms" : "–"},
                          {Tr("stat.fps"), fps},
                          {Tr("stat.frameGen"), frameGen},
                          {Tr("stat.bitrate"), s.mbps > 0 ? FormatMs(s.mbps) + " Mbps" : "–"},
                          {Tr("stat.decoding"), s.decodeMs > 0 ? FormatMs(s.decodeMs) + " ms" : "–"},
                          {Tr("stat.network"), s.rttMs > 0 ? FormatMs(s.rttMs) + " ms" : "–"},
                          {Tr("stat.video"), s.codec.empty() ? "–" : s.codec + " " + PixelSize(s.width, s.height)}};
    for (int i = 0; i < 7; i++) {
        const float y = r.y + 16 + i * 38;
        R.Text(items[i].label, r.x + 22, y, 20, kMuted);
        const bool poor = (i == 0 && PoorLatency(latencyEma)) ||
                          (i == 1 && app.CurrentMode() == Mode::Vr && PoorFrameRate(s, app.Settings().refreshRate));
        R.Text(items[i].value, r.Right() - 22, y, 20, poor ? kWarn : kText, Face::SemiBold, Align::Right);
    }
    // The latency, stage by stage. Bars are shares of the whole; the biggest stands out.
    double total = 0, biggest = 0;
    for (double ms : s.stageMs) {
        total += std::max(ms, 0.0);
        biggest = std::max(biggest, ms);
    }
    // One more row than before the frame-generation line, so the divider moved down with it.
    float y = r.y + 16 + 7 * 38 + 6;
    ui.Divider(r.x + 22, y, r.w - 44);
    y += 16;
    if (total <= 0) {
        R.Text(Tr("stats.stagesWaiting"), r.x + 22, y, 18, kMuted);
        return;
    }
    R.Text(Tr("stats.stages"), r.x + 22, y, 20, kText, Face::SemiBold);
    y += 36;
    for (int i = 0; i < kStageCount; i++) {
        const double ms = s.stageMs[i];
        if (ms < 0) continue;
        R.Text(Tr((std::string("stage.") + kLatencyStages[i].key).c_str()), r.x + 22, y, 18, kMuted);
        R.Text(FormatMs(ms) + " ms", r.Right() - 22, y, 18, kText, Face::SemiBold, Align::Right);
        const float w = float((r.w - 44) * std::max(ms, 0.0) / total);
        R.RectFill({r.x + 22, y + 25, std::max(w, 2.f), 4}, ms == biggest ? kAccent : kEdge, 2);
        y += 40;
    }
    y += 8;
    R.Text(app.CompatibilityDiagnostic(), r.x + 22, y, 14, kMuted);
}

// ---- Notices ---------------------------------------------------------------------------

void BuildNotices(Ui& ui, const Rect& r) {
    App& app = *g_app;
    UiRenderer& R = ui.R();
    if (app.Notices().empty()) return;
    const Notice& n = app.Notices().back();
    const Color c = n.level == "error" ? kDanger : n.level == "warning" ? kWarn : kText;
    const float fade = std::min(1.f, (n.life - n.age) * 2.f);
    R.RectBorder(r.Inset(1), kPanel.WithAlpha(fade), kPanelRadius, (n.level == "info" ? kPanelBorder : c).WithAlpha(fade), 2.f);
    R.Icon(n.level == "error" ? "error" : n.level == "warning" ? "warning" : "info", r.x + 42, r.y + r.h / 2, 32, c.WithAlpha(fade));
    // Something to do: the action, and a way to say no to it.
    const bool actions = !n.action.empty();
    const bool stays = actions || n.level == "error";
    const Rect text{r.x + 76, r.y, r.w - (actions ? 390 : stays ? 240 : 100), r.h};
    const float h = WrapHeight(ui, n.text, text.w, 20, 27);
    R.WrapText(n.text, {text.x, r.y + (r.h - h) / 2, text.w, h}, 20, kText.WithAlpha(fade), Face::Medium, 27);
    if (stays && !actions && ui.Button("dismiss", {r.Right() - 134, r.y + 22, 114, r.h - 44}, Tr("btn.dismiss"))) app.DismissNotice();
    if (actions) {
        const std::string label = std::string(n.action == "vr.retry" ? Tr("btn.tryAgain") : n.action.find("estart") != std::string::npos ? Tr("btn.restart") : Tr("btn.open"));
        const std::string action = n.action;
        if (ui.Button("dismiss", {r.Right() - 170 - 8 - 114, r.y + 22, 114, r.h - 44}, Tr("btn.notNow"))) app.DismissNotice();
        else if (ui.Button("action", {r.Right() - 170, r.y + 22, 150, r.h - 44}, label, nullptr, label == Tr("btn.restart") ? ButtonStyle::Danger : ButtonStyle::Primary)) {
            app.DismissNotice();
            app.RunAction(action);
        }
    }
}

// ---- VR launch ---------------------------------------------------------------------------

void BuildLaunch(Ui& ui, const Rect& r) {
    App& app = *g_app;
    UiRenderer& R = ui.R();
    Background(ui, r);
    ui.Spinner(r.x + 44, r.y + r.h / 2, 20, kText);
    R.TextIn(app.VrStateText(), {r.x + 80, r.y + 18, r.w - 320, 40}, 26, kText, Face::SemiBold);
    R.TextIn(Tr("launch.takeTime"), {r.x + 80, r.y + 58, r.w - 320, 30}, 20, kMuted);
    // SteamVR keeps starting on the PC; this only stops waiting for it here.
    if (ui.Button("launch-cancel", {r.Right() - 24 - 200, r.y + (r.h - 54) / 2, 200, 54}, Tr("btn.backToDesktop"))) app.ExitVr(true);
}

// ---- VR overlay ---------------------------------------------------------------------------

void BuildVrOverlay(Ui& ui, const Rect& r) {
    App& app = *g_app;
    UiRenderer& R = ui.R();
    Background(ui, r);
    const float pad = 24;
    R.TextIn(app.VrStateText(), {r.x + pad, r.y + 14, r.w - 400, 54}, 30, kText, Face::SemiBold);
    // The same menu opens on the desktop while SteamVR runs (double-press the menu button),
    // with the way back to VR in place of the way out of it.
    const bool inVr = app.CurrentMode() == Mode::Vr;
    const std::string close = inVr ? Tr("btn.resume") : Tr("btn.hideMenu");
    const float cw = std::max(120.f, ButtonWidth(ui, close));
    const double latencyEma = app.LatencyDisplayMs();  // A22: the panels show the smoothed value
    if (latencyEma >= 0) R.TextIn(Trf("dock.latency", {{"ms", FormatMs(latencyEma)}}), {r.Right() - pad - cw - 246, r.y + 14, 230, 54}, 21, kMuted, Face::Medium, Align::Right);
    if (ui.Button("close", {r.Right() - pad - cw, r.y + 14, cw, 54}, close, nullptr, ButtonStyle::Primary))
        app.ToggleWindow("vr-overlay", false);
    // Left-aligned with the title, in the order you're likely to need them.
    ButtonRow b{r.x + pad, r.y + 88};
    if (inVr ? b.Add(ui, "desktop", Tr("btn.backToDesktop")) : b.Add(ui, "back-to-vr", app.VrRunning() ? Tr("btn.backToVr") : Tr("btn.startSteamvr"))) {
        if (inVr) app.ExitVr(true);
        else app.LaunchSteamVr();
    }
    if (b.Add(ui, "recenter", Tr("btn.recenter"))) app.Recenter();
    if (b.Add(ui, "stats", app.Settings().showStats ? Tr("vrOverlay.hidePerf") : Tr("vrOverlay.showPerf"))) {
        app.Settings().showStats = !app.Settings().showStats;
        app.SaveSettings();
        app.ToggleWindow("stats", app.Settings().showStats);
    }
    const bool armed = ui.Time() - g_disconnectArmed < 4.f;
    const Rect dis{r.Right() - pad - 180, r.y + 88, 180, 54};
    if (ui.Button("disconnect", dis, armed ? Tr("btn.pressAgain") : Tr("btn.disconnect"), nullptr, armed ? ButtonStyle::Danger : ButtonStyle::Secondary)) {
        if (armed) app.Disconnect();
        else g_disconnectArmed = ui.Time();
    }
}

}  // namespace

void SetPage(int page) { g_page = std::clamp(page, 0, kPageCount - 1); }

void ToggleMenu(App& app) {
    // Before connecting the panel always stays up, so there's nothing to show or hide.
    if (app.CurrentMode() != Mode::Home && app.CurrentMode() != Mode::Connecting) app.ToggleWindow("home");
}

void Build(App& app, WindowManager& wm) {
    g_app = &app;
    auto make = [&](const char* id, int w, int h, float widthM, bool resizable,
                    std::function<void(Ui&, const Rect&)> build) {
        Window* win = wm.Create(id, WindowKind::Ui);
        win->canvasW = w;
        win->canvasH = h;
        win->widthM = widthM;
        win->aspect = float(h) / float(w);
        win->resizable = resizable;
        win->minWidthM = widthM * 0.6f;
        win->maxWidthM = widthM * 2.5f;
        win->build = std::move(build);
        return win;
    };
    make("home", 1240, 860, 1.34f, true, BuildMain)->visible = true;
    Window* dock = make("dock", 1000, 112, 0.84f, false, BuildDock);
    dock->showChrome = true;
    // The keyboard (keyboard/keyboard.h) sizes, draws and points for itself: both hands at once,
    // its own handles, and the clear space around it lets the ray through.
    Window* kb = make("keyboard", 1600, 700, 0.6f, true, nullptr);
    kb->build = [kb](Ui& ui, const Rect&) { g_app->Kb().Build(ui.R(), *kb, ui.Time()); };
    kb->zoneAt = [](Vec2 p) { return g_app->Kb().ZoneAt(p); };
    kb->twoHands = true;
    kb->showChrome = false;
    // 640 (was 600): the frame-generation row (TASK-OPLOGIC-FIX-HMD-2026-11-22 §1.1) added a
    // 38 px row, and the stage list below already ended at the old bottom edge.
    Window* stats = make("stats", 380, 756, 0.30f, false, BuildStats);
    stats->showChrome = true;
    Window* notices = make("notices", 800, 104, 0.68f, false, BuildNotices);
    notices->showChrome = false;
    notices->movable = false;
    Window* launch = make("launch", 760, 108, 0.65f, false, BuildLaunch);
    launch->showChrome = false;
    launch->movable = false;
    make("vr-overlay", 960, 166, 0.94f, false, BuildVrOverlay);
}

}  // namespace rv::screens
