// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include <algorithm>
#include <cmath>
#include <d2d1.h>

namespace Microsoft::Console::Render::Atlas
{
    inline constexpr unsigned GLYPH_CELL_FIT_KEY_SHIFT = 16;

    /** @brief Baseline-local scale and translation for one isolated glyph's ink. */
    struct GlyphCellFit
    {
        /** @brief Horizontal scale factor; never greater than one. */
        float scaleX = 1.0f;
        /** @brief Vertical scale factor, equal to the horizontal factor. */
        float scaleY = 1.0f;
        /** @brief Horizontal baseline translation in logical pixels. */
        float offsetX = 0.0f;
        /** @brief Vertical baseline translation in logical pixels. */
        float offsetY = 0.0f;
    };

    /**
     * @brief Fit one isolated glyph into its columns without clipping or enlargement.
     * @param bounds Raw baseline-local ink bounds, before placement and line rendition.
     * @param allocatedWidth Horizontal space in logical pixels, excluding line rendition.
     * @return Scale and baseline translation that center the ink horizontally with an
     * antialiasing margin. Empty, invalid or non-finite bounds return the identity fit.
     */
    inline GlyphCellFit fitGlyphToCell(const D2D1_RECT_F& bounds, const float allocatedWidth) noexcept
    {
        const auto inkWidth = bounds.right - bounds.left;
        if (!(inkWidth > 0.0f) || !(allocatedWidth > 0.0f) ||
            !std::isfinite(inkWidth) || !std::isfinite(allocatedWidth) ||
            !std::isfinite(bounds.left) || !std::isfinite(bounds.top) || !std::isfinite(bounds.bottom) ||
            !(bounds.bottom > bounds.top))
        {
            return {};
        }

        constexpr float antialiasingMargin = 1.0f;
        const auto margin = (std::min)(antialiasingMargin, allocatedWidth * 0.25f);
        const auto scaleX = (std::min)(1.0f, (allocatedWidth - 2.0f * margin) / inkWidth);
        const auto scaleY = scaleX;
        return {
            .scaleX = scaleX,
            .scaleY = scaleY,
            .offsetX = (allocatedWidth - inkWidth * scaleX) * 0.5f - bounds.left * scaleX,
            .offsetY = (1.0f - scaleY) * (bounds.top * 0.5f + bounds.bottom * 0.5f),
        };
    }
}
