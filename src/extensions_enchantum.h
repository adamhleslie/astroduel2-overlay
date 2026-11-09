#pragma once

#include <algorithm>
#include <iterator>
#include <ranges>
#include <string>
#include <type_traits>

#include "enchantum/common.hpp"
#include "enchantum/enchantum.hpp"

// Extension functions for the enchantum library
namespace ext::enchantum
{
	template<::enchantum::Enum E>
	E cast_or(std::underlying_type_t<E> value, E&& defaultValue)
	{
		return ::enchantum::cast<E>(value).value_or(defaultValue);
	}

	template<::enchantum::Enum E>
	std::string to_string_lower(const E value)
	{
		std::string name;
		std::ranges::transform(::enchantum::to_string(value), std::back_inserter(name),
			[](const unsigned char c) { return std::tolower(c); });
		return name;
	}

	template<::enchantum::Enum E>
	std::optional<E> cast_lower(const std::string& name)
	{
		return ::enchantum::cast<E>(name,[](const char a, const char b)
		{
			return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
		});
	}

	template<::enchantum::Enum E>
	E cast_lower_or(const std::string& name, E&& defaultValue)
	{
		return cast_lower<E>(name).value_or(defaultValue);
	}

	namespace details
	{
		template<::enchantum::Enum E>
		constexpr auto get_names_c_str() noexcept
		{
			constexpr auto names = ::enchantum::details::get_names<E, std::string_view, true>();
			std::array<const char*, std::size(names)> ret{};
			for (const auto [i, name] : names | std::ranges::views::enumerate)
			{
				ret[i] = name.data();
			}
			return ret;
		}
	}

	template<::enchantum::Enum E>
	constexpr auto names_c_str = details::get_names_c_str<E>();
}
