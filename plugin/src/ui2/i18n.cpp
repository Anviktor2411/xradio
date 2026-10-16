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
    {"Rare tropospheric ducting (realism)", "Harv troposfääri juhtekanal (realism)"},
    {"On a warm, still evening the flight's shared weather may let VHF reach far past the horizon over the sea",
     "Soojal ja tuulevaiksel õhtul võib lennu ühine ilm lasta VHF-il ulatuda mere kohal kaugele üle horisondi"},
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
    // Tower mode's windows
    {"Tower: ground radar", "Torn: maaradar"}, {"Tower: strips", "Torn: ribad"}, {"Tower: Delivery", "Torn: Delivery"}, {"Tower: frequency", "Torn: sagedus"},
    {"Tower: ATIS / operations", "Torn: ATIS / tegevus"}, {"Tower: TowerTrainer", "Torn: TowerTrainer"},
    {"Fit", "Kogu väli"}, {"the whole airport", "kogu lennuväli pildile"}, {"Stop following", "Lõpeta jälgimine"}, {"Follow", "Jälgi"},
    {"the camera follows the selected aircraft", "kaamera jälgib valitud õhusõidukit"}, {"Look at", "Vaata"}, {"Binoculars", "Binokkel"}, {"vehicle", "sõiduk"},
    {"followed", "jälgimisel"}, {"Select", "Vali"}, {"Follow with the camera", "Jälgi kaameraga"}, {"Look at it", "Pööra kaamera sinna"}, {"Accept the handoff", "Võta üle"},
    {"aircraft", "õhusõidukit"},
    {"wheel: zoom   drag: move   right-drag: look around   click: select   right-click: more", "ratas: suum   lohistus: liiguta   parem lohistus: vaata ringi   klõps: vali   parem klõps: rohkem"},
    {"to another bay", "teise lahtrisse"}, {"stand", "seisukoht"}, {"Accept", "Võta vastu"}, {"strips", "ribad"},
    {"Incoming", "Saabuvad üleandmised"}, {"Pending", "Ootel"}, {"Clearance", "Lennuluba"}, {"Push", "Pushback"}, {"Taxi", "Ruleerimine"}, {"Holding", "Ootepunkt"},
    {"Runway", "Rada"}, {"Airborne", "Õhus"}, {"Approach", "Lähenemine"}, {"Final", "Lõpplähenemine"}, {"Runway (arrivals)", "Rada (saabuvad)"}, {"Taxi in", "Ruleerimine sisse"},
    {"Enroute", "Teel"}, {"Live pilots", "Päris piloodid"}, {"Vehicles", "Sõidukid"}, {"Other", "Muud"},
    {"Clearance requests", "Lennuloa taotlused"}, {"waiting", "ootab"}, {"none waiting", "keegi ei oota"}, {"Nobody has asked for a clearance.", "Keegi pole lennuluba küsinud."},
    {"To", "Kuhu"}, {"Stand", "Seisukoht"}, {"Waiting", "Ootab"}, {"live", "päris"}, {"vectors", "vektorid"}, {"Clear", "Anna luba"},
    {"TX - you are transmitting", "TX – sina saadad"}, {"me", "mina"},
    {"Instruction, e.g. FIN321 runway 26 cleared for takeoff", "Korraldus, nt FIN321 runway 26 cleared for takeoff"},
    {"Enter sends   Up / Down: earlier instructions   Tab: completes the callsign   Esc: clears", "Enter saadab   Üles / alla: varasemad korraldused   Tab: täiendab kutsungi   Esc: tühjendab"},
    {"operations", "tegevus"}, {"Runway in use", "Kasutusel rada"}, {"Low visibility (LVP)", "Halb nähtavus (LVP)"}, {"Stop bars", "Stopptuled"}, {"on", "sisse"}, {"off", "välja"},
    {"no ATIS yet", "ATIS-t veel pole"}, {"Light gun for", "Valgussignaal:"}, {"(on the ground)", "(maas)"}, {"(in the air)", "(õhus)"},
    {"Light gun: select an aircraft first (radar, strips or Delivery)", "Valgussignaal: vali enne õhusõiduk (radar, ribad või Delivery)"},
    {"Steady green", "Püsiv roheline"}, {"Flashing green", "Vilkuv roheline"}, {"Steady red", "Püsiv punane"}, {"Flashing red", "Vilkuv punane"}, {"Flashing white", "Vilkuv valge"},
    {"on the ground", "maas"}, {"in the air", "õhus"}, {"cleared for takeoff", "start lubatud"}, {"cleared to land", "maandumine lubatud"}, {"cleared to taxi", "ruleerimine lubatud"},
    {"return for landing", "pöördu tagasi maanduma"}, {"stop", "seisa"}, {"give way to other aircraft and continue circling", "anna teed teistele ja jätka tiirlemist"},
    {"taxi clear of the landing area", "ruleeri maandumisalalt eemale"}, {"aerodrome unsafe, do not land", "lennuväli ohtlik, ära maandu"},
    {"return to the starting point", "pöördu tagasi lähtekohta"}, {"no meaning in the air", "õhus tähendus puudub"},
    {"TowerTrainer decides: these are requests to its simulation.", "Otsustab TowerTrainer: need on palved tema simulatsioonile."},
    {"Connecting to TowerTrainer at %s\xE2\x80\xA6", "Ühendun TowerTraineriga aadressil %s\xE2\x80\xA6"},
    {"Start TowerTrainer (server.py) \xE2\x80\x93 waiting for %s\xE2\x80\xA6", "Käivita TowerTrainer (server.py) \xE2\x80\x93 ootan %s\xE2\x80\xA6"},
    {"No answer: %s. Trying again in %d s, or press Retry once it is running.", "Vastust pole: %s. Proovin uuesti %d s pärast \xE2\x80\x93 või vajuta „Proovi kohe uuesti“, kui see töötab."},
    {"No answer. Trying again in %d s, or press Retry once it is running.", "Vastust pole. Proovin uuesti %d s pärast \xE2\x80\x93 või vajuta „Proovi kohe uuesti“, kui see töötab."},
    {"No answer yet. Trying again now.", "Vastust veel pole. Proovin kohe uuesti."},
    {"TowerTrainer is running, but it hosts no session %s", "TowerTrainer töötab, aga seanssi %s seal ei ole"},
    {"In TowerTrainer tick Settings > XRadio > X-Plane tower link (session code %s). The plugin keeps trying.",
     "Lülita TowerTraineris sisse Seaded > XRadio > X-Plane tower link (seansi kood %s). Plugin proovib edasi."},
    {"TowerTrainer's window is away", "TowerTraineri aken on eemal"},
    {"Its simulation waits until the window is back; what you see here is from before.", "Simulatsioon ootab, kuni aken tagasi on; siin näed varasemat seisu."},
    {"Linked to TowerTrainer \xE2\x80\x93 waiting for its airport\xE2\x80\xA6", "TowerTrainer on ühendatud \xE2\x80\x93 ootan lennuvälja\xE2\x80\xA6"},
    {"Open an airport in TowerTrainer's window.", "Ava TowerTraineri aknas lennuväli."},
    {"Retry now", "Proovi kohe uuesti"}, {"Tower settings", "Torni seaded"}, {"Linked to TowerTrainer", "TowerTrainer on ühendatud"},
    {"Back into X-Plane", "Tagasi X-Plane'i"}, {"Pop out into its own window", "Ava eraldi aknas"},
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
