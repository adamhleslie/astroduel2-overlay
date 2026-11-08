#include "ad2.h"

#include <algorithm>
#include <iterator>
#include <string>

#include "mod.h"

ad2::WorldType ad2::ToWorldType(std::underlying_type_t<WorldType> underlyingType)
{
	auto typeOpt = enchantum::cast<WorldType>(underlyingType);
	return (typeOpt ? *typeOpt : WorldType::Invalid);
}

std::string ad2::WorldTypeToName(WorldType type)
{
	std::string name;
	std::ranges::transform(enchantum::to_string(type), std::back_inserter(name),
						   [](unsigned char c) { return std::tolower(c); });
	return name;
}

ad2::WorldType ad2::WorldNameToType(const std::string& name)
{
	auto typeOpt = enchantum::cast<WorldType>(name,[](char a, char b)
	{
		return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
	});

	return (typeOpt ? *typeOpt : WorldType::Invalid);
}