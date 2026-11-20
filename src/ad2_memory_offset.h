#pragma once

#include "ad2.h"

#include <format>
#include <cassert>

namespace ad2
{
#pragma region Helpers
	inline uintptr_t GetOffset(const void* base, const void* address)
	{
		assert(base != nullptr);
		const uintptr_t baseInt = reinterpret_cast<uintptr_t>(base);
		const uintptr_t addressInt = reinterpret_cast<uintptr_t>(address);
		assert(addressInt >= baseInt);
		return (addressInt - baseInt);
	}

	template<typename T = void>
	T* GetAddress(void* base, uintptr_t offset)
	{
		assert(base != nullptr);
		return reinterpret_cast<T*>(reinterpret_cast<uintptr_t>(base) + offset);
	}
#pragma endregion

	// Identifies a memory offset within the ad2 module
	// Note: Backed by unsigned type, asserts if underflow in an arithmetic operation goes below
	// the base memory address for the module
	struct MemoryOffset
	{
		explicit constexpr MemoryOffset(uintptr_t offset) : offset(offset) {}
		explicit MemoryOffset(const void* address) : offset(GetOffset(ad2Module, address)) {}

		constexpr explicit operator uintptr_t() const { return offset; }

		template<typename T = void>
		T* ToAddress() const { return GetAddress<T>(ad2Module, offset); }

		// Arithmetic operators - assert on arithmetic that produces a negative value
		constexpr MemoryOffset operator+(std::ptrdiff_t delta) const
		{
			assert((delta >= 0) || (offset >= static_cast<uintptr_t>(-delta)));
			return MemoryOffset{offset + delta};
		}
		constexpr MemoryOffset operator-(std::ptrdiff_t delta) const
		{
			assert((delta <= 0) || (offset >= static_cast<uintptr_t>(delta)));
			return MemoryOffset{offset - delta};
		}
		constexpr MemoryOffset& operator++() { return *this += 1; }
		constexpr MemoryOffset& operator--() { return *this -= 1; }
		constexpr MemoryOffset operator++(int)
		{
			const MemoryOffset initial = *this;
			offset += 1;
			return initial;
		}
		constexpr MemoryOffset operator--(int)
		{
			assert(offset > 0);
			const MemoryOffset initial = *this;
			offset -= 1;
			return initial;
		}
		constexpr MemoryOffset& operator+=(std::ptrdiff_t delta)
		{
			assert((delta >= 0) || (offset >= static_cast<uintptr_t>(-delta)));
			offset += delta;
			return *this;
		}
		constexpr MemoryOffset& operator-=(std::ptrdiff_t delta)
		{
			assert((delta <= 0) || (offset >= static_cast<uintptr_t>(delta)));
			offset -= delta;
			return *this;
		}

		// Relational operators
		friend constexpr bool operator==(MemoryOffset a, MemoryOffset b) { return a.offset == b.offset; }
		friend constexpr auto operator<=>(MemoryOffset a, MemoryOffset b) { return a.offset <=> b.offset; }

		uintptr_t offset;
	};
}

template<>
struct std::formatter<ad2::MemoryOffset> : std::formatter<std::string>
{
	auto format(const ad2::MemoryOffset& address, std::format_context& ctx) const
	{
		return std::formatter<string>::format(std::format("{:#010x}", address.offset), ctx);
	}
};