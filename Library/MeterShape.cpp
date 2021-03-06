/* Copyright (C) 2016 Rainmeter Project Developers
 *
 * This Source Code Form is subject to the terms of the GNU General Public
 * License; either version 2 of the License, or (at your option) any later
 * version. If a copy of the GPL was not distributed with this file, You can
 * obtain one at <https://www.gnu.org/licenses/gpl-2.0.html>. */

#include "StdAfx.h"
#include "MeterShape.h"
#include "Logger.h"
#include "../Common/StringUtil.h"
#include "../Common/Gfx/Shape.h"
#include "../Common/Gfx/Shapes/Rectangle.h"
#include "../Common/Gfx/Shapes/RoundedRectangle.h"

MeterShape::MeterShape(Skin* skin, const WCHAR* name) : Meter(skin, name),
	m_Shapes()
{
	Meter::Initialize();
}

MeterShape::~MeterShape()
{
	Dispose();
}

void MeterShape::Dispose()
{
	for (auto& shape : m_Shapes)
	{
		delete shape;
	}

	m_Shapes.clear();
}

void MeterShape::ReadOptions(ConfigParser& parser, const WCHAR* section)
{
	Meter::ReadOptions(parser, section);

	// Clear any shapes
	Dispose();

	std::unordered_map<size_t, std::wstring> combinedShapes;

	const std::wstring delimiter(1, L'|');
	std::wstring shape = parser.ReadString(section, L"Shape", L"");

	size_t i = 1;
	while (!shape.empty())
	{
		std::vector<std::wstring> args = ConfigParser::Tokenize(shape, delimiter);

		bool isCombined = false;
		if (!CreateShape(args, isCombined, i - 1)) break;

		// If the shape is combined with another, save the shape definition and
		// process later. Otherwise, parse any modifiers for the shape.
		if (isCombined)
		{
			combinedShapes.insert(std::make_pair(i - 1, shape));
		}
		else
		{
			args.erase(args.begin());
			ParseModifiers(args, parser, section);
		}

		D2D1_RECT_F bounds = m_Shapes.back()->GetBounds();
		if (!m_WDefined && m_W < bounds.right)
		{
			m_W = (int)bounds.right;
		}
		if (!m_HDefined && m_H < bounds.bottom)
		{
			m_H = (int)bounds.bottom;
		}

		// Check for Shape2 ... etc.
		const std::wstring num = std::to_wstring(++i);
		std::wstring key = L"Shape" + num;
		shape = parser.ReadString(section, key.c_str(), L"");
	}

	// Process combined shapes
	for (const auto& shape : combinedShapes)
	{
		std::vector<std::wstring> args = ConfigParser::Tokenize(shape.second, delimiter);
		if (!CreateCombinedShape(shape.first, args)) break;
	}
}

bool MeterShape::Update()
{
	if (Meter::Update())
	{
		return true;
	}

	return false;
}

bool MeterShape::Draw(Gfx::Canvas& canvas)
{
	if (!Meter::Draw(canvas)) return false;

	int x = Meter::GetX();
	int y = Meter::GetY();

	for (const auto& shape : m_Shapes)
	{
		if (!shape->IsCombined())
		{
			canvas.DrawGeometry(*shape, x, y);
		}
	}

	return true;
}

bool MeterShape::HitTest(int x, int y)
{
	D2D1_POINT_2F point = { (FLOAT)(x - Meter::GetX()), (FLOAT)(y - Meter::GetY()) };
	for (auto& shape : m_Shapes)
	{
		if (!shape->IsCombined() && shape->ContainsPoint(point))
		{
			return true;
		}
	}

	return false;
}

void MeterShape::BindMeasures(ConfigParser& parser, const WCHAR* section)
{
	if (BindPrimaryMeasure(parser, section, true))
	{
		BindSecondaryMeasures(parser, section);
	}
}

