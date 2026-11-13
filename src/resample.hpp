#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

std::vector<std::int16_t> ResampleTo16k(const std::int16_t* samples, std::size_t count, int source_rate);
