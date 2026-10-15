#include "i18n.h"

#include <cstring>
#include <string>

namespace xr {
namespace ui2 {

namespace {
std::string g_lang = "en";

struct Pair { const char* key; const char* et; };
// Keys are the English texts. Only what the window says; the radio itself is always English.
const Pair kEt[] = {
    {"Radio", "Raadio"}, {"ATC", "ATC"}, {"Pilots", "Piloodid"}, {"Messages", "Sõnumid"}, {"Flight plan", "Lennuplaan"},
    {"Host / Connect", "Host / Ühendus"}, {"Settings", "Seaded"}, {"Tower", "Torn"},
    {"Active", "Aktiivne"}, {"Standby", "Ootel"}, {"Swap", "Vaheta"}, {"Volume", "Helitugevus"}, {"no power", "vooluta"},
    {"Guard 121.500 on COM2", "Guard 121.500 COM2-s"}, {"Nobody is transmitting", "Keegi ei saada"}, {"You are transmitting", "Sina saadad"},
    {"is transmitting", "saadab"}, {"Signal", "Signaal"}, {"Push-to-talk", "Saatenupp"}, {"microphone keyed for", "mikrofon all"},
    {"Online controllers", "Lennujuhid võrgus"}, {"No controllers online", "Lennujuhte võrgus pole"}, {"click to tune COM1, right-click COM2", "klõps häälestab COM1, parem klõps COM2"},
    {"ATIS", "ATIS"}, {"Read", "Loe"}, {"Listen", "Kuula"}, {"Hide computer-flown", "Peida arvuti lennatavad"}, {"Filter", "Filter"},
    {"nobody else is in this flight", "kedagi teist lennus pole"}, {"Message", "Sõnum"}, {"Say", "Ütle"}, {"Send", "Saada"},
    {"wilco", "wilco"}, {"roger", "roger"}, {"say again", "say again"}, {"unable", "unable"}, {"private", "privaatne"}, {"to", "kellele"},
    {"Optional: with your SimBrief username or pilot ID, the Flight plan tab imports your latest OFP with one press (official API, only when you press). Leave it empty to skip.", "Valikuline: SimBriefi kasutajanime või piloodi ID-ga impordib lennuplaani leht sinu viimase OFP ühe vajutusega (ametlik API, ainult vajutades). Jäta tühjaks, kui ei kasuta."},
    {"Plan on SimBrief", "Planeeri SimBriefis"}, {"fetching...", "laen..."}, {"no OFP imported yet", "OFP-d pole veel imporditud"},
    {"enter your SimBrief username or pilot ID in Settings > SimBrief", "sisesta SimBriefi kasutajanimi või piloodi ID: Seaded > SimBrief"},
    {"rwy ", "rada "}, {"alternate", "varulennuväli"}, {"cruise", "reisikõrgus"}, {"route", "marsruut"}, {"fuel block / trip", "kütus block / trip"},
    {"Open OFP (PDF)", "Ava OFP (PDF)"}, {"Level", "Kõrgus"}, {"SID", "SID"},
    {"Kept only in XRadio's settings file. The latest OFP is fetched through SimBrief's official API, and only when you press Import.", "Hoitakse ainult XRadio seadete failis. Viimane OFP laetakse SimBriefi ametliku API kaudu ja ainult siis, kui vajutad Impordi."},
    {"Import from SimBrief", "Impordi SimBriefist"}, {"File", "Esita"}, {"Cancel plan", "Tühista plaan"}, {"Write X-Plane .fms", "Kirjuta X-Plane'i .fms"},
    {"Rules", "Reeglid"}, {"Departure", "Väljumine"}, {"Destination", "Sihtkoht"}, {"Alternate", "Varulennuväli"}, {"Type", "Tüüp"},
    {"Callsign", "Kutsung"}, {"Route", "Marsruut"}, {"Cruise altitude, ft", "Reisikõrgus, ft"}, {"TAS, kt", "TAS, kt"}, {"EOBT (UTC)", "EOBT (UTC)"},
    {"Equipment", "Varustus"}, {"Remarks", "Märkused"}, {"filed, waiting for ATC", "esitatud, ootab ATC-d"}, {"acknowledged by ATC", "ATC kinnitas"},
    {"cleared", "luba antud"}, {"cancelled", "tühistatud"}, {"not filed", "esitamata"}, {"Plan", "Plaan"}, {"Received clearance", "Saadud luba"},
    {"Squawk", "Squawk"}, {"differs from the plan", "erineb plaanist"}, {"Host a flight", "Hosti lend"}, {"Port", "Port"}, {"Password", "Parool"},
    {"Ask the router to open the port", "Palu ruuteril port avada"}, {"Friends type", "Sõbrad sisestavad"}, {"Copy", "Kopeeri"}, {"copied", "kopeeritud"},
    {"Join code", "Liitumiskood"}, {"Connect to", "Ühendu"}, {"Server address or join code", "Serveri aadress või liitumiskood"}, {"Reconnect", "Ühenda uuesti"},
    {"Connection", "Ühendus"}, {"connected", "ühendatud"}, {"not connected", "ühendamata"}, {"last packet", "viimane pakett"}, {"packets/s", "paketti/s"},
    {"Server version", "Serveri versioon"}, {"Everyone must run the same XRadio version", "Kõik peavad olema samal XRadio versioonil"},
    {"Audio", "Heli"}, {"Controls", "Juhtimine"}, {"Display", "Kuva"}, {"Traffic", "Liiklus"}, {"Language", "Keel"}, {"Microphone", "Mikrofon"},
    {"Speakers", "Kõlarid"}, {"system default", "süsteemi vaikimisi"}, {"Mic test: hold", "Mikrofoni test: hoia"}, {"Level", "Tase"},
    {"Cockpit noise", "Kokpiti müra"}, {"Squelch", "Squelch"}, {"Radio effect", "Raadioefekt"}, {"Hiss", "Kahin"}, {"Sidetone", "Sidetone"},
    {"PTT key", "PTT klahv"}, {"press a key", "vajuta klahvi"}, {"Tower-mode key", "Torni režiimi klahv"}, {"none", "puudub"},
    {"UI scale", "Kasutajaliidese suurus"}, {"Theme", "Teema"}, {"Dark", "Tume"}, {"Light", "Hele"}, {"Cockpit overlay", "Kokpiti riba"},
    {"CSL folder", "CSL kaust"}, {"Labels", "Sildid"}, {"Label distance, nm", "Siltide kaugus, NM"}, {"Show computer-flown aircraft", "Näita arvuti lennatavaid"},
    {"Computer-flown aircraft drawn", "Arvuti lennatavaid joonistatakse"}, {"Read out ATC calls", "Loe ATC kõned ette"}, {"Apply", "Rakenda"}, {"Saved", "Salvestatud"},
    {"English", "English"}, {"Estonian", "Eesti"}, {"Welcome to XRadio", "Tere tulemast XRadiosse"}, {"Next", "Edasi"}, {"Back", "Tagasi"}, {"Finish", "Valmis"},
    {"Skip", "Jäta vahele"}, {"Aircraft type (ICAO)", "Õhusõiduki tüüp (ICAO)"}, {"Tower mode", "Torni režiim"}, {"Enter the tower", "Mine torni"}, {"Leave the tower", "Lahku tornist"},
    {"Update available", "Uuendus saadaval"}, {"Open", "Ava"}, {"Close", "Sulge"}, {"Check for updates", "Kontrolli uuendusi"},
    {"Messages on your frequency, private ones highlighted", "Sinu sageduse sõnumid, privaatsed esile tõstetud"}, {"unread", "lugemata"},
    {"Weather and time", "Ilm ja aeg"}, {"Who hears what you say", "Kes kuuleb, mida sa ütled"}, {"No stations online", "Jaamu võrgus pole"},
    {"Not hosting", "Ei hosti"}, {"Running", "Töötab"}, {"connected", "ühendatud"}, {"on your network", "sinu võrgus"},
    {"Router opened the port", "Ruuter avas pordi"}, {"Port not opened", "Port on avamata"}, {"Hosting over the internet cannot work from here (double NAT)", "Interneti kaudu hostimine siit ei tööta (topelt-NAT)"},
    {"distance", "kaugus"}, {"frequency", "sagedus"}, {"altitude", "kõrgus"}, {"speed", "kiirus"}, {"AI", "AI"}, {"talking", "räägib"},
    {"Done. Start by tuning a controller on the ATC tab.", "Valmis. Alusta ATC vahelehelt lennujuhi häälestamisega."},
};
}  // namespace

void setLanguage(const std::string& lang) { g_lang = (lang == "et") ? "et" : "en"; }
const char* language() { return g_lang.c_str(); }

const char* tr(const char* key) {
    if (g_lang != "et" || !key) return key;
    for (const Pair& p : kEt) if (strcmp(p.key, key) == 0) return p.et;
    return key;
}

}  // namespace ui2
}  // namespace xr
