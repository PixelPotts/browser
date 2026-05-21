#pragma once
#include "simulators.h"
#include <vector>
#include <string>

void initDisplay();
void cleanupDisplay();
int  getKey();

void renderDisplay(
    const std::vector<State>& states,
    const std::vector<std::string>& log,
    int leftScroll,
    int& rightScroll,
    int64_t startMs);
