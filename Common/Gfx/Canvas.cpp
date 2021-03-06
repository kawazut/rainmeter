/* Copyright (C) 2013 Rainmeter Project Developers
 *
 * This Source Code Form is subject to the terms of the GNU General Public
 * License; either version 2 of the License, or (at your option) any later
 * version. If a copy of the GPL was not distributed with this file, You can
 * obtain one at <https://www.gnu.org/licenses/gpl-2.0.html>. */

#include "StdAfx.h"
#include "Canvas.h"
#include "TextFormatD2D.h"
#include "Util/D2DUtil.h"
#include "Util/DWriteFontCollectionLoader.h"
#include "Util/DWriteHelpers.h"
#include "Util/WICBitmapLockGDIP.h"
#include "../../Library/Util.h"

namespace Gfx {

UINT Canvas::c_Instances = 0;
Microsoft::WRL::ComPtr<ID2D1Factory1> Canvas::c_D2DFactory;
Microsoft::WRL::ComPtr<IDWriteFactory1> Canvas::c_DWFactory;
Microsoft::WRL::ComPtr<IDWriteGdiInterop> Canvas::c_DWGDIInterop;
Microsoft::WRL::ComPtr<IWICImagingFactory> Canvas::c_WICFactory;

Canvas::Canvas() :
	m_Bitmap(),
	m_TextAntiAliasing(false),
	m_CanUseAxisAlignClip(false)
{
	Initialize();
}

Canvas::~Canvas()
{
	Finalize();
}

bool Canvas::Initialize()
{
	++c_Instances;
	if (c_Instances == 1)
	{
		D2D1_FACTORY_OPTIONS fo = {};
#ifdef _DEBUG
		fo.debugLevel = D2D1_DEBUG_LEVEL_INFORMATION;
#endif

		HRESULT hr = D2D1CreateFactory(
			D2D1_FACTORY_TYPE_SINGLE_THREADED,
			fo,
			c_D2DFactory.GetAddressOf());
		if (FAILED(hr)) return false;

		hr = CoCreateInstance(
			CLSID_WICImagingFactory,
			nullptr,
			CLSCTX_INPROC_SERVER,
			IID_IWICImagingFactory,
			(LPVOID*)c_WICFactory.GetAddressOf());
		if (FAILED(hr)) return false;

		hr = DWriteCreateFactory(
			DWRITE_FACTORY_TYPE_SHARED,
			__uuidof(c_DWFactory),
			(IUnknown**)c_DWFactory.GetAddressOf());
		if (FAILED(hr)) return false;

		hr = c_DWFactory->GetGdiInterop(c_DWGDIInterop.GetAddressOf());
		if (FAILED(hr)) return false;

		hr = c_DWFactory->RegisterFontCollectionLoader(Util::DWriteFontCollectionLoader::GetInstance());
		if (FAILED(hr)) return false;
	}

	return true;
}

void Canvas::Finalize()
{
	--c_Instances;
	if (c_Instances == 0)
	{
		c_D2DFactory.Reset();
		c_WICFactory.Reset();
		c_DWGDIInterop.Reset();

		if (c_DWFactory)
		{
			c_DWFactory->UnregisterFontCollectionLoader(Util::DWriteFontCollectionLoader::GetInstance());
			c_DWFactory.Reset();
		}
	}
}

void Canvas::Resize(int w, int h)
{
	m_W = w;
	m_H = h;

	m_Target.Reset();

	m_Bitmap.Resize(w, h);

	m_GdipBitmap.reset(new Gdiplus::Bitmap(w, h, w * 4, PixelFormat32bppPARGB, m_Bitmap.GetData()));
	m_GdipGraphics.reset(new Gdiplus::Graphics(m_GdipBitmap.get()));
}

bool Canvas::BeginDraw()
{
	return true;
}

void Canvas::EndDraw()
{
	EndTargetDraw();
}

bool Canvas::BeginTargetDraw()
{
	if (m_Target) return true;

	const D2D1_PIXEL_FORMAT format = D2D1::PixelFormat(
		DXGI_FORMAT_B8G8R8A8_UNORM,
		D2D1_ALPHA_MODE_PREMULTIPLIED);

	const D2D1_RENDER_TARGET_PROPERTIES properties = D2D1::RenderTargetProperties(
		D2D1_RENDER_TARGET_TYPE_DEFAULT,
		format,
		0.0f,	// Default DPI
		0.0f,	// Default DPI
		D2D1_RENDER_TARGET_USAGE_GDI_COMPATIBLE);

	// A new Direct2D render target must be created for each sequence of Direct2D draw operations
	// since we use GDI+ to render to the same pixel data. Without creating a new render target
	// each time, it has been found that Direct2D may overwrite the draws by GDI+ since it is
	// unaware of the changes made by GDI+. By creating a new render target and then releasing it
	// before the next GDI+ draw operations, we ensure that the pixel data result is as expected
	// Once GDI+ drawing is no longer needed, we change to recreate the render target only when the
	// bitmap size is changed.
	HRESULT hr = c_D2DFactory->CreateWicBitmapRenderTarget(&m_Bitmap, properties, &m_Target);
	if (SUCCEEDED(hr))
	{
		SetTextAntiAliasing(m_TextAntiAliasing);

		m_Target->BeginDraw();

		// Apply any transforms that occurred before creation of |m_Target|.
		UpdateTargetTransform();

		return true;
	}

	return false;
}

void Canvas::EndTargetDraw()
{
	if (m_Target)
	{
		m_Target->EndDraw();
		m_Target.Reset();
	}
}

Gdiplus::Graphics& Canvas::BeginGdiplusContext()
{
	EndTargetDraw();
	return *m_GdipGraphics;
}

void Canvas::EndGdiplusContext()
{
}

HDC Canvas::GetDC()
{
	EndTargetDraw();

	HDC dcMemory = CreateCompatibleDC(nullptr);
	SelectObject(dcMemory, m_Bitmap.GetHandle());
	return dcMemory;
}

void Canvas::ReleaseDC(HDC dc)
{
	DeleteDC(dc);
}

bool Canvas::IsTransparentPixel(int x, int y)
{
	if (!(x >= 0 && y >= 0 && x < m_W && y < m_H)) return false;

	bool transparent = true;

	DWORD* data = (DWORD*)m_Bitmap.GetData();
	if (data)
	{
		DWORD pixel = data[y * m_W + x];  // Top-down DIB.
		transparent = (pixel & 0xFF000000) != 0;
	}

	return transparent;
}

void Canvas::UpdateTargetTransform()
{
	Gdiplus::Matrix gdipMatrix;
	m_GdipGraphics->GetTransform(&gdipMatrix);

	D2D1_MATRIX_3X2_F d2dMatrix;
	gdipMatrix.GetElements((Gdiplus::REAL*)&d2dMatrix);

	m_Target->SetTransform(d2dMatrix);
	m_CanUseAxisAlignClip =
		d2dMatrix._12 == 0.0f && d2dMatrix._21 == 0.0f &&
		d2dMatrix._31 == 0.0f && d2dMatrix._32 == 0.0f;
}

void Canvas::SetTransform(const Gdiplus::Matrix& matrix)
{
	m_GdipGraphics->SetTransform(&matrix);

	if (m_Target)
	{
		UpdateTargetTransform();
	}
}

void Canvas::ResetTransform()
{
	m_GdipGraphics->ResetTransform();

	if (m_Target)
	{
		m_Target->SetTransform(D2D1::Matrix3x2F::Identity());
	}
}

void Canvas::RotateTransform(float angle, float x, float y, float dx, float dy)
{
	m_GdipGraphics->TranslateTransform(x, y);
	m_GdipGraphics->RotateTransform(angle);
	m_GdipGraphics->TranslateTransform(dx, dy);

	if (m_Target)
	{
		UpdateTargetTransform();
	}
}

void Canvas::SetAntiAliasing(bool enable)
{
	// TODO: Set m_Target aliasing?
	m_GdipGraphics->SetSmoothingMode(
		enable ? Gdiplus::SmoothingModeHighQuality : Gdiplus::SmoothingModeNone);
	m_GdipGraphics->SetPixelOffsetMode(
		enable ? Gdiplus::PixelOffsetModeHighQuality : Gdiplus::PixelOffsetModeDefault);
}

void Canvas::SetTextAntiAliasing(bool enable)
{
	// TODO: Add support for D2D1_TEXT_ANTIALIAS_MODE_CLEARTYPE?
	m_TextAntiAliasing = enable;

	if (m_Target)
	{
		m_Target->SetTextAntialiasMode(
			m_TextAntiAliasing ? D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE : D2D1_TEXT_ANTIALIAS_MODE_ALIASED);
	}
}

void Canvas::Clear(const Gdiplus::Color& color)
{
	if (!m_Target)  // Use GDI+ if D2D render target has not been created.
	{
		m_GdipGraphics->Clear(color);
		return;
	}

	m_Target->Clear(Util::ToColorF(color));
}

void Canvas::DrawTextW(const std::wstring& srcStr, const TextFormat& format, Gdiplus::RectF& rect,
	const Gdiplus::SolidBrush& brush, bool applyInlineFormatting)
{
	if (!BeginTargetDraw()) return;

	Gdiplus::Color color;
	brush.GetColor(&color);

	Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> solidBrush;
	HRESULT hr = m_Target->CreateSolidColorBrush(Util::ToColorF(color), solidBrush.GetAddressOf());
	if (FAILED(hr)) return;

	TextFormatD2D& formatD2D = (TextFormatD2D&)format;

	static std::wstring str;
	str = srcStr;
	formatD2D.ApplyInlineCase(str);

	if (!formatD2D.CreateLayout(m_Target.Get(), str, rect.Width, rect.Height, !m_AccurateText && m_TextAntiAliasing)) return;

	D2D1_POINT_2F drawPosition;
	drawPosition.x = [&]()
	{
		if (!m_AccurateText)
		{
			const float xOffset = formatD2D.m_TextFormat->GetFontSize() / 6.0f;
			switch (formatD2D.GetHorizontalAlignment())
			{
			case HorizontalAlignment::Left: return rect.X + xOffset;
			case HorizontalAlignment::Right: return rect.X - xOffset;
			}
		}

		return rect.X;
	} ();

	drawPosition.y = [&]()
	{
		// GDI+ compatibility.
		float yPos = rect.Y - formatD2D.m_LineGap;
		switch (formatD2D.GetVerticalAlignment())
		{
		case VerticalAlignment::Bottom: yPos -= formatD2D.m_ExtraHeight; break;
		case VerticalAlignment::Center: yPos -= formatD2D.m_ExtraHeight / 2; break;
		}

		return yPos;
	} ();

	if (formatD2D.m_Trimming)
	{
		D2D1_RECT_F clipRect = Util::ToRectF(rect);

		if (m_CanUseAxisAlignClip)
		{
			m_Target->PushAxisAlignedClip(clipRect, D2D1_ANTIALIAS_MODE_ALIASED);
		}
		else
		{
			const D2D1_LAYER_PARAMETERS layerParams =
				D2D1::LayerParameters(clipRect, nullptr, D2D1_ANTIALIAS_MODE_ALIASED);
			m_Target->PushLayer(layerParams, nullptr);
		}
	}

	// When different "effects" are used with inline coloring options, we need to
	// remove the previous inline coloring, then reapply them (if needed) - instead
	// of destroying/recreating the text layout.
	UINT32 strLen = (UINT32)str.length();
	formatD2D.ResetInlineColoring(solidBrush.Get(), strLen);
	if (applyInlineFormatting)
	{
		formatD2D.ApplyInlineColoring(m_Target.Get(), &drawPosition);

		// Draw any 'shadow' effects
		formatD2D.ApplyInlineShadow(m_Target.Get(), solidBrush.Get(), strLen, drawPosition);
	}

	m_Target->DrawTextLayout(drawPosition, formatD2D.m_TextLayout.Get(), solidBrush.Get());

	if (applyInlineFormatting)
	{
		// Inline gradients require the drawing position, so in case that position
		// changes, we need a way to reset it after drawing time so on the next
		// iteration it will know the correct position.
		formatD2D.ResetGradientPosition(&drawPosition);
	}

	if (formatD2D.m_Trimming)
	{
		if (m_CanUseAxisAlignClip)
		{
			m_Target->PopAxisAlignedClip();
		}
		else
		{
			m_Target->PopLayer();
		}
	}
}

bool Canvas::MeasureTextW(const std::wstring& str, const TextFormat& format, Gdiplus::RectF& rect)
{
	TextFormatD2D& formatD2D = (TextFormatD2D&)format;

	static std::wstring formatStr;
	formatStr = str;
	formatD2D.ApplyInlineCase(formatStr);

	const DWRITE_TEXT_METRICS metrics = formatD2D.GetMetrics(formatStr, !m_AccurateText);
	rect.Width = metrics.width;
	rect.Height = metrics.height;
	return true;
}

bool Canvas::MeasureTextLinesW(const std::wstring& str, const TextFormat& format, Gdiplus::RectF& rect, UINT& lines)
{
	TextFormatD2D& formatD2D = (TextFormatD2D&)format;
	formatD2D.m_TextFormat->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);

