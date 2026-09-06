#pragma once
#include <stdint.h>

inline bool SaveBlockFits(uint32_t declared, uint32_t capacity)
{
	// Check before alignment, so corrupt large sizes cannot wrap to zero.
	return declared <= capacity && declared <= UINT32_MAX-3 &&
		((declared+3U)&~3U) <= capacity;
}