bool MeterShape::CreateShape(std::vector<std::wstring>& args, bool& isCombined, size_t keyId)
{
	auto createShape = [&](Gfx::Shape* shape) -> bool
	{
		std::wstring id = keyId == 0 ? L"" : std::to_wstring(keyId);
		bool exists = shape->DoesShapeExist();
		if (exists)
		{
			m_Shapes.push_back(shape);
		}
		else
		{
			LogErrorF(this, L"Could not create shape: Shape%s", id.c_str());
			delete shape;
		}

		return exists;
	};

	const size_t argSize = args.size();
	const WCHAR* shapeName = args[0].c_str();
	if (_wcsnicmp(shapeName, L"RECTANGLE", 9) == 0)
	{
		shapeName += 9;
		auto tokens = ConfigParser::Tokenize2(shapeName, L',', PairedPunctuation::Parentheses);
		auto tokSize = tokens.size();

		if (tokSize == 4)
		{
			FLOAT x = (FLOAT)ConfigParser::ParseInt(tokens[0].c_str(), 0);
			FLOAT y = (FLOAT)ConfigParser::ParseInt(tokens[1].c_str(), 0);
			FLOAT w = (FLOAT)ConfigParser::ParseInt(tokens[2].c_str(), 0);
			FLOAT h = (FLOAT)ConfigParser::ParseInt(tokens[3].c_str(), 0);

			if (!createShape(new Gfx::Rectangle(x, y, w, h)))
			{
				return false;
			}

			return true;
		}
		else if (tokSize > 4)
		{
			FLOAT x = (FLOAT)ConfigParser::ParseInt(tokens[0].c_str(), 0);
			FLOAT y = (FLOAT)ConfigParser::ParseInt(tokens[1].c_str(), 0);
			FLOAT w = (FLOAT)ConfigParser::ParseInt(tokens[2].c_str(), 0);
			FLOAT h = (FLOAT)ConfigParser::ParseInt(tokens[3].c_str(), 0);
			FLOAT xRadius = (FLOAT)ConfigParser::ParseInt(tokens[4].c_str(), 0);
			FLOAT yRadius = (tokSize > 5) ?
				yRadius = (FLOAT)ConfigParser::ParseInt(tokens[5].c_str(), 0) :
				xRadius;

			if (!createShape(new Gfx::RoundedRectangle(x, y, w, h, xRadius, yRadius)))
			{
				return false;
			}

			return true;
		}
		else
		{
			LogErrorF(this, L"Rectangle has too few parameters");
			return false;
		}
	}
	// Add new shapes here
	//else if (_wcsnicmp(shapeName, L"", ) == 0)
	//{
	//}
	else if (_wcsnicmp(shapeName, L"COMBINE", 7) == 0)
	{
		// Combined shapes are processed after all shapes are created
		isCombined = true;
		return true;
	}

	LogErrorF(this, L"Invalid shape: %s", shapeName);
	return false;
}

bool MeterShape::CreateCombinedShape(size_t shapeId, std::vector<std::wstring>& args)
{
	auto showError = [&shapeId, this](const WCHAR* description, const WCHAR* error) -> void
	{
		std::wstring key = L"Shape";
		key += std::to_wstring(shapeId + 1);
		LogErrorF(this, L"%s %s \"%s\"", key.c_str(), description, error);
	};

	auto getShapeId = [=](const WCHAR* shape) -> size_t
	{
		int id = _wtoi(shape) - 1;
		return id < 0 ? (size_t)0 : (size_t)id;
	};

	size_t parentId = 0;

	const WCHAR* parentName = args[0].c_str();
	parentName += 8;  // Strip off 'Combine '
	if (_wcsnicmp(parentName, L"SHAPE", 5) == 0)
	{
		parentName += 5;  // Strip off 'Shape'
		parentId = getShapeId(parentName);

		if (parentId == shapeId)
		{
			// Cannot use myself as a parent shape
			showError(L"cannot combine with:", parentName - 5);
			return false;
		}

		if (parentId < m_Shapes.size())
		{
			Gfx::Shape* clonedShape = m_Shapes[parentId]->Clone();
			if (clonedShape)
			{
				m_Shapes.insert(m_Shapes.begin() + shapeId, clonedShape);
				m_Shapes[parentId]->SetCombined();

				// Combine with empty shape
				m_Shapes[shapeId]->CombineWith(nullptr, D2D1_COMBINE_MODE_UNION);
			}
			else
			{
				// Shape could not be cloned
				return false;
			}
		}
		else
		{
			showError(L"definition contains invalid shape reference: ", parentName - 5);
			return false;
		}
	}
	else
	{
		showError(L"defintion contains invalid shape identifier: ", parentName);
		return false;
	}

	args.erase(args.begin());  // Remove Combine definition

	for (const auto& option : args)
	{
		D2D1_COMBINE_MODE mode = D2D1_COMBINE_MODE_FORCE_DWORD;
		const WCHAR* combined = option.c_str();
		if (_wcsnicmp(combined, L"UNION", 5) == 0)
		{
			combined += 6;
			mode = D2D1_COMBINE_MODE_UNION;
		}
		else if (_wcsnicmp(combined, L"XOR", 3) == 0)
		{
			combined += 4;
			mode = D2D1_COMBINE_MODE_XOR;
		}
		else if (_wcsnicmp(combined, L"INTERSECT", 9) == 0)
		{
			combined += 10;
			mode = D2D1_COMBINE_MODE_INTERSECT;
		}
		else if (_wcsnicmp(combined, L"EXCLUDE", 7) == 0)
		{
			combined += 8;
			mode = D2D1_COMBINE_MODE_EXCLUDE;
		}
		else
		{
			showError(L"definition contains invalid combine: ", combined);
			return false;
		}

		combined += 5;  // Remove 'Shape'
		size_t id = getShapeId(combined);
		if (id == shapeId)
		{
			// Cannot combine with myself
			showError(L"cannot combine with:", combined - 5);
			return false;
		}

		if (id < m_Shapes.size())
		{
			m_Shapes[id]->SetCombined();

			if (!m_Shapes[shapeId]->CombineWith(m_Shapes[id], mode))
			{
				showError(L"could not combine with: ", combined - 5);
				return false;
			}
		}
		else
		{
			showError(L"defintion contains invalid shape identifier: ", combined - 5);
			return false;
		}
	}

	return true;
}

