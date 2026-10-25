// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "precomp.h"
#include "WexTestClass.h"
#include "../../../types/inc/CodepointWidthDetector.hpp"
#include "../GlyphCellFit.h"
#include "../AtlasEngine.h"
#include "../../inc/FontInfoDesired.hpp"
#include "../../../buffer/out/textBuffer.hpp"

#include <numeric>

using namespace WEX::TestExecution;

namespace Microsoft::Console::Render::Atlas
{
    class AtlasEngineTests
    {
        TEST_CLASS(AtlasEngineTests);
        TEST_METHOD(KeycapUsesSingleFallbackFace);
        TEST_METHOD(KeycapRetainsSurroundingText);
        TEST_METHOD(KeycapWithFontAxesAndFeatures);
        TEST_METHOD(KeycapWithExplicitFallbackList);
        TEST_METHOD(ExistingClusterFragmentRetainsMapping);
        TEST_METHOD(OrdinaryClustersRetainCellAdvances);
        TEST_METHOD(KeycapMeasurementModes);
        TEST_METHOD(AdjacentKeycapsKeepSeparateFits);
        TEST_METHOD(GlyphCellFitPreservesGeometry);
        TEST_METHOD(KeycapRowStreaming);
        TEST_METHOD(NonKeycapFallbackRetainsMapping);

        static std::unique_ptr<AtlasEngine> makeEngine(bool axes = false, const wchar_t* family = L"Consolas")
        {
            auto engine = std::make_unique<AtlasEngine>();

            FontInfoDesired desired{ family, FF_MODERN, FW_NORMAL, 12.0f, CP_UTF8 };
            desired.SetEnableBuiltinGlyphs(false);

            FontInfo actual{ family, FF_MODERN, FW_NORMAL, { 0, 16 }, CP_UTF8 };

            VERIFY_SUCCEEDED(engine->UpdateDpi(96));
            VERIFY_SUCCEEDED(engine->UpdateViewport({ 0, 0, 39, 1 }));
            if (axes)
            {
                VERIFY_SUCCEEDED(engine->UpdateFont(desired, actual, { { L"liga", 0.0f } }, { { L"wght", 400.0f } }));
            }
            else
            {
                VERIFY_SUCCEEDED(engine->UpdateFont(desired, actual));
            }

            return engine;
        }

        static const ShapedRow& paint(AtlasEngine& engine, std::span<const Cluster> clusters)
        {
            VERIFY_SUCCEEDED(engine.InvalidateAll());
            VERIFY_SUCCEEDED(engine.StartPaint());
            VERIFY_SUCCEEDED(engine.PaintBufferLine(clusters, { 0, 0 }, false));
            VERIFY_SUCCEEDED(engine.EndPaint());
            // Shaping requires neither an HWND nor a backend or swap chain. Never call Present().
            VERIFY_IS_TRUE(!engine._api.s->target->hwnd);
            VERIFY_IS_TRUE(!engine._b);
            VERIFY_IS_TRUE(!engine._p.swapChain.swapChain);

            return *engine._p.rows[0];
        }

        static void verifyAdvance(const AtlasEngine& engine, const ShapedRow& row, unsigned columns)
        {
            const auto actual = std::accumulate(row.glyphAdvances.begin(), row.glyphAdvances.end(), 0.0f);
            const auto expected = static_cast<float>(columns * engine._p.s->font->cellSize.x);

            VERIFY_IS_TRUE(std::abs(actual - expected) < 0.001f);
        }
    };

    void AtlasEngineTests::KeycapUsesSingleFallbackFace()
    {
        auto engine = makeEngine();
        for (const auto base : std::wstring_view{ L"0123456789#*" })
        {
            for (const bool variationSelector : { false, true })
            {
                std::wstring text(1, base);
                if (variationSelector)
                {
                    text += L'\uFE0F';
                }
                text += L'\u20E3';

                WEX::Logging::Log::Comment(WEX::Common::String().Format(L"Keycap base U+%04X, VS16=%d", static_cast<unsigned>(base), variationSelector));
                CodepointWidthDetector detector;
                GraphemeState state{};
                detector.Reset(TextMeasurementMode::Graphemes);
                detector.GraphemeNext(state, text);
                VERIFY_ARE_EQUAL(text.size(), static_cast<size_t>(state.len));

                const std::array clusters{ Cluster{ text, state.width } };
                const auto& row = paint(*engine, clusters);

                VERIFY_IS_TRUE(!row.glyphIndices.empty());
                VERIFY_ARE_EQUAL(size_t{ 1 }, row.mappings.size());
                VERIFY_IS_TRUE(row.glyphAdvances.front() > 0.0f);
                VERIFY_ARE_EQUAL(size_t{ 1 }, row.glyphIndices.size());
                VERIFY_ARE_EQUAL(static_cast<u16>(state.width), row.mappings.front().fitToCells);
                verifyAdvance(*engine, row, state.width);
            }
        }
    }