	static std::wstring formatStr;
	formatStr = str;
	formatD2D.ApplyInlineCase(formatStr);

	const DWRITE_TEXT_METRICS metrics = formatD2D.GetMetrics(formatStr, !m_AccurateText, rect.Width);
	rect.Width = metrics.width;
	rect.Height = metrics.height;
	lines = metrics.lineCount;

	if (rect.Height > 0.0f)
	{
		// GDI+ draws multi-line text even though the last line may be clipped slightly at the
		// bottom. This is a workaround to emulate that behaviour.
		rect.Height += 1.0f;
	}
	else
	{
		// GDI+ compatibility: Zero height text has no visible lines.
		lines = 0;
	}
	return true;
}

void Canvas::DrawBitmap(Gdiplus::Bitmap* bitmap, const Gdiplus::Rect& dstRect, const Gdiplus::Rect& srcRect)
{
	if (srcRect.Width != dstRect.Width || srcRect.Height != dstRect.Height)
	{
		// If the bitmap needs to be scaled, get rid of the D2D target and use the GDI+ code path
		// to draw the bitmap. This is due to antialiasing differences between GDI+ and D2D on
		// scaled bitmaps.
		EndTargetDraw();
	}

	if (!m_Target)  // Use GDI+ if D2D render target has not been created.
	{
		m_GdipGraphics->DrawImage(
			bitmap, dstRect, srcRect.X, srcRect.Y, srcRect.Width, srcRect.Height, Gdiplus::UnitPixel);
		return;
	}

	// The D2D DrawBitmap seems to perform exactly like Gdiplus::Graphics::DrawImage since we are
	// not using a hardware accelerated render target. Nevertheless, we will use it to avoid
	// the EndDraw() call needed for GDI+ drawing.
	Util::WICBitmapLockGDIP* bitmapLock = new Util::WICBitmapLockGDIP();
	Gdiplus::Rect lockRect(0, 0, bitmap->GetWidth(), bitmap->GetHeight());
	Gdiplus::Status status = bitmap->LockBits(
		&lockRect, Gdiplus::ImageLockModeRead, PixelFormat32bppPARGB, bitmapLock->GetBitmapData());
	if (status == Gdiplus::Ok)
	{
		D2D1_BITMAP_PROPERTIES props = D2D1::BitmapProperties(
			D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
		Microsoft::WRL::ComPtr<ID2D1Bitmap> d2dBitmap;
		HRESULT hr = m_Target->CreateSharedBitmap(
			__uuidof(IWICBitmapLock), bitmapLock, &props, d2dBitmap.GetAddressOf());
		if (SUCCEEDED(hr))
		{
			auto rDst = Util::ToRectF(dstRect);
			auto rSrc = Util::ToRectF(srcRect);
			m_Target->DrawBitmap(d2dBitmap.Get(), rDst, 1.0F, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR, rSrc);
		}

		// D2D will still use the pixel data after this call (at the next Flush() or EndDraw()).
		bitmap->UnlockBits(bitmapLock->GetBitmapData());
	}

	bitmapLock->Release();
}

void Canvas::DrawMaskedBitmap(Gdiplus::Bitmap* bitmap, Gdiplus::Bitmap* maskBitmap, const Gdiplus::Rect& dstRect,
	const Gdiplus::Rect& srcRect, const Gdiplus::Rect& srcRect2)
{
	if (!BeginTargetDraw()) return;

	auto rDst = Util::ToRectF(dstRect);
	auto rSrc = Util::ToRectF(srcRect);

	Util::WICBitmapLockGDIP* bitmapLock = new Util::WICBitmapLockGDIP();
	Gdiplus::Rect lockRect(srcRect2);
	Gdiplus::Status status = bitmap->LockBits(
		&lockRect, Gdiplus::ImageLockModeRead, PixelFormat32bppPARGB, bitmapLock->GetBitmapData());
	if (status == Gdiplus::Ok)
	{
		D2D1_BITMAP_PROPERTIES props = D2D1::BitmapProperties(
			D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
		Microsoft::WRL::ComPtr<ID2D1Bitmap> d2dBitmap;
		HRESULT hr = m_Target->CreateSharedBitmap(
			__uuidof(IWICBitmapLock), bitmapLock, &props, d2dBitmap.GetAddressOf());
		if (SUCCEEDED(hr))
		{
			// Create bitmap brush from original |bitmap|.
			Microsoft::WRL::ComPtr<ID2D1BitmapBrush> brush;
			D2D1_BITMAP_BRUSH_PROPERTIES propertiesXClampYClamp = D2D1::BitmapBrushProperties(
				D2D1_EXTEND_MODE_CLAMP,
				D2D1_EXTEND_MODE_CLAMP,
				D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);

			// "Move" and "scale" the |bitmap| to match the destination.
			D2D1_MATRIX_3X2_F translate = D2D1::Matrix3x2F::Translation(rDst.left, rDst.top);
			D2D1_MATRIX_3X2_F scale = D2D1::Matrix3x2F::Scale(
				D2D1::SizeF((rDst.right - rDst.left) / (float)srcRect2.Width, (rDst.bottom - rDst.top) / (float)srcRect2.Height));
			D2D1_BRUSH_PROPERTIES brushProps = D2D1::BrushProperties(1.0F, scale * translate);

			hr = m_Target->CreateBitmapBrush(
				d2dBitmap.Get(),
				propertiesXClampYClamp,
				brushProps,
				brush.GetAddressOf());

			// Load the |maskBitmap| and use the bitmap brush to "fill" its contents.
			// Note: The image must be aliased when applying the opacity mask.
			if (SUCCEEDED(hr))
			{
				Util::WICBitmapLockGDIP* maskBitmapLock = new Util::WICBitmapLockGDIP();
				Gdiplus::Rect maskLockRect(0, 0, maskBitmap->GetWidth(), maskBitmap->GetHeight());
				status = maskBitmap->LockBits(
					&maskLockRect, Gdiplus::ImageLockModeRead, PixelFormat32bppPARGB, maskBitmapLock->GetBitmapData());
				if (status == Gdiplus::Ok)
				{
					D2D1_BITMAP_PROPERTIES maskProps = D2D1::BitmapProperties(
						D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
					Microsoft::WRL::ComPtr<ID2D1Bitmap> d2dMaskBitmap;
					hr = m_Target->CreateSharedBitmap(
						__uuidof(IWICBitmapLock), maskBitmapLock, &props, d2dMaskBitmap.GetAddressOf());
					if (SUCCEEDED(hr))
					{
						m_Target->SetAntialiasMode(D2D1_ANTIALIAS_MODE_ALIASED); // required
						m_Target->FillOpacityMask(
							d2dMaskBitmap.Get(),
							brush.Get(),
							D2D1_OPACITY_MASK_CONTENT_GRAPHICS,
							&rDst,
							&rSrc);
						m_Target->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
					}

					maskBitmap->UnlockBits(bitmapLock->GetBitmapData());
				}

				maskBitmapLock->Release();
			}
		}

		bitmap->UnlockBits(bitmapLock->GetBitmapData());
	}

	bitmapLock->Release();
}

void Canvas::FillRectangle(Gdiplus::Rect& rect, const Gdiplus::SolidBrush& brush)
{
	if (!m_Target)  // Use GDI+ if D2D render target has not been created.
	{
		m_GdipGraphics->FillRectangle(&brush, rect);
		return;
	}

	Gdiplus::Color color;
	brush.GetColor(&color);

	Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> solidBrush;
	HRESULT hr = m_Target->CreateSolidColorBrush(Util::ToColorF(color), solidBrush.GetAddressOf());
	if (SUCCEEDED(hr))
	{
		m_Target->FillRectangle(Util::ToRectF(rect), solidBrush.Get());
	}
}

void Canvas::DrawGeometry(Shape& shape, int xPos, int yPos)
{
	if (!BeginTargetDraw()) return;

	D2D1_MATRIX_3X2_F worldTransform;
	m_Target->GetTransform(&worldTransform);
	m_Target->SetTransform(shape.GetShapeMatrix() *
		worldTransform *
		D2D1::Matrix3x2F::Translation((FLOAT)xPos, (FLOAT)yPos));

	Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> solidBrush;
	HRESULT hr = m_Target->CreateSolidColorBrush(shape.m_FillColor, solidBrush.GetAddressOf());
	if (SUCCEEDED(hr))
	{
		if (shape.m_FillColor.a > 0.0f)
		{
			m_Target->FillGeometry(shape.m_Shape.Get(), solidBrush.Get());
		}

		solidBrush->SetColor(shape.m_StrokeColor);
		if (shape.m_StrokeColor.a > 0.0f && shape.m_StrokeWidth > 0.0f)
		{
			m_Target->DrawGeometry(shape.m_Shape.Get(), solidBrush.Get(), shape.m_StrokeWidth, nullptr);
		}
	}

	m_Target->SetTransform(worldTransform);
}

}  // namespace Gfx