void MeterShape::ParseModifiers(std::vector<std::wstring>& args, ConfigParser& parser, const WCHAR* section, bool recursive)
{
	auto& shape = m_Shapes.back();

	for (const auto& option : args)
	{
		const WCHAR* modifier = option.c_str();

		if (_wcsnicmp(modifier, L"FILLCOLOR", 9) == 0)
		{
			modifier += 9;
			auto color = ConfigParser::ParseColor(modifier);
			shape->SetFillColor(color);
			continue;
		}
		else if (_wcsnicmp(modifier, L"STROKECOLOR", 11) == 0)
		{
			modifier += 11;
			auto color = ConfigParser::ParseColor(modifier);
			shape->SetStrokeColor(color);
			continue;
		}
		else if (_wcsnicmp(modifier, L"STROKEWIDTH", 11) == 0)
		{
			modifier += 11;
			int width = ConfigParser::ParseInt(modifier, 0);
			if (width < 0)
			{
				LogWarningF(this, L"StrokeWidth must not be negative");
				width = 0;
			}

			shape->SetStrokeWidth(width);
			continue;
		}
		else if (_wcsnicmp(modifier, L"OFFSET", 6) == 0)
		{
			modifier += 6;
			auto offset = ConfigParser::Tokenize2(modifier, L',', PairedPunctuation::Parentheses);
			if (offset.size() >= 2)
			{
				int x = ConfigParser::ParseInt(offset[0].c_str(), 0);
				int y = ConfigParser::ParseInt(offset[1].c_str(), 0);
				shape->SetOffset(x, y);
			}
			else
			{
				LogErrorF(this, L"Offset has too few parameters");
			}
			continue;
		}
		else if (_wcsnicmp(modifier, L"ROTATE", 6) == 0)
		{
			modifier += 6;
			auto rotate = ConfigParser::Tokenize2(modifier, L',', PairedPunctuation::Parentheses);
			size_t size = rotate.size();
			if (size > 0)
			{
				bool anchorDefined = false;
				FLOAT anchorX = 0.0f;
				FLOAT anchorY = 0.0f;
				FLOAT rotation = (FLOAT)ConfigParser::ParseInt(rotate[0].c_str(), 0);
				if (size > 2)
				{
					anchorX = (FLOAT)ConfigParser::ParseInt(rotate[1].c_str(), 0);
					anchorY = (FLOAT)ConfigParser::ParseInt(rotate[2].c_str(), 0);
					anchorDefined = true;
				}

				shape->SetRotation(rotation, anchorX, anchorY, anchorDefined);
			}
			else
			{
				LogWarningF(this, L"Rotate has too few parameters");
			}
			continue;
		}
		// Add new modifiers here
		//else if (_wcsnicmp(modifier, L"", ) == 0)
		//{
		//}
		else if (_wcsnicmp(modifier, L"EXTEND", 6) == 0)
		{
			modifier += 6;
			if (!recursive)
			{
				std::vector<std::wstring> extendParameters = ConfigParser::Tokenize(modifier, L",");
				for (auto& extend : extendParameters)
				{
					std::wstring key = parser.ReadString(section, extend.c_str(), L"");
					if (!key.empty())
					{
						std::vector<std::wstring> newArgs = ConfigParser::Tokenize(key, L"|");
						ParseModifiers(newArgs, parser, section, true);
					}
				}
			}
			else
			{
				LogErrorF(this, L"Extend cannot be used recursively");
			}
			continue;
		}
		else
		{
			LogErrorF(this, L"Invalid shape modifier: %s", modifier);
			continue;
		}
	}
}