    void AtlasEngineTests::KeycapRetainsSurroundingText()
    {
        auto engine = makeEngine();
        const std::array clusters{ Cluster{ L"A", 1 }, Cluster{ L"1\uFE0F\u20E3", 2 }, Cluster{ L"B", 1 } };
        const auto& row = paint(*engine, clusters);

        VERIFY_ARE_EQUAL(size_t{ 3 }, row.mappings.size());
        VERIFY_IS_TRUE(!row.glyphIndices.empty());
        const u32 prefix = L'A';
        const u32 suffix = L'B';
        u16 prefixGlyph = 0;
        u16 suffixGlyph = 0;

        VERIFY_SUCCEEDED(row.mappings.front().fontFace->GetGlyphIndicesW(&prefix, 1, &prefixGlyph));
        VERIFY_SUCCEEDED(row.mappings.back().fontFace->GetGlyphIndicesW(&suffix, 1, &suffixGlyph));
        VERIFY_ARE_EQUAL(prefixGlyph, row.glyphIndices.front());
        VERIFY_ARE_EQUAL(suffixGlyph, row.glyphIndices.back());
        verifyAdvance(*engine, row, 4);
    }

    void AtlasEngineTests::KeycapWithFontAxesAndFeatures()
    {
        auto engine = makeEngine(true);
        for (const auto text : { L"1\uFE0F\u20E3", L"*\uFE0F\u20E3", L"*\u20E3", L"\U0001F51F" })
        {
            CodepointWidthDetector detector;
            GraphemeState state{};
            detector.Reset(TextMeasurementMode::Graphemes);
            detector.GraphemeNext(state, text);

            const std::array clusters{ Cluster{ text, state.width } };
            const auto& row = paint(*engine, clusters);

            VERIFY_ARE_EQUAL(size_t{ 1 }, row.glyphIndices.size());
            VERIFY_ARE_EQUAL(size_t{ 1 }, row.mappings.size());
            VERIFY_ARE_EQUAL(static_cast<u16>(state.width), row.mappings.front().fitToCells);
            VERIFY_IS_TRUE(row.glyphAdvances.front() > 0.0f);
            verifyAdvance(*engine, row, state.width);
        }
    }
    void AtlasEngineTests::OrdinaryClustersRetainCellAdvances()
    {
        auto engine = makeEngine();
        const std::array clusters{ Cluster{ L"A", 1 }, Cluster{ L"B", 1 }, Cluster{ L"\u754C", 2 }, Cluster{ L"\U0001F469\u200D\U0001F4BB", 2 } };
        const auto& row = paint(*engine, clusters);

        VERIFY_IS_TRUE(!row.glyphIndices.empty());
        verifyAdvance(*engine, row, 6);
    }

