#pragma once
#include <string>
#include <vector>

namespace harness {

// One captured XPLMDrawString call: the text and where it was drawn.
struct Drawn {
    std::string text;
    int x = 0, y = 0;
};

void   set(const std::string& dataref, double value);
double get(const std::string& dataref);
void   setString(const std::string& dataref, const std::string& value);   // byte-array refs
std::string getString(const std::string& dataref);

void tick(float dt);                      // run one flight-loop callback
std::vector<std::string> draw();          // run the window draw, capture its text
void ptt(bool down);                      // press / release the PTT command
int  simKey(int vk, bool down);           // a key with no window focused (key sniffer)
void menu(int item);                      // click a plugin menu item

// second window (settings) -- id 2 in creation order
std::vector<std::string> drawWindow(int id);
bool windowVisible(int id);
void click(int id, int x, int y);
void typeText(int id, const std::string& text);
void pressVk(int id, int vk);
void windowTop(int id, int* top, int* left);
void windowRect(int id, int* l, int* t, int* r, int* b);
void resetWindows();
void setScreen(int l, int t, int r, int b);
void clearMonitors();
void addMonitor(int l, int t, int r, int b);

bool drawnContains(const std::vector<std::string>& lines, const std::string& needle);

// Position of a line from the most recent draw()/drawWindow() call.
bool drawnAt(const std::string& needle, int* x, int* y);
const std::vector<Drawn>& drawnPositions();
const std::vector<std::string>& log();

extern bool g_verbose;

}  // namespace harness
