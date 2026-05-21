#pragma once
#include "simulators.h"
#include <vector>
#include <memory>

enum class SimMode { DEFAULT, NORMAL1 };

std::vector<std::unique_ptr<Sim>> createIntersection(SimMode mode = SimMode::NORMAL1);

// Returns "OK", "CPU_CRASH", "ALL_CLEARED", or "NOT_FOUND"
std::string injectFaultById(std::vector<std::unique_ptr<Sim>>& sims,
                             const std::string& id, const std::string& type);