    void AtlasEngineTests::KeycapWithExplicitFallbackList()
    {
        for (const auto family : { L"Consolas, Segoe UI Symbol", L"Consolas, Segoe UI Emoji" })
        {
            auto engine = makeEngine(false, family);
            for (const auto text : { L"1\uFE0F\u20E3", L"*\uFE0F\u20E3", L"*\u20E3" })
            {
                CodepointWidthDetector detector;
                GraphemeState state{};
                detector.Reset(TextMeasurementMode::Graphemes);
                detector.GraphemeNext(state, text);

                const std::array clusters{ Cluster{ text, state.width } };
                const auto& row = paint(*engine, clusters);

                VERIFY_ARE_EQUAL(size_t{ 1 }, row.glyphIndices.size());
                VERIFY_ARE_EQUAL(size_t{ 1 }, row.mappings.size());
                VERIFY_ARE_EQUAL(static_cast<u16>(state.width), row.mappings.front().fitToCells);
                VERIFY_IS_TRUE(row.glyphAdvances.front() > 0.0f);
                verifyAdvance(*engine, row, state.width);
            }
        }
    }
    void AtlasEngineTests::ExistingClusterFragmentRetainsMapping()
    {
        auto engine = makeEngine();
        VERIFY_SUCCEEDED(engine->StartPaint());

        // A drawing cluster may contain several fragments. Starting within it must not
        // retry face selection for the tail after an earlier attempt retained its mapping.
        constexpr std::wstring_view text{ L"x1\uFE0F\u20E3" };
        engine->_api.bufferLine.assign(text.begin(), text.end());
        engine->_api.bufferLineColumn = { 0, 0, 0, 0, 2 };
        u32 originalLength = 0;
        u32 actualLength = 0;
        wil::com_ptr<IDWriteFontFace2> originalFace;
        wil::com_ptr<IDWriteFontFace2> actualFace;

        engine->_mapCharacters(text.data() + 1, 3, &originalLength, originalFace.addressof());
        engine->_mapCharactersForGrapheme(1, 4, &actualLength, actualFace.addressof());
        VERIFY_ARE_EQUAL(originalLength, actualLength);
        VERIFY_IS_TRUE(originalFace && actualFace);
        VERIFY_IS_TRUE(originalFace.query<IDWriteFontFace5>()->Equals(actualFace.get()));

        // A mapping starting within one cluster can also split the following cluster.
        // Preserve its prefix boundary so that the next cluster still gets its own retry.
        constexpr std::wstring_view following{ L"xA1\uFE0F\u20E3" };
        engine->_api.bufferLine.assign(following.begin(), following.end());
        engine->_api.bufferLineColumn = { 0, 0, 1, 1, 1, 3 };
        actualFace.reset();
        engine->_mapCharactersForGrapheme(1, 5, &actualLength, actualFace.addressof());
        VERIFY_ARE_EQUAL(u32{ 1 }, actualLength);

        engine->_api.bufferLine.clear();
        engine->_api.bufferLineColumn.clear();
        VERIFY_SUCCEEDED(engine->EndPaint());
    }

    void AtlasEngineTests::KeycapMeasurementModes()
    {
        for (const auto mode : { TextMeasurementMode::Graphemes, TextMeasurementMode::Wcswidth, TextMeasurementMode::Console })
        {
            for (const auto base : std::wstring_view{ L"0123456789#*" })
            {
                for (const bool vs16 : { false, true })
                {
                    std::wstring text(1, base);
                    if (vs16)
                    {
                        text += L'\uFE0F';
                    }
                    text += L'\u20E3';

                    CodepointWidthDetector detector;
                    detector.Reset(mode);
                    GraphemeState state{};
                    int columns = 0;
                    size_t consumed = 0;
                    for (;;)
                    {
                        const auto more = detector.GraphemeNext(state, text);
                        VERIFY_IS_TRUE(state.len > 0 && static_cast<size_t>(state.len) <= text.size() - consumed);
                        consumed += state.len;
                        columns += state.width;
                        if (!more)
                        {
                            break;
                        }
                    }
                    VERIFY_ARE_EQUAL(text.size(), consumed);
                    VERIFY_ARE_EQUAL(mode == TextMeasurementMode::Graphemes ? 2 : 1, columns);
                }
            }

            CodepointWidthDetector detector;
            detector.Reset(mode);
            GraphemeState state{};
            detector.GraphemeNext(state, L"\U0001F51F");
            VERIFY_ARE_EQUAL(2, state.width);
            VERIFY_ARE_EQUAL(2, state.len);
        }
    }

    void AtlasEngineTests::AdjacentKeycapsKeepSeparateFits()
    {
        auto engine = makeEngine();
        const std::array clusters{ Cluster{ L"0\u20E3", 1 }, Cluster{ L"1\uFE0F\u20E3", 2 }, Cluster{ L"B", 1 }, Cluster{ L"\U0001F51F", 2 } };
        const auto& row = paint(*engine, clusters);

        VERIFY_ARE_EQUAL(size_t{ 4 }, row.mappings.size());
        VERIFY_ARE_EQUAL(u16{ 1 }, row.mappings[0].fitToCells);
        VERIFY_ARE_EQUAL(u16{ 2 }, row.mappings[1].fitToCells);
        VERIFY_ARE_EQUAL(u16{ 0 }, row.mappings[2].fitToCells);
        VERIFY_ARE_EQUAL(u16{ 2 }, row.mappings[3].fitToCells);
        VERIFY_ARE_EQUAL(size_t{ 4 }, row.glyphIndices.size());
        verifyAdvance(*engine, row, 6);
    }

