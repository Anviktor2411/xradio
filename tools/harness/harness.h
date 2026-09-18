#pragma once
#include <string>
#include <vector>

namespace harness {

void   set(const std::string& dataref, double value);
double get(const std::string& dataref);

void tick(float dt);                      // run one flight-loop callback
std::vector<std::string> draw();          // run the window draw, capture its text
void ptt(bool down);                      // press / release the PTT command
void menu(int item);                      // click a plugin menu item

// second window (settings) -- id 2 in creation order
std::vector<std::string> drawWindow(int id);
bool windowVisible(int id);
void click(int id, int x, int y);
void typeText(int id, const std::string& text);
void pressVk(int id, int vk);
void windowTop(int id, int* top, int* left);

bool drawnContains(const std::vector<std::string>& lines, const std::string& needle);
const std::vector<std::string>& log();

extern bool g_verbose;

}  // namespace harness
