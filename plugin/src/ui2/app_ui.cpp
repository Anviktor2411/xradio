#include "app_ui.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "imgui.h"
#include "i18n.h"
#include "theme.h"

namespace xr {
namespace ui2 {

namespace {
UiState g_st;
bool g_overlayWantsMouse = false;

const char* kTabs[8] = {"Radio", "ATC", "Pilots", "Messages", "Flight plan", "Host / Connect", "Settings", "Tower"};

void pushFont(void* f) { if (f) ImGui::PushFont((ImFont*)f); }
void popFont(void* f) { if (f) ImGui::PopFont(); }

void setNote(const std::string& s, double now) { g_st.note = s; g_st.noteAt = now; }

void muted(const char* s) { ImGui::TextColored(palette().muted, "%s", s); }
void heading(const char* s) { pushFont(g_st.fonts.heading); ImGui::TextUnformatted(s); popFont(g_st.fonts.heading); }

// ---------------------------------------------------------------- Radio
void drawCom(Model& m, const Actions& a, int i) {
    Com& c = m.com[i];
    const Palette& p = palette();
    char id[16]; snprintf(id, sizeof id, "com%d", i + 1);
    ImGui::PushID(id);
    ImGui::BeginChild("box", ImVec2(0, 0), ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_Borders);
    char title[16]; snprintf(title, sizeof title, "COM%d", i + 1);
    heading(title);
    ImGui::SameLine();
    light(p.rx, c.rx); ImGui::SameLine(); muted("RX"); ImGui::SameLine();
    light(p.tx, c.tx); ImGui::SameLine(); muted("TX");
    if (!c.powered) { ImGui::SameLine(); ImGui::TextColored(p.warn, "(%s)", tr("no power")); }
    // active
    pushFont(g_st.fonts.big);
    ImGui::TextColored(c.powered ? p.text : p.muted, "%s", fmtKhz(c.activeKhz).c_str());
    popFont(g_st.fonts.big);
    ImGui::SameLine();
    ImGui::BeginGroup();
    muted(tr("Active"));
    if (!c.stationName.empty()) ImGui::TextColored(p.atc, "%s", c.stationName.c_str()); else muted("-");
    ImGui::EndGroup();
    if (c.rx) { ImGui::SameLine(); ImGui::TextUnformatted(tr("Signal")); ImGui::SameLine(); signalBarsWidget(signalBars(c.signal), 5, p.rx); }
    // standby with the swap in between, wheel tunes
    ImGui::TextColored(p.muted, "%s", tr("Standby"));
    ImGui::SameLine();
    pushFont(g_st.fonts.heading);
    ImGui::TextUnformatted(fmtKhz(c.standbyKhz).c_str());
    popFont(g_st.fonts.heading);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", "scroll: 25 kHz, shift+scroll: 1 MHz");
        const float w = ImGui::GetIO().MouseWheel;
        if (w != 0.f && a.setStandby) {
            const int step = ImGui::GetIO().KeyShift ? 1000 : 25;
            long v = (long)c.standbyKhz + (w > 0 ? step : -step);
            v = std::max(118000L, std::min(136975L, v));
            a.setStandby(i + 1, (uint32_t)v);
        }
    }
    ImGui::SameLine();
    if (ImGui::Button(tr("Swap")) && a.swap) a.swap(i + 1);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(90);
    if (ImGui::InputTextWithHint("##f", "118.000", g_st.freqInput[i], sizeof g_st.freqInput[i], ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CharsDecimal | ImGuiInputTextFlags_AutoSelectAll)) {
        const uint32_t k = parseFreq(g_st.freqInput[i]);
        if (k && a.setStandby) { a.setStandby(i + 1, k); g_st.freqInput[i][0] = 0; }
        else setNote("not a COM frequency (118.000-136.975)", m.elapsed);
    }
    ImGui::SameLine(); muted("Enter = standby");
    ImGui::SetNextItemWidth(160);
    float vol100 = c.volume * 100.f;
    if (ImGui::SliderFloat(tr("Volume"), &vol100, 0.f, 100.f, "%.0f %%") && a.setVolume) a.setVolume(i + 1, vol100 / 100.f);
    ImGui::EndChild();
    ImGui::PopID();
}

void drawRadio(Model& m, const Actions& a) {
    const Palette& p = palette();
    drawCom(m, a, 0);
    drawCom(m, a, 1);
    bool guard = m.guard;
    if (ImGui::Checkbox(tr("Guard 121.500 on COM2"), &guard) && a.setGuard) a.setGuard(guard);
    ImGui::Separator();
    // the big push-to-talk indicator
    ImGui::BeginChild("ptt", ImVec2(0, 0), ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_Borders);
    const bool someone = !m.txWho.empty();
    light(m.pttDown ? p.tx : (someone ? p.rx : p.muted), m.pttDown || someone, 9.f);
    ImGui::SameLine();
    pushFont(g_st.fonts.heading);
    if (m.pttDown) ImGui::TextColored(p.tx, "%s", tr("You are transmitting"));
    else if (someone) ImGui::TextColored(p.rx, "%s %s", m.txWho.c_str(), tr("is transmitting"));
    else muted(tr("Nobody is transmitting"));
    popFont(g_st.fonts.heading);
    if (m.pttDown) {
        ImGui::SameLine(); meter(m.voice.micLevel, p.tx, 140.f);
        const std::string stuck = stuckMicText(m.elapsed - m.pttDownAt, 30.0);
        if (!stuck.empty()) ImGui::TextColored(p.warn, "%s", stuck.c_str());
    }
    const char* pttLabel = tr("Push-to-talk");
    if (ImGui::Button(pttLabel, ImVec2(160, 0))) {}
    if (ImGui::IsItemActivated() && a.keyPtt) a.keyPtt(true);
    if (ImGui::IsItemDeactivated() && a.keyPtt) a.keyPtt(false);
    ImGui::SameLine();
    char keyTxt[64]; snprintf(keyTxt, sizeof keyTxt, "%s: %s", tr("PTT key"), m.settings.pttKey > 0 ? keyName(m.settings.pttKey).c_str() : tr("none"));
    muted(keyTxt);
    ImGui::SameLine(); muted(m.voice.status.c_str());
    ImGui::EndChild();
    if (!m.conn.skyLine.empty()) { ImGui::TextColored(p.muted, "%s: ", tr("Weather and time")); ImGui::SameLine(); ImGui::TextUnformatted(m.conn.skyLine.c_str()); }
}

// ---------------------------------------------------------------- ATC
void drawAtc(Model& m, const Actions& a) {
    const Palette& p = palette();
    heading(tr("Online controllers"));
    ImGui::SameLine(); muted(tr("click to tune COM1, right-click COM2"));
    const auto list = sortedStations(m.atc);
    if (list.empty()) { muted(m.conn.connected ? tr("No controllers online") : tr("not connected")); }
    else if (ImGui::BeginTable("atc", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn(tr("Callsign"), ImGuiTableColumnFlags_WidthStretch, 2.f);
        ImGui::TableSetupColumn("Facility", ImGuiTableColumnFlags_WidthStretch, 2.f);
        ImGui::TableSetupColumn(tr("frequency"), ImGuiTableColumnFlags_WidthStretch, 1.5f);
        ImGui::TableSetupColumn(tr("distance"), ImGuiTableColumnFlags_WidthStretch, 1.f);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthStretch, 1.f);
        ImGui::TableHeadersRow();
        for (const Station* s : list) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::PushID(s->callsign.c_str());
            const bool clicked = ImGui::Selectable(s->callsign.c_str(), s->tuned, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap);
            const bool right = ImGui::IsItemClicked(ImGuiMouseButton_Right);
            if (right && a.tune && s->freqKhz) a.tune(2, s->freqKhz);
            else if (clicked && a.tune && s->freqKhz) a.tune(s->atis ? 2 : 1, s->freqKhz);
            ImGui::TableNextColumn(); ImGui::TextUnformatted(s->kindName.c_str());
            ImGui::TableNextColumn(); ImGui::TextColored(s->tuned ? p.ok : p.text, "%s", fmtKhz(s->freqKhz).c_str());
            ImGui::TableNextColumn(); if (s->distNm >= 0) ImGui::Text("%.1f nm", s->distNm); else muted("-");
            ImGui::TableNextColumn();
            if (s->ai) { badge("AI", p.ai); ImGui::SameLine(); }
            if (s->atis) { badge("ATIS", p.warn); ImGui::SameLine(); }
            if (s->tuned) badge("tuned", p.ok);
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    if (!m.atis.empty()) {
        ImGui::Separator();
        for (const auto& kv : m.atis) {
            heading(kv.first.c_str());
            ImGui::TextWrapped("%s", kv.second.c_str());
        }
    }
}

// ---------------------------------------------------------------- Pilots
void drawPilots(Model& m, const Actions& a) {
    (void)a;
    const Palette& p = palette();
    ImGui::SetNextItemWidth(160);
    ImGui::InputTextWithHint("##flt", tr("Filter"), g_st.pilotFilter, sizeof g_st.pilotFilter);
    ImGui::SameLine();
    ImGui::Checkbox(tr("Hide computer-flown"), &g_st.hideAi);
    const auto list = filterPilots(m.pilots, g_st.hideAi, g_st.pilotFilter);
    ImGui::SameLine(); ImGui::TextColored(p.muted, "%d", (int)list.size());
    if (list.empty()) { muted(m.conn.connected ? tr("nobody else is in this flight") : tr("not connected")); return; }
    if (ImGui::BeginTable("pilots", 8, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_ScrollY)) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn(tr("Callsign"), ImGuiTableColumnFlags_WidthStretch, 2.f);
        ImGui::TableSetupColumn(tr("Type"), ImGuiTableColumnFlags_WidthStretch, 1.2f);
        ImGui::TableSetupColumn(tr("distance"), ImGuiTableColumnFlags_WidthStretch, 1.2f);
        ImGui::TableSetupColumn(tr("altitude"), ImGuiTableColumnFlags_WidthStretch, 1.2f);
        ImGui::TableSetupColumn(tr("speed"), ImGuiTableColumnFlags_WidthStretch, 1.f);
        ImGui::TableSetupColumn(tr("frequency"), ImGuiTableColumnFlags_WidthStretch, 1.3f);
        ImGui::TableSetupColumn(tr("Squawk"), ImGuiTableColumnFlags_WidthStretch, 1.f);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthStretch, 1.6f);
        ImGui::TableHeadersRow();
        for (const Pilot* q : list) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::PushID((int)q->sid);
            light(p.rx, q->talking, 4.f); ImGui::SameLine();
            if (ImGui::Selectable(q->callsign.c_str(), false, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap)) {
                snprintf(g_st.chatInput, sizeof g_st.chatInput, "@%s ", q->callsign.c_str());
                g_st.tab = 3; g_st.chatFocus = true;
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Message"));
            ImGui::TableNextColumn(); ImGui::TextUnformatted(q->type.c_str());
            ImGui::TableNextColumn(); if (q->distNm >= 0) ImGui::Text("%.1f nm", q->distNm); else muted("-");
            ImGui::TableNextColumn(); ImGui::Text("%.0f ft", q->altFt);
            ImGui::TableNextColumn(); ImGui::Text("%.0f kt", q->gsKt);
            ImGui::TableNextColumn(); if (q->freqKhz) ImGui::TextUnformatted(fmtKhz(q->freqKhz).c_str()); else muted("-");
            ImGui::TableNextColumn(); ImGui::TextColored(q->emergency ? p.err : p.text, "%04u", q->squawk);
            ImGui::TableNextColumn();
            if (q->ai) { badge(q->drawn ? "AI" : "AI (not drawn)", p.ai); ImGui::SameLine(); }
            if (q->talking) { badge(tr("talking"), p.rx); ImGui::SameLine(); }
            if (q->emergency) badge("EMERG", p.err);
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
}

// ---------------------------------------------------------------- Messages
void drawMessages(Model& m, const Actions& a) {
    const Palette& p = palette();
    muted(tr("Messages on your frequency, private ones highlighted"));
    const float inputH = ImGui::GetFrameHeightWithSpacing() * 2.2f;
    ImGui::BeginChild("log", ImVec2(0, -inputH), ImGuiChildFlags_Borders);
    for (const Message& msg : m.messages) {
        ImVec4 c = msg.mine ? p.accent : msg.priv ? p.priv : msg.atc ? p.atc : msg.guard ? p.err : msg.system ? p.muted : p.text;
        ImGui::TextColored(p.muted, "%s", msg.timeText.c_str()); ImGui::SameLine();
        if (msg.priv) { badge(tr("private"), p.priv); ImGui::SameLine(); }
        if (msg.atc) { badge("ATC", p.atc); ImGui::SameLine(); }
        if (!msg.from.empty()) { ImGui::TextColored(c, "%s:", msg.from.c_str()); ImGui::SameLine(); }
        ImGui::PushStyleColor(ImGuiCol_Text, msg.mine || msg.system ? p.muted : p.text);
        ImGui::TextWrapped("%s", msg.text.c_str());
        ImGui::PopStyleColor();
    }
    if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 2) ImGui::SetScrollHereY(1.f);
    ImGui::EndChild();
    m.readUntil = m.elapsed;
    // quick replies
    const char* quick[4] = {"wilco", "roger", "say again", "unable"};
    for (int i = 0; i < 4; ++i) {
        if (i) ImGui::SameLine();
        if (ImGui::SmallButton(quick[i]) && a.sendText) a.sendText(quick[i]);
    }
    // the @ list
    const std::string typed = g_st.chatInput;
    if (!typed.empty() && typed[0] == '@' && typed.find(' ') == std::string::npos) {
        const std::string part = upperTrim(typed.substr(1));
        int shown = 0;
        for (const Pilot& q : m.pilots) {
            if (!part.empty() && upperTrim(q.callsign).find(part) != 0) continue;
            if (shown++ >= 6) break;
            if (shown > 1) ImGui::SameLine();
            if (ImGui::SmallButton(("@" + q.callsign).c_str())) { snprintf(g_st.chatInput, sizeof g_st.chatInput, "@%s ", q.callsign.c_str()); g_st.chatFocus = true; }
        }
        if (!shown) muted(m.pilots.empty() ? tr("nobody else is in this flight") : "no such callsign");
    }
    if (g_st.chatFocus) { ImGui::SetKeyboardFocusHere(); g_st.chatFocus = false; }
    ImGui::SetNextItemWidth(-70);
    const bool enter = ImGui::InputTextWithHint("##say", "Say ...  (@CALLSIGN for a private message, 121.500 is heard by all)", g_st.chatInput, sizeof g_st.chatInput, ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    if ((ImGui::Button(tr("Send")) || enter) && g_st.chatInput[0] && a.sendText) {
        a.sendText(g_st.chatInput);
        g_st.chatInput[0] = 0;
        g_st.chatFocus = true;
    }
}

// ---------------------------------------------------------------- Flight plan
void drawFlightPlan(Model& m, const Actions& a) {
    const Palette& p = palette();
    if (!g_st.fpLoaded || g_st.fpSerial != m.fp.ofpSerial) { g_st.fpEdit = m.fp; g_st.fpLoaded = true; g_st.fpSerial = m.fp.ofpSerial; }
    FlightPlanView& f = g_st.fpEdit;
    auto field = [](const char* label, std::string& v, float w, const char* hint = "") {
        char buf[256]; snprintf(buf, sizeof buf, "%s", v.c_str());
        ImGui::SetNextItemWidth(w);
        if (ImGui::InputTextWithHint(label, hint, buf, sizeof buf)) v = buf;
    };
    // SimBrief: the official fetch of the latest OFP (only when pressed), and the official page for a new plan.
    ImGui::BeginDisabled(m.fp.fetching);
    if (ImGui::Button(m.fp.fetching ? tr("fetching...") : tr("Import from SimBrief")) && a.importSimbrief) a.importSimbrief();
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button(tr("Plan on SimBrief")) && a.openUrl) a.openUrl(kSimbriefNewFlightUrl);
    ImGui::SameLine();
    if (!m.fp.simbriefSummary.empty()) muted(m.fp.simbriefSummary.c_str());
    else if (m.settings.simbriefUser.empty()) ImGui::TextColored(p.warn, "%s", tr("enter your SimBrief username or pilot ID in Settings > SimBrief"));
    else muted(tr("no OFP imported yet"));
    if (m.fp.ofp.present) {
        const OfpView& o = m.fp.ofp;
        ImGui::PushStyleColor(ImGuiCol_ChildBg, p.panelAlt);
        ImGui::BeginChild("ofp", ImVec2(0, ImGui::GetTextLineHeightWithSpacing() * 6.2f + ImGui::GetFrameHeightWithSpacing()), ImGuiChildFlags_AlwaysUseWindowPadding);
        ImGui::PopStyleColor();
        heading("OFP"); ImGui::SameLine(); ImGui::Text("%s   %s   %s", o.callsign.c_str(), o.type.c_str(), o.reg.c_str()); ImGui::SameLine(); badge(o.units.c_str(), p.ai);
        ImGui::Text("%s %s%s  >  %s %s%s", o.dep.c_str(), o.depRwy.empty() ? "" : tr("rwy "), o.depRwy.c_str(), o.dest.c_str(), o.destRwy.empty() ? "" : tr("rwy "), o.destRwy.c_str());
        if (!o.altn.empty()) { ImGui::SameLine(); muted(tr("alternate")); ImGui::SameLine(); ImGui::TextUnformatted(o.altn.c_str()); }
        muted("SID"); ImGui::SameLine(); ImGui::TextUnformatted(o.sid.empty() ? "-" : o.sid.c_str()); ImGui::SameLine(); muted("STAR"); ImGui::SameLine(); ImGui::TextUnformatted(o.star.empty() ? "-" : o.star.c_str());
        ImGui::SameLine(); muted(tr("cruise")); ImGui::SameLine(); ImGui::Text("%s  TAS %s  %s", o.cruise.c_str(), o.tas.c_str(), o.mach.c_str());
        muted(tr("route")); ImGui::SameLine(); ImGui::TextWrapped("%s", o.route.c_str());
        muted("EOBT"); ImGui::SameLine(); ImGui::TextUnformatted(o.eobt.c_str()); ImGui::SameLine(); muted("ETD"); ImGui::SameLine(); ImGui::TextUnformatted(o.etd.c_str());
        ImGui::SameLine(); muted("ETA"); ImGui::SameLine(); ImGui::TextUnformatted(o.eta.c_str());
        ImGui::SameLine(); muted(tr("fuel block / trip")); ImGui::SameLine(); ImGui::Text("%s / %s %s", o.block.c_str(), o.trip.c_str(), o.units.c_str());
        muted("ZFW / TOW / LDW"); ImGui::SameLine(); ImGui::Text("%s / %s / %s %s", o.zfw.c_str(), o.tow.c_str(), o.ldw.c_str(), o.units.c_str());
        if (!o.pdfUrl.empty()) { if (ImGui::Button(tr("Open OFP (PDF)")) && a.openUrl) a.openUrl(o.pdfUrl); ImGui::SameLine(); }
        if (ImGui::Button(tr("Write X-Plane .fms")) && a.writeFms) setNote(a.writeFms(), m.elapsed);
        ImGui::EndChild();
    }
    ImGui::Separator();
    field(tr("Rules"), f.rules, 50, "I"); ImGui::SameLine(); field(tr("Callsign"), f.callsign, 110); ImGui::SameLine(); field(tr("Type"), f.type, 80, "A320");
    field(tr("Departure"), f.dep, 80, "EETN"); ImGui::SameLine(); field(tr("Destination"), f.dest, 80, "EVRA"); ImGui::SameLine(); field(tr("Alternate"), f.altn, 80, "EYVI");
    field(tr("Route"), f.route, -1, "ULGOT N746 ABETI");
    field(tr("Cruise altitude, ft"), f.cruise, 90, "36000"); ImGui::SameLine(); field(tr("TAS, kt"), f.tas, 70, "447"); ImGui::SameLine(); field(tr("EOBT (UTC)"), f.eobt, 70, "0730");
    field(tr("Equipment"), f.equip, 220, "SDE2E3FGHIRWXY/LB1D1"); ImGui::SameLine(); field(tr("Remarks"), f.remarks, -1, "PBN/A1B1 REG/ESTTR");
    const auto problems = flightPlanProblems(f);
    for (const auto& pr : problems) ImGui::TextColored(p.warn, "- %s", pr.c_str());
    ImGui::BeginDisabled(!problems.empty() || !m.conn.connected);
    if (ImGui::Button(tr("File"), ImVec2(100, 0)) && a.fileFlightPlan) { m.fp = f; setNote(a.fileFlightPlan(), m.elapsed); }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button(tr("Cancel plan")) && a.cancelFlightPlan) a.cancelFlightPlan();
    ImGui::SameLine();
    const char* st = m.fp.status == 1 ? tr("filed, waiting for ATC") : m.fp.status == 2 ? tr("acknowledged by ATC") : m.fp.status == 3 ? tr("cleared") : m.fp.status == 4 ? tr("cancelled") : tr("not filed");
    ImGui::TextColored(m.fp.status == 2 || m.fp.status == 3 ? p.ok : p.muted, "%s", st);
    if (m.fp.status >= 1) {
        ImGui::Separator();
        heading(tr("Received clearance"));
        if (!m.fp.clearance.empty()) ImGui::TextWrapped("%s", m.fp.clearance.c_str());
        if (ImGui::BeginTable("clr", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
            ImGui::TableSetupColumn(""); ImGui::TableSetupColumn(tr("Plan")); ImGui::TableSetupColumn("ATC"); ImGui::TableHeadersRow();
            for (const ClearanceRow& r : clearanceRows(m.fp)) {
                ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::TextUnformatted(tr(r.what.c_str()));
                ImGui::TableNextColumn(); ImGui::TextUnformatted(r.plan.empty() ? "-" : r.plan.c_str());
                ImGui::TableNextColumn(); ImGui::TextColored(r.differs ? p.warn : (r.atc.empty() ? p.muted : p.ok), "%s%s", r.atc.empty() ? "-" : r.atc.c_str(), r.differs ? "  !" : "");
                if (r.differs && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("differs from the plan"));
            }
            ImGui::EndTable();
        }
    }
}

// ---------------------------------------------------------------- Host / Connect
void drawHost(Model& m, const Actions& a) {
    const Palette& p = palette();
    if (!g_st.editLoaded) { g_st.edit = m.settings; g_st.editLoaded = true; }
    Settings& e = g_st.edit;
    heading(tr("Host a flight"));
    if (ImGui::Checkbox("##host", &e.hostEnabled)) g_st.editDirty = true;
    ImGui::SameLine(); ImGui::TextUnformatted(tr("Host a flight"));
    ImGui::SameLine(); ImGui::SetNextItemWidth(70);
    char port[8]; snprintf(port, sizeof port, "%s", e.hostPort.c_str());
    if (ImGui::InputText(tr("Port"), port, sizeof port, ImGuiInputTextFlags_CharsDecimal)) { e.hostPort = port; g_st.editDirty = true; }
    ImGui::SameLine(); ImGui::SetNextItemWidth(120);
    char pw[64]; snprintf(pw, sizeof pw, "%s", e.password.c_str());
    if (ImGui::InputText(tr("Password"), pw, sizeof pw, ImGuiInputTextFlags_Password)) { e.password = pw; g_st.editDirty = true; }
    if (ImGui::Checkbox(tr("Ask the router to open the port"), &e.hostUpnp)) g_st.editDirty = true;
    if (ImGui::Checkbox(tr("Rare tropospheric ducting (realism)"), &e.hostDucting)) g_st.editDirty = true;
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", tr("On a warm, still evening the flight's shared weather may let VHF reach far past the horizon over the sea"));
    const HostInfo& h = m.host;
    if (h.running) {
        ImGui::TextColored(p.ok, "%s: %s %u  ·  %d %s", tr("Running"), tr("Port"), h.port, h.clients, tr("connected"));
        if (!h.address.empty()) {
            ImGui::TextUnformatted(tr("Friends type")); ImGui::SameLine();
            pushFont(g_st.fonts.heading); ImGui::TextColored(p.accent, "%s", h.address.c_str()); popFont(g_st.fonts.heading);
            ImGui::SameLine();
            if (ImGui::SmallButton(tr("Copy")) && a.copyAddress) setNote(a.copyAddress(), m.elapsed);
        }
        if (!h.joinCode.empty()) { ImGui::TextUnformatted(tr("Join code")); ImGui::SameLine(); ImGui::TextColored(p.ok, "%s", h.joinCode.c_str()); }
        if (h.mapped) ImGui::TextColored(p.ok, "%s (%s)", tr("Router opened the port"), h.router.empty() ? "UPnP" : h.router.c_str());
        else if (h.upnpBusy) muted("asking the router...");
        else if (h.doubleNat) ImGui::TextColored(p.warn, "%s", tr("Hosting over the internet cannot work from here (double NAT)"));
        else if (!h.error.empty()) ImGui::TextColored(p.warn, "%s: %s", tr("Port not opened"), h.error.c_str());
        if (!h.lanAddress.empty()) { muted(tr("on your network")); ImGui::SameLine(); ImGui::TextUnformatted(h.lanAddress.c_str()); }
        for (const auto& cs : h.callsigns) { ImGui::Bullet(); ImGui::TextUnformatted(cs.c_str()); }
    } else muted(tr("Not hosting"));
    ImGui::Separator();
    heading(tr("Connect to"));
    char host[128]; snprintf(host, sizeof host, "%s", e.host.c_str());
    ImGui::SetNextItemWidth(260);
    if (ImGui::InputTextWithHint("##srv", tr("Server address or join code"), host, sizeof host)) { e.host = host; g_st.editDirty = true; }
    ImGui::SameLine(); ImGui::SetNextItemWidth(70);
    char cport[8]; snprintf(cport, sizeof cport, "%s", e.port.c_str());
    if (ImGui::InputText(tr("Port"), cport, sizeof cport, ImGuiInputTextFlags_CharsDecimal)) { e.port = cport; g_st.editDirty = true; }
    if (ImGui::Button(g_st.editDirty ? tr("Apply") : tr("Reconnect"))) {
        if (g_st.editDirty && a.applySettings) { a.applySettings(e); g_st.editDirty = false; setNote(tr("Saved"), m.elapsed); }
        else if (a.reconnect) a.reconnect();
    }
    ImGui::Separator();
    heading(tr("Connection"));
    light(m.conn.connected ? p.ok : (m.conn.connecting ? p.warn : p.err), true); ImGui::SameLine();
    ImGui::TextUnformatted(m.conn.status.c_str());
    if (!m.conn.endpoint.empty()) { muted(m.conn.endpoint.c_str()); ImGui::SameLine(); ImGui::Text("as %s (%s%s%s)", m.conn.callsign.c_str(), m.conn.icao.c_str(), m.conn.livery.empty() ? "" : " ", m.conn.livery.c_str()); }
    if (m.conn.connected) {
        ImGui::Text("%s: %.1f s  ·  %d %s  ·  %d pilots, %d AI", tr("last packet"), m.conn.lastPacketS, m.conn.packetsPerS, tr("packets/s"), m.conn.peers, m.conn.aiCount);
        ImGui::Text("%s: v%u (protocol %u)", tr("Server version"), m.conn.protoServer, m.conn.protoLocal);
    }
    if (!m.conn.versionMismatch.empty()) { ImGui::TextColored(p.err, "%s", m.conn.versionMismatch.c_str()); muted(tr("Everyone must run the same XRadio version")); }
    if (m.update.newer) {
        ImGui::TextColored(p.warn, "%s: v%s (you: v%s)", tr("Update available"), m.update.latest.c_str(), m.update.current.c_str());
        ImGui::SameLine(); if (ImGui::SmallButton(tr("Open")) && a.openUrl) a.openUrl(m.update.url);
    }
}

// ---------------------------------------------------------------- Settings
void drawField(const FieldRef& f, Model& m, const Actions& a) {
    (void)a;
    const Palette& p = palette();
    ImGui::PushID(f.key);
    switch (f.kind) {
        case Kind::Text: {
            std::string& v = *(std::string*)f.ptr;
            char buf[256]; snprintf(buf, sizeof buf, "%s", v.c_str());
            ImGui::SetNextItemWidth(220);
            if (ImGui::InputText(tr(f.label), buf, std::min<size_t>(sizeof buf, f.maxLen + 1), f.digitsOnly ? ImGuiInputTextFlags_CharsDecimal : 0)) { v = buf; g_st.editDirty = true; }
            break;
        }
        case Kind::Bool: if (ImGui::Checkbox(tr(f.label), (bool*)f.ptr)) g_st.editDirty = true; break;
        case Kind::Slider: {
            ImGui::SetNextItemWidth(220);
            char fmt[16]; snprintf(fmt, sizeof fmt, "%%.%df %s", f.decimals, f.unit);
            if (ImGui::SliderFloat(tr(f.label), (float*)f.ptr, f.lo, f.hi, fmt)) g_st.editDirty = true;
            break;
        }
        case Kind::Choice: {
            std::string& v = *(std::string*)f.ptr;
            const bool isMic = std::string(f.key) == "mic";
            const auto& opts = isMic ? m.voice.micDevices : m.voice.outDevices;
            ImGui::SetNextItemWidth(260);
            if (ImGui::BeginCombo(tr(f.label), v.empty() ? tr("system default") : v.c_str())) {
                if (ImGui::Selectable(tr("system default"), v.empty())) { v.clear(); g_st.editDirty = true; }
                for (const auto& o : opts) if (ImGui::Selectable(o.c_str(), o == v)) { v = o; g_st.editDirty = true; }
                ImGui::EndCombo();
            }
            break;
        }
        case Kind::KeyBind: {
            int& v = *(int*)f.ptr;
            const bool isPtt = std::string(f.key) == "pttkey";
            bool& cap = isPtt ? g_st.capturingPtt : g_st.capturingTower;
            ImGui::TextUnformatted(tr(f.label)); ImGui::SameLine();
            if (ImGui::Button(cap ? tr("press a key") : (v > 0 ? keyName(v).c_str() : tr("none")), ImVec2(150, 0))) { cap = !cap; if (isPtt) g_st.capturingTower = false; else g_st.capturingPtt = false; }
            if (cap) { ImGui::SameLine(); ImGui::TextColored(p.warn, "..."); }
            ImGui::SameLine(); if (ImGui::SmallButton("x")) { v = 0; g_st.editDirty = true; cap = false; }
            break;
        }
    }
    ImGui::PopID();
}

void drawSettings(Model& m, const Actions& a) {
    const Palette& p = palette();
    if (!g_st.editLoaded) { g_st.edit = m.settings; g_st.editLoaded = true; }
    Settings& e = g_st.edit;
    const char* groups[7] = {tr("Audio"), tr("Controls"), tr("Display"), tr("Traffic"), tr("Language"), tr("Tower"), "SimBrief"};
    for (int i = 0; i < 7; ++i) { if (i) ImGui::SameLine(); if (ImGui::RadioButton(groups[i], g_st.settingsGroup == i)) g_st.settingsGroup = i; }
    ImGui::Separator();
    ImGui::BeginChild("fields", ImVec2(0, -ImGui::GetFrameHeightWithSpacing() * 1.4f));
    auto fields = describe(e);
    const int g = g_st.settingsGroup;
    if (g == 0) {
        for (auto& f : fields) if (f.tab == 1 && f.kind != Kind::KeyBind) drawField(f, m, a);
        ImGui::Separator();
        if (ImGui::Button(tr("Mic test: hold"), ImVec2(140, 0))) {}
        const bool held = ImGui::IsItemActive();
        if (held != g_st.micTest) { g_st.micTest = held; if (a.keyPtt) a.keyPtt(held); }
        ImGui::SameLine(); ImGui::TextUnformatted(tr("Level")); ImGui::SameLine(); meter(m.voice.micLevel, p.ok, 160.f);
        ImGui::SameLine(); if (ImGui::SmallButton("refresh devices") && a.refreshDevices) a.refreshDevices();
        muted(m.voice.status.c_str());
    } else if (g == 1) {
        for (auto& f : fields) if (f.kind == Kind::KeyBind) drawField(f, m, a);
        muted("Also: bind xradio/ptt and xradio/tower/* in X-Plane's keyboard or joystick settings.");
    } else if (g == 2) {
        ImGui::SetNextItemWidth(200);
        if (ImGui::SliderFloat(tr("UI scale"), &e.uiScale, 0.75f, 2.f, "%.2f")) g_st.editDirty = true;
        ImGui::TextUnformatted(tr("Theme")); ImGui::SameLine();
        if (ImGui::RadioButton(tr("Dark"), e.uiTheme != "light")) { e.uiTheme = "dark"; g_st.editDirty = true; }
        ImGui::SameLine(); if (ImGui::RadioButton(tr("Light"), e.uiTheme == "light")) { e.uiTheme = "light"; g_st.editDirty = true; }
        if (ImGui::Checkbox(tr("Cockpit overlay"), &e.uiOverlay)) g_st.editDirty = true;
        if (ImGui::Checkbox("Notices in the corner while the window is closed", &e.popUps)) g_st.editDirty = true;
    } else if (g == 3) {
        for (auto& f : fields) if (f.tab == 2 || (f.tab == 4 && (std::string(f.key) == "showai" || std::string(f.key) == "aimax" || std::string(f.key) == "atccue"))) drawField(f, m, a);
    } else if (g == 4) {
        if (ImGui::RadioButton(tr("English"), e.uiLang != "et")) { e.uiLang = "en"; g_st.editDirty = true; }
        ImGui::SameLine(); if (ImGui::RadioButton(tr("Estonian"), e.uiLang == "et")) { e.uiLang = "et"; g_st.editDirty = true; }
        muted("The radio itself stays English.");
    } else if (g == 5) {
        for (auto& f : fields) if (f.tab == 5) drawField(f, m, a);
    } else {
        for (auto& f : fields) if (std::string(f.key) == "simbriefuser") drawField(f, m, a);
        muted(tr("Kept only in XRadio's settings file. The latest OFP is fetched through SimBrief's official API, and only when you press Import."));
        if (ImGui::Button(tr("Plan on SimBrief")) && a.openUrl) a.openUrl(kSimbriefNewFlightUrl);
    }
    ImGui::EndChild();
    ImGui::BeginDisabled(!g_st.editDirty);
    if (ImGui::Button(tr("Apply"), ImVec2(120, 0)) && a.applySettings) { a.applySettings(e); g_st.editDirty = false; setNote(tr("Saved"), m.elapsed); }
    ImGui::EndDisabled();
    ImGui::SameLine(); if (ImGui::Button("Reload")) { g_st.edit = m.settings; g_st.editDirty = false; }
}

// ---------------------------------------------------------------- Tower
void drawTower(Model& m, const Actions& a) {
    const Palette& p = palette();
    heading(tr("Tower mode"));
    light(m.towerActive ? p.ok : p.muted, m.towerActive); ImGui::SameLine();
    ImGui::TextUnformatted(m.towerStatus.empty() ? "off" : m.towerStatus.c_str());
    if (ImGui::Button(m.towerActive ? tr("Leave the tower") : tr("Enter the tower"), ImVec2(180, 0)) && a.toggleTower) a.toggleTower();
    ImGui::TextWrapped("TowerTrainer must be running with Settings > XRadio > 'X-Plane tower link' ticked; the address, session code and position are in Settings > Tower. Commands xradio/tower/* look around, zoom and follow aircraft.");
}

// ---------------------------------------------------------------- the wizard
void drawWizard(Model& m, const Actions& a) {
    const Palette& p = palette();
    if (!g_st.editLoaded) { g_st.edit = m.settings; g_st.editLoaded = true; }
    Settings& e = g_st.edit;
    Wizard& w = g_st.wizard;
    heading(tr("Welcome to XRadio"));
    ImGui::Text("%d / %d  -  %s", w.step + 1, Wizard::kSteps, Wizard::title(w.step, language()));
    ImGui::Separator();
    if (w.step == 0) {
        char cs[16]; snprintf(cs, sizeof cs, "%s", e.callsign.c_str());
        ImGui::SetNextItemWidth(160);
        if (ImGui::InputTextWithHint(tr("Callsign"), "ESXYZ", cs, sizeof cs, ImGuiInputTextFlags_CharsUppercase)) e.callsign = cs;
        char ty[8]; snprintf(ty, sizeof ty, "%s", e.acIcao.c_str());
        ImGui::SetNextItemWidth(100);
        if (ImGui::InputTextWithHint(tr("Aircraft type (ICAO)"), "C172", ty, sizeof ty, ImGuiInputTextFlags_CharsUppercase)) e.acIcao = ty;
        muted("The type is what others see as your model; leave it empty to take X-Plane's.");
    } else if (w.step == 1) {
        auto fields = describe(e);
        for (auto& f : fields) if (f.kind == Kind::Choice) drawField(f, m, a);
        if (ImGui::Button(tr("Mic test: hold"), ImVec2(140, 0))) {}
        const bool held = ImGui::IsItemActive();
        if (held != g_st.micTest) { g_st.micTest = held; if (a.keyPtt) a.keyPtt(held); }
        ImGui::SameLine(); meter(m.voice.micLevel, p.ok, 160.f);
        muted(m.voice.status.c_str());
    } else if (w.step == 2) {
        auto fields = describe(e);
        for (auto& f : fields) if (std::string(f.key) == "pttkey") drawField(f, m, a);
        muted("Or bind the command xradio/ptt to a joystick button in X-Plane.");
    } else if (w.step == 4) {
        auto fields = describe(e);
        for (auto& f : fields) if (std::string(f.key) == "simbriefuser") drawField(f, m, a);
        muted(tr("Optional: with your SimBrief username or pilot ID, the Flight plan tab imports your latest OFP with one press (official API, only when you press). Leave it empty to skip."));
        if (ImGui::Button(tr("Plan on SimBrief")) && a.openUrl) a.openUrl(kSimbriefNewFlightUrl);
    } else {
        if (ImGui::RadioButton("Join a friend's flight", !e.hostEnabled)) e.hostEnabled = false;
        ImGui::SameLine(); if (ImGui::RadioButton("Host it myself", e.hostEnabled)) e.hostEnabled = true;
        if (!e.hostEnabled) {
            char host[128]; snprintf(host, sizeof host, "%s", e.host.c_str());
            ImGui::SetNextItemWidth(260);
            if (ImGui::InputTextWithHint(tr("Server address or join code"), "K7M2Q-X4PB9", host, sizeof host)) e.host = host;
        } else muted("Your friends get the address or join code from the Host tab.");
    }
    const std::string block = Wizard::blocker(w.step, e, m.voice);
    if (!block.empty()) ImGui::TextColored(p.warn, "Needed: %s", block.c_str());
    ImGui::Separator();
    if (w.step > 0 && ImGui::Button(tr("Back"))) w.step--;
    if (w.step > 0) ImGui::SameLine();
    ImGui::BeginDisabled(!block.empty() && w.step != 1);
    if (ImGui::Button(w.step == Wizard::kSteps - 1 ? tr("Finish") : tr("Next"), ImVec2(100, 0))) {
        if (w.step < Wizard::kSteps - 1) w.step++;
        else { e.uiWizardDone = true; if (a.applySettings) a.applySettings(e); g_st.editDirty = false; g_st.wizardOpen = false; setNote(tr("Done. Start by tuning a controller on the ATC tab."), m.elapsed); g_st.tab = 1; }
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button(tr("Skip"))) { e.uiWizardDone = true; if (a.applySettings) a.applySettings(e); g_st.wizardOpen = false; }
}

}  // namespace

// ---------------------------------------------------------------- public
UiState& state() { return g_st; }
void setFonts(const Fonts& f) { g_st.fonts = f; }
void toast(const std::string& text, int colour, double now) { g_st.toasts.push(text, colour, now); g_st.lastActivity = now; }
const char* tabName(int tab) { return tr(kTabs[tab < 0 || tab > 7 ? 0 : tab]); }
int tabCount(bool withTower) { return withTower ? 8 : 7; }
bool overlayWantsMouse() { return g_overlayWantsMouse; }

void noteKey(int vk, bool down) {
    if (!down || vk <= 0) return;
    if (g_st.capturingPtt) { g_st.edit.pttKey = vk; g_st.capturingPtt = false; g_st.editDirty = true; }
    else if (g_st.capturingTower) { g_st.edit.towerKey = vk; g_st.capturingTower = false; g_st.editDirty = true; }
}

void drawMain(Model& m, const Actions& a) {
    const Palette& p = palette();
    setLanguage(m.settings.uiLang);
    g_st.toasts.expire(m.elapsed);
    if (!m.settings.uiWizardDone && !g_st.wizardOpen && !g_st.editLoaded) { g_st.wizardOpen = true; }
    const int tabs = tabCount(true);
    // sidebar
    ImGui::BeginChild("side", ImVec2(126, 0), ImGuiChildFlags_Borders);
    pushFont(g_st.fonts.heading);
    ImGui::TextColored(p.accent, "XRadio");
    popFont(g_st.fonts.heading);
    muted(("v" + std::string(m.update.current)).c_str());
    ImGui::Spacing();
    const int unread = countUnread(m.messages, m.readUntil);
    for (int i = 0; i < tabs; ++i) {
        const bool on = g_st.tab == i && !g_st.wizardOpen;
        ImGui::PushStyleColor(ImGuiCol_Button, on ? ImVec4(p.accent.x, p.accent.y, p.accent.z, 0.35f) : ImVec4(0, 0, 0, 0));
        char label[48];
        if (i == 3 && unread) snprintf(label, sizeof label, "%s (%d)", tabName(i), unread);
        else snprintf(label, sizeof label, "%s", tabName(i));
        if (ImGui::Button(label, ImVec2(-1, 0))) { g_st.tab = i; g_st.wizardOpen = false; }
        ImGui::PopStyleColor();
    }
    ImGui::Spacing();
    ImGui::Separator();
    light(m.conn.connected ? p.ok : p.err, true); ImGui::SameLine(); ImGui::TextUnformatted(m.conn.connected ? tr("connected") : tr("not connected"));
    ImGui::TextUnformatted(m.conn.callsign.c_str());
    light(m.pttDown ? p.tx : (m.txWho.empty() ? p.muted : p.rx), m.pttDown || !m.txWho.empty()); ImGui::SameLine();
    muted(m.pttDown ? "TX" : (m.txWho.empty() ? "quiet" : m.txWho.c_str()));
    ImGui::EndChild();
    ImGui::SameLine();
    // content
    ImGui::BeginChild("content", ImVec2(0, 0));
    if (g_st.wizardOpen) drawWizard(m, a);
    else switch (g_st.tab) {
        case 0: drawRadio(m, a); break;
        case 1: drawAtc(m, a); break;
        case 2: drawPilots(m, a); break;
        case 3: drawMessages(m, a); break;
        case 4: drawFlightPlan(m, a); break;
        case 5: drawHost(m, a); break;
        case 6: drawSettings(m, a); break;
        default: drawTower(m, a); break;
    }
    if (m.elapsed - g_st.noteAt < 4.0 && !g_st.note.empty()) {
        ImGui::SetCursorPosY(ImGui::GetWindowHeight() - ImGui::GetFrameHeight() - 6);
        ImGui::TextColored(p.ok, "%s", g_st.note.c_str());
    }
    ImGui::EndChild();
}

void drawOverlay(Model& m, const Actions& a, float width) {
    (void)a;
    const Palette& p = palette();
    setLanguage(m.settings.uiLang);
    g_st.toasts.expire(m.elapsed);
    const bool busy = m.pttDown || !m.txWho.empty() || m.com[0].rx || m.com[1].rx;
    if (busy || m.elapsed - m.readUntil < 20.0) g_st.lastActivity = m.elapsed;
    for (const Message& msg : m.messages) if (!msg.mine && msg.t > g_st.lastActivity) g_st.lastActivity = msg.t;
    const bool collapsed = (m.elapsed - g_st.lastActivity > 20.0) && !g_st.overlayHover && g_st.toasts.size() == 0;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 o = ImGui::GetCursorScreenPos();
    // drag handle
    dl->AddRectFilled(ImVec2(o.x, o.y), ImVec2(o.x + 10, o.y + (collapsed ? 22.f : 46.f)), col(p.accent, 0.8f), 3.f);
    ImGui::SetCursorScreenPos(ImVec2(o.x + 16, o.y + 4));
    ImGui::BeginGroup();
    pushFont(g_st.fonts.heading);
    ImGui::TextColored(m.com[0].powered ? p.text : p.muted, "COM1 %s", fmtKhz(m.com[0].activeKhz).c_str());
    ImGui::SameLine();
    if (!m.com[0].stationName.empty()) { ImGui::TextColored(p.atc, "%s", m.com[0].stationName.c_str()); ImGui::SameLine(); }
    if (!collapsed) { ImGui::TextColored(m.com[1].powered ? p.text : p.muted, "COM2 %s", fmtKhz(m.com[1].activeKhz).c_str()); ImGui::SameLine(); }
    popFont(g_st.fonts.heading);
    light(m.pttDown ? p.tx : (m.txWho.empty() ? p.muted : p.rx), busy, 7.f); ImGui::SameLine();
    if (m.pttDown) ImGui::TextColored(p.tx, "TX");
    else if (!m.txWho.empty()) ImGui::TextColored(p.rx, "%s", m.txWho.c_str());
    else muted("-");
    if (m.fp.squawk) { ImGui::SameLine(); ImGui::TextColored(p.ok, "SQK %04u", m.fp.squawk); }
    if (!collapsed) {
        const Message* lastAtc = nullptr;
        for (auto it = m.messages.rbegin(); it != m.messages.rend(); ++it) if (it->atc || it->priv) { lastAtc = &*it; break; }
        if (lastAtc) {
            std::string t = lastAtc->from + ": " + lastAtc->text;
            const size_t maxc = (size_t)std::max(20.f, width / 7.5f);
            if (t.size() > maxc) t = t.substr(0, maxc - 1) + "~";
            ImGui::TextColored(lastAtc->priv ? p.priv : p.atc, "%s", t.c_str());
        }
    }
    ImGui::EndGroup();
    for (const Toast& t : g_st.toasts.items()) {
        const ImVec4 c = t.colour == 1 ? p.warn : t.colour == 2 ? p.ok : t.colour == 3 ? p.err : p.text;
        const float age = (float)(m.elapsed - t.at);
        const float alpha = age > 6.f ? std::max(0.f, 1.f - (age - 6.f) / 2.f) : 1.f;
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(c.x, c.y, c.z, alpha));
        ImGui::TextWrapped("%s", t.text.c_str());
        ImGui::PopStyleColor();
    }
    g_st.overlayHover = ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
    g_overlayWantsMouse = ImGui::IsMouseHoveringRect(ImVec2(o.x - 4, o.y - 4), ImVec2(o.x + 14, o.y + 50));
}

}  // namespace ui2
}  // namespace xr