    void AtlasEngineTests::NonKeycapFallbackRetainsMapping()
    {
        auto engine = makeEngine();
        VERIFY_SUCCEEDED(engine->StartPaint());
        for (const std::wstring_view text : { L"A\u20E3", L"/\u20E3", L"0\uFE0E\u20E3", L"a\u0363", L"e\u0364\u0364", L"i\u0365" })
        {
            engine->_api.bufferLine.assign(text.begin(), text.end());
            engine->_api.bufferLineColumn.assign(text.size(), 0);
            engine->_api.bufferLineColumn.push_back(1);

            u32 index = 0;
            while (index < text.size())
            {
                u32 expectedLength = 0;
                u32 actualLength = 0;
                wil::com_ptr<IDWriteFontFace2> expectedFace;
                wil::com_ptr<IDWriteFontFace2> actualFace;
                engine->_mapCharacters(text.data() + index, static_cast<u32>(text.size()) - index, &expectedLength, expectedFace.addressof());
                engine->_mapCharactersForGrapheme(index, static_cast<u32>(text.size()), &actualLength, actualFace.addressof());
                VERIFY_IS_TRUE(expectedLength > 0);
                VERIFY_ARE_EQUAL(expectedLength, actualLength);
                VERIFY_ARE_EQUAL(static_cast<bool>(expectedFace), static_cast<bool>(actualFace));
                if (expectedFace)
                {
                    VERIFY_IS_TRUE(expectedFace.query<IDWriteFontFace5>()->Equals(actualFace.get()));
                }
                index += actualLength;
            }
        }
        engine->_api.bufferLine.clear();
        engine->_api.bufferLineColumn.clear();
        VERIFY_SUCCEEDED(engine->EndPaint());
    }

    void AtlasEngineTests::KeycapRowStreaming()
    {
        auto& detector = CodepointWidthDetector::Singleton();
        const auto originalMode = detector.GetMode();
        const auto restoreMode = wil::scope_exit([&]() noexcept { detector.Reset(originalMode); });
        detector.Reset(TextMeasurementMode::Graphemes);

        for (const auto base : std::wstring_view{ L"0123456789#*" })
        {
            TextBuffer buffer{ { 12, 2 }, {}, 12, false, nullptr };
            const std::wstring prefix = L"A" + std::wstring(1, base);
            RowWriteState write{ .text = prefix };
            buffer.Replace(0, {}, write);
            VERIFY_ARE_EQUAL(2, write.columnEnd);

            write = { .text = L"\u20E3-text", .columnBegin = write.columnEnd };
            buffer.Replace(0, {}, write);
            VERIFY_IS_TRUE(write.text.empty());
            VERIFY_ARE_EQUAL(8, write.columnEnd);
            const auto& row = buffer.GetRowByOffset(0);
            const std::wstring keycap = std::wstring(1, base) + L'\u20E3';
            VERIFY_ARE_EQUAL(std::wstring_view{ keycap }, row.GetText(1, 3));
            VERIFY_ARE_EQUAL(DbcsAttribute::Leading, row.DbcsAttrAt(1));
            VERIFY_ARE_EQUAL(DbcsAttribute::Trailing, row.DbcsAttrAt(2));
            VERIFY_ARE_EQUAL(std::wstring_view{ L"-text" }, row.GetText(3, 8));

            // Overwriting the trailer clears the preceding half of the old cluster.
            write = { .text = L"X", .columnBegin = 2 };
            buffer.Replace(0, {}, write);
            VERIFY_ARE_EQUAL(std::wstring_view{ L" X" }, row.GetText(1, 3));
            VERIFY_ARE_EQUAL(DbcsAttribute::Single, row.DbcsAttrAt(1));
            VERIFY_ARE_EQUAL(DbcsAttribute::Single, row.DbcsAttrAt(2));
        }
    }

    void AtlasEngineTests::GlyphCellFitPreservesGeometry()
    {
        for (const auto bounds : { D2D1_RECT_F{ -3, -15, 20, 3 }, D2D1_RECT_F{ 2, -12, 10, 0 }, D2D1_RECT_F{ 0, -9, 9, 1 } })
        {
            for (const float width : { 6.0f, 9.0f, 18.0f, 24.0f })
            {
                const auto fit = fitGlyphToCell(bounds, width);
                const auto left = bounds.left * fit.scaleX + fit.offsetX;
                const auto right = bounds.right * fit.scaleX + fit.offsetX;
                const auto centerY = (bounds.top + bounds.bottom) * 0.5f;

                VERIFY_IS_TRUE(fit.scaleX > 0 && fit.scaleX <= 1);
                VERIFY_IS_TRUE(left >= -0.001f && right <= width + 0.001f);
                VERIFY_IS_TRUE(std::abs((left + right) * 0.5f - width * 0.5f) < 0.001f);
                VERIFY_IS_TRUE(std::abs(centerY * fit.scaleY + fit.offsetY - centerY) < 0.001f);
                VERIFY_ARE_EQUAL(fit.scaleX, fit.scaleY);
            }
        }
    }
}
