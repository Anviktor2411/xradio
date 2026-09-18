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

bool drawnContains(const std::vector<std::string>& lines, const std::string& needle);
const std::vector<std::string>& log();

extern bool g_verbose;

}  // namespace harness
