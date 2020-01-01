#pragma once

/*
 * PS4Delta : PS4 emulation and research project
 *
 * Copyright 2019-2020 Force67.
 * For information regarding licensing see LICENSE
 * in the root of the source tree.
 */

#include "device.h"

namespace krnl
{
	class proc;

	class gcDevice : public device
	{
	public:
		gcDevice(proc*);

		bool init(const char*, uint32_t, uint32_t) override;
		uint8_t* map(void*, size_t, uint32_t, uint32_t, size_t) override;
	};
}