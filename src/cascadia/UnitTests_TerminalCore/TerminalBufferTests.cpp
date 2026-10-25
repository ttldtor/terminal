// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include <WexTestClass.h>

#include "../renderer/inc/DummyRenderer.hpp"
#include "../cascadia/TerminalCore/Terminal.hpp"
#include "MockTermSettings.h"
#include "consoletaeftemplates.hpp"
#include "../../inc/TestUtils.h"
#include "../../types/inc/CodepointWidthDetector.hpp"

using namespace winrt::Microsoft::Terminal::Core;
using namespace Microsoft::Terminal::Core;

using namespace WEX::Common;
using namespace WEX::Logging;
using namespace WEX::TestExecution;

namespace TerminalCoreUnitTests
{
    class TerminalBufferTests;
};
using namespace TerminalCoreUnitTests;

class TerminalCoreUnitTests::TerminalBufferTests final
{
    // !!! DANGER: Many tests in this class expect the Terminal buffer
    // to be 80x32. If you change these, you'll probably inadvertently break a
    // bunch of tests !!!
    static const til::CoordType TerminalViewWidth = 80;
    static const til::CoordType TerminalViewHeight = 32;
    static const til::CoordType TerminalHistoryLength = 100;

    TEST_CLASS(TerminalBufferTests);

    TEST_METHOD(TestSimpleBufferWriting);

    TEST_METHOD(TestWrappingCharByChar);
    TEST_METHOD(TestWrappingALongString);
    TEST_METHOD(TestKeycapWrappingAcrossWrites);
    TEST_METHOD(TestKeycapNoWrapBoundary);
    TEST_METHOD(TestKeycapWrappingModes);

    TEST_METHOD(DontSnapToOutputTest);

    TEST_METHOD(TestResetClearTabStops);

    TEST_METHOD(TestAddTabStop);

    TEST_METHOD(TestClearTabStop);

    TEST_METHOD(TestGetForwardTab);

    TEST_METHOD(TestGetReverseTab);

    TEST_METHOD(TestURLPatternDetection);

    TEST_METHOD_SETUP(MethodSetup)
    {
        // STEP 1: Set up the Terminal
        term = std::make_unique<Terminal>(Terminal::TestDummyMarker{});
        emptyRenderer = std::make_unique<DummyRenderer>(term.get());
        term->Create({ TerminalViewWidth, TerminalViewHeight }, TerminalHistoryLength, *emptyRenderer);
        return true;
    }

    TEST_METHOD_CLEANUP(MethodCleanup)
    {
        emptyRenderer = nullptr;
        term = nullptr;
        return true;
    }

private:
    void _SetTabStops(std::list<til::CoordType> columns, bool replace);
    std::list<til::CoordType> _GetTabStops();

    std::unique_ptr<DummyRenderer> emptyRenderer;
    std::unique_ptr<Terminal> term;
};

void TerminalBufferTests::TestSimpleBufferWriting()
{
    auto& termTb = *term->_mainBuffer;
    auto& termSm = *term->_stateMachine;
    const auto initialView = term->GetViewport();

    VERIFY_ARE_EQUAL(0, initialView.Top());
    VERIFY_ARE_EQUAL(32, initialView.BottomExclusive());

    termSm.ProcessString(L"Hello World");

    const auto secondView = term->GetViewport();

    VERIFY_ARE_EQUAL(0, secondView.Top());
    VERIFY_ARE_EQUAL(32, secondView.BottomExclusive());

    TestUtils::VerifyExpectedString(termTb, L"Hello World", { 0, 0 });
}

void TerminalBufferTests::TestWrappingCharByChar()
{
    auto& termTb = *term->_mainBuffer;
    auto& termSm = *term->_stateMachine;
    const auto initialView = term->GetViewport();
    auto& cursor = termTb.GetCursor();

    const auto charsToWrite = gsl::narrow_cast<til::CoordType>(TestUtils::Test100CharsString.size());

    VERIFY_ARE_EQUAL(0, initialView.Top());
    VERIFY_ARE_EQUAL(32, initialView.BottomExclusive());

    for (auto i = 0; i < charsToWrite; i++)
    {
        // This is a handy way of just printing the printable characters that
        // _aren't_ the space character.
        const auto wch = static_cast<wchar_t>(33 + (i % 94));
        termSm.ProcessCharacter(wch);
    }

    const auto secondView = term->GetViewport();

    VERIFY_ARE_EQUAL(0, secondView.Top());
    VERIFY_ARE_EQUAL(32, secondView.BottomExclusive());

    // Verify the cursor wrapped to the second line
    VERIFY_ARE_EQUAL(charsToWrite % initialView.Width(), cursor.GetPosition().x);
    VERIFY_ARE_EQUAL(1, cursor.GetPosition().y);

    // Verify that we marked the 0th row as _wrapped_
    const auto& row0 = termTb.GetRowByOffset(0);
    VERIFY_IS_TRUE(row0.WasWrapForced());

    const auto& row1 = termTb.GetRowByOffset(1);
    VERIFY_IS_FALSE(row1.WasWrapForced());

    TestUtils::VerifyExpectedString(termTb, TestUtils::Test100CharsString, { 0, 0 });
}

void TerminalBufferTests::TestKeycapWrappingAcrossWrites()
{
    auto& detector = CodepointWidthDetector::Singleton();
    const auto originalMode = detector.GetMode();
    const auto restoreMode = wil::scope_exit([&]() noexcept { detector.Reset(originalMode); });
    detector.Reset(TextMeasurementMode::Graphemes);

    for (const auto base : std::wstring_view{ L"0123456789#*" })
    {
        for (const bool vs16 : { false, true })
        {
            for (const bool streamed : { false, true })
            {
                for (const int startRow : { 0, TerminalViewHeight - 1 })
                {
                    Terminal candidate{ Terminal::TestDummyMarker{} };
                    DummyRenderer renderer{ &candidate };
                    candidate.Create({ TerminalViewWidth, TerminalViewHeight }, TerminalHistoryLength, renderer);
                    auto& buffer = *candidate._mainBuffer;
                    auto& machine = *candidate._stateMachine;
                    auto& cursor = buffer.GetCursor();
                    cursor.SetPosition({ TerminalViewWidth - 1, startRow });

                    std::wstring keycap(1, base);
                    if (vs16)
                    {
                        keycap += L'\uFE0F';
                    }
                    keycap += L'\u20E3';

                    if (streamed)
                    {
                        for (const auto ch : keycap)
                        {
                            machine.ProcessString({ &ch, 1 });
                        }
                    }
                    else
                    {
                        machine.ProcessString(keycap);
                    }
                    machine.ProcessString(L"-text");

                    const auto nextRow = startRow + 1;
                    const auto expected = keycap + L"-text";
                    VERIFY_ARE_EQUAL(std::wstring_view{ L" " }, buffer.GetRowByOffset(startRow).GetText(79, 80));
                    VERIFY_ARE_EQUAL(std::wstring_view{ expected }, buffer.GetRowByOffset(nextRow).GetText(0, 7));
                    VERIFY_ARE_EQUAL(til::point(7, nextRow), cursor.GetPosition());
                    VERIFY_ARE_EQUAL(DbcsAttribute::Leading, buffer.GetRowByOffset(nextRow).DbcsAttrAt(0));
                    VERIFY_ARE_EQUAL(DbcsAttribute::Trailing, buffer.GetRowByOffset(nextRow).DbcsAttrAt(1));
                }
            }
        }
    }
}

void TerminalBufferTests::TestKeycapNoWrapBoundary()
{
    auto& detector = CodepointWidthDetector::Singleton();
    const auto originalMode = detector.GetMode();
    const auto restoreMode = wil::scope_exit([&]() noexcept { detector.Reset(originalMode); });
    detector.Reset(TextMeasurementMode::Graphemes);

    for (const auto base : std::wstring_view{ L"0123456789#*" })
    {
        for (const bool vs16 : { false, true })
        {
            Terminal candidate{ Terminal::TestDummyMarker{} };
            DummyRenderer renderer{ &candidate };
            candidate.Create({ TerminalViewWidth, TerminalViewHeight }, TerminalHistoryLength, renderer);
            auto& buffer = *candidate._mainBuffer;
            auto& machine = *candidate._stateMachine;
            machine.ProcessString(L"\x1b[?7l\x1b[1;79H");
            const std::wstring expected = L"1" + std::wstring(1, base);
            machine.ProcessString(expected);
            if (vs16)
            {
                machine.ProcessString(L"\uFE0F");
            }
            machine.ProcessString(L"\u20E3");
            // No two-column promotion fits here with wrapping off. Preserve
            // the base and its neighbour instead of making the wrong keycap.
            VERIFY_ARE_EQUAL(std::wstring_view{ expected }, buffer.GetRowByOffset(0).GetText(78, 80));
            VERIFY_ARE_EQUAL(til::point(79, 0), buffer.GetCursor().GetPosition());
            VERIFY_IS_TRUE(buffer.GetCursor().WasWriteClamped());

            machine.ProcessString(L"\x1b[1;80H");
            VERIFY_IS_FALSE(buffer.GetCursor().WasWriteClamped());
            machine.ProcessString(L"X");
            VERIFY_ARE_EQUAL(std::wstring_view{ L"1X" }, buffer.GetRowByOffset(0).GetText(78, 80));
        }
    }
}

void TerminalBufferTests::TestKeycapWrappingModes()
{
    auto& detector = CodepointWidthDetector::Singleton();
    const auto originalMode = detector.GetMode();
    const auto restoreMode = wil::scope_exit([&]() noexcept { detector.Reset(originalMode); });
    detector.Reset(TextMeasurementMode::Graphemes);

    struct Test
    {
        std::wstring_view setup;
        int lastColumn;
        int nextColumn;
        bool existingDestination = false;
    };
    const std::array tests{
        Test{ L"\x1b[4h", 79, 0, true }, // Insert mode, preserving destination text.
        Test{ L"\x1b#6", 39, 0 }, // Double-width source line.
        Test{ L"\x1b[?69h\x1b[10;20s", 19, 9 }, // Horizontal margins.
    };
    for (const auto& test : tests)
    {
        Terminal candidate{ Terminal::TestDummyMarker{} };
        DummyRenderer renderer{ &candidate };
        candidate.Create({ TerminalViewWidth, TerminalViewHeight }, 0, renderer);
        auto& buffer = *candidate._mainBuffer;
        auto& machine = *candidate._stateMachine;
        machine.ProcessString(test.setup);
        if (test.existingDestination)
        {
            buffer.GetCursor().SetPosition({ 0, 1 });
            machine.ProcessString(L"ZZZZZ");
        }
        buffer.GetCursor().SetPosition({ test.lastColumn, 0 });
        machine.ProcessString(L"0");
        machine.ProcessString(L"\u20E3X");
        VERIFY_ARE_EQUAL(std::wstring_view{ L" " }, buffer.GetRowByOffset(0).GetText(test.lastColumn, test.lastColumn + 1));
        VERIFY_ARE_EQUAL(std::wstring_view{ L"0\u20E3X" }, buffer.GetRowByOffset(1).GetText(test.nextColumn, test.nextColumn + 3));
        VERIFY_ARE_EQUAL(til::point(test.nextColumn + 3, 1), buffer.GetCursor().GetPosition());
        if (test.existingDestination)
        {
            VERIFY_ARE_EQUAL(std::wstring_view{ L"ZZZZZ" }, buffer.GetRowByOffset(1).GetText(3, 8));
        }
    }

    // The core clamps a viewport to at least two physical cells, but DEC
    // double-width lines can still leave only one logical cell available.
    Terminal narrow{ Terminal::TestDummyMarker{} };
    DummyRenderer renderer{ &narrow };
    narrow.Create({ 2, 3 }, 0, renderer);
    narrow._stateMachine->ProcessString(L"\x1b#6\x1b[2;1H\x1b#6\x1b[1;1H");
    VERIFY_ARE_EQUAL(1, narrow._mainBuffer->GetLineWidth(1));
    narrow._stateMachine->ProcessString(L"0");
    narrow._stateMachine->ProcessString(L"\u20E3X");
    // Dropping an unfittable glyph also marks delayed wrap; the next printable
    // character therefore advances to the following normal-width line.
    VERIFY_ARE_EQUAL(std::wstring_view{ L"X" }, narrow._mainBuffer->GetRowByOffset(2).GetText(0, 1));
}

void TerminalBufferTests::TestWrappingALongString()
{
    auto& termTb = *term->_mainBuffer;
    auto& termSm = *term->_stateMachine;
    const auto initialView = term->GetViewport();
    auto& cursor = termTb.GetCursor();

    const auto charsToWrite = gsl::narrow_cast<til::CoordType>(TestUtils::Test100CharsString.size());
    VERIFY_ARE_EQUAL(100, charsToWrite);

    VERIFY_ARE_EQUAL(0, initialView.Top());
    VERIFY_ARE_EQUAL(32, initialView.BottomExclusive());

    termSm.ProcessString(TestUtils::Test100CharsString);

    const auto secondView = term->GetViewport();

    VERIFY_ARE_EQUAL(0, secondView.Top());
    VERIFY_ARE_EQUAL(32, secondView.BottomExclusive());

    // Verify the cursor wrapped to the second line
    VERIFY_ARE_EQUAL(charsToWrite % initialView.Width(), cursor.GetPosition().x);
    VERIFY_ARE_EQUAL(1, cursor.GetPosition().y);

    // Verify that we marked the 0th row as _wrapped_
    const auto& row0 = termTb.GetRowByOffset(0);
    VERIFY_IS_TRUE(row0.WasWrapForced());

    const auto& row1 = termTb.GetRowByOffset(1);
    VERIFY_IS_FALSE(row1.WasWrapForced());

    TestUtils::VerifyExpectedString(termTb, TestUtils::Test100CharsString, { 0, 0 });
}

void TerminalBufferTests::DontSnapToOutputTest()
{
    auto& termTb = *term->_mainBuffer;
    auto& termSm = *term->_stateMachine;
    const auto initialView = term->GetViewport();

    VERIFY_ARE_EQUAL(0, initialView.Top());
    VERIFY_ARE_EQUAL(TerminalViewHeight, initialView.BottomExclusive());
    VERIFY_ARE_EQUAL(0, term->_scrollOffset);

    // -1 so that we don't print the last \n
    for (auto i = 0; i < TerminalViewHeight + 8 - 1; i++)
    {
        termSm.ProcessString(L"x\n");
    }

    const auto secondView = term->GetViewport();

    VERIFY_ARE_EQUAL(8, secondView.Top());
    VERIFY_ARE_EQUAL(TerminalViewHeight + 8, secondView.BottomExclusive());
    VERIFY_ARE_EQUAL(0, term->_scrollOffset);

    Log::Comment(L"Scroll up one line");
    term->_scrollOffset = 1;

    const auto thirdView = term->GetViewport();
    VERIFY_ARE_EQUAL(7, thirdView.Top());
    VERIFY_ARE_EQUAL(TerminalViewHeight + 7, thirdView.BottomExclusive());
    VERIFY_ARE_EQUAL(1, term->_scrollOffset);

    Log::Comment(L"Print a few lines, to see that the viewport stays where it was");
    for (auto i = 0; i < 8; i++)
    {
        termSm.ProcessString(L"x\n");
    }

    const auto fourthView = term->GetViewport();
    VERIFY_ARE_EQUAL(7, fourthView.Top());
    VERIFY_ARE_EQUAL(TerminalViewHeight + 7, fourthView.BottomExclusive());
    VERIFY_ARE_EQUAL(1 + 8, term->_scrollOffset);

    Log::Comment(L"Print enough lines to get the buffer just about ready to "
                 L"circle (on the next newline)");
    auto viewBottom = term->_mutableViewport.BottomInclusive();
    do
    {
        termSm.ProcessString(L"x\n");
        viewBottom = term->_mutableViewport.BottomInclusive();
    } while (viewBottom < termTb.GetSize().BottomInclusive());

    const auto fifthView = term->GetViewport();
    VERIFY_ARE_EQUAL(7, fifthView.Top());
    VERIFY_ARE_EQUAL(TerminalViewHeight + 7, fifthView.BottomExclusive());
    VERIFY_ARE_EQUAL(TerminalHistoryLength - 7, term->_scrollOffset);

    Log::Comment(L"Print 3 more lines, and see that we stick to where the old "
                 L"rows now are in the buffer (after circling)");
    for (auto i = 0; i < 3; i++)
    {
        termSm.ProcessString(L"x\n");
        Log::Comment(NoThrowString().Format(
            L"_scrollOffset: %d", term->_scrollOffset));
    }
    const auto sixthView = term->GetViewport();
    VERIFY_ARE_EQUAL(4, sixthView.Top());
    VERIFY_ARE_EQUAL(TerminalViewHeight + 4, sixthView.BottomExclusive());
    VERIFY_ARE_EQUAL(TerminalHistoryLength - 4, term->_scrollOffset);

    Log::Comment(L"Print 8 more lines, and see that we're now just stuck at the"
                 L"top of the buffer");
    for (auto i = 0; i < 8; i++)
    {
        termSm.ProcessString(L"x\n");
        Log::Comment(NoThrowString().Format(
            L"_scrollOffset: %d", term->_scrollOffset));
    }
    const auto seventhView = term->GetViewport();
    VERIFY_ARE_EQUAL(0, seventhView.Top());
    VERIFY_ARE_EQUAL(TerminalViewHeight, seventhView.BottomExclusive());
    VERIFY_ARE_EQUAL(TerminalHistoryLength, term->_scrollOffset);
}

void TerminalBufferTests::_SetTabStops(std::list<til::CoordType> columns, bool replace)
{
    auto& termTb = *term->_mainBuffer;
    auto& termSm = *term->_stateMachine;
    auto& cursor = termTb.GetCursor();

    const auto clearTabStops = L"\033[3g";
    const auto addTabStop = L"\033H";

    if (replace)
    {
        termSm.ProcessString(clearTabStops);
    }

    for (auto column : columns)
    {
        cursor.SetXPosition(column);
        termSm.ProcessString(addTabStop);
    }
}

std::list<til::CoordType> TerminalBufferTests::_GetTabStops()
{
    std::list<til::CoordType> columns;
    auto& termTb = *term->_mainBuffer;
    auto& termSm = *term->_stateMachine;
    const auto initialView = term->GetViewport();
    const auto lastColumn = initialView.RightInclusive();
    auto& cursor = termTb.GetCursor();

    cursor.SetPosition({ 0, 0 });
    for (;;)
    {
        termSm.ProcessCharacter(L'\t');
        auto column = cursor.GetPosition().x;
        if (column >= lastColumn)
        {
            break;
        }
        columns.push_back(column);
    }

    return columns;
}

void TerminalBufferTests::TestResetClearTabStops()
{
    auto& termSm = *term->_stateMachine;
    const auto initialView = term->GetViewport();

    const auto clearTabStops = L"\033[3g";
    const auto resetToInitialState = L"\033c";

    Log::Comment(L"Default tabs every 8 columns.");
    std::list<til::CoordType> expectedStops{ 8, 16, 24, 32, 40, 48, 56, 64, 72 };
    VERIFY_ARE_EQUAL(expectedStops, _GetTabStops());

    Log::Comment(L"Clear all tabs.");
    termSm.ProcessString(clearTabStops);
    expectedStops = {};
    VERIFY_ARE_EQUAL(expectedStops, _GetTabStops());

    Log::Comment(L"RIS resets tabs to defaults.");
    termSm.ProcessString(resetToInitialState);
    expectedStops = { 8, 16, 24, 32, 40, 48, 56, 64, 72 };
    VERIFY_ARE_EQUAL(expectedStops, _GetTabStops());
}

void TerminalBufferTests::TestAddTabStop()
{
    auto& termTb = *term->_mainBuffer;
    auto& termSm = *term->_stateMachine;
    auto& cursor = termTb.GetCursor();

    const auto clearTabStops = L"\033[3g";
    const auto addTabStop = L"\033H";

    Log::Comment(L"Clear all tabs.");
    termSm.ProcessString(clearTabStops);
    std::list<til::CoordType> expectedStops{};
    VERIFY_ARE_EQUAL(expectedStops, _GetTabStops());

    Log::Comment(L"Add tab to empty list.");
    cursor.SetXPosition(12);
    termSm.ProcessString(addTabStop);
    expectedStops.push_back(12);
    VERIFY_ARE_EQUAL(expectedStops, _GetTabStops());

    Log::Comment(L"Add tab to head of existing list.");
    cursor.SetXPosition(4);
    termSm.ProcessString(addTabStop);
    expectedStops.push_front(4);
    VERIFY_ARE_EQUAL(expectedStops, _GetTabStops());

    Log::Comment(L"Add tab to tail of existing list.");
    cursor.SetXPosition(30);
    termSm.ProcessString(addTabStop);
    expectedStops.push_back(30);
    VERIFY_ARE_EQUAL(expectedStops, _GetTabStops());

    Log::Comment(L"Add tab to middle of existing list.");
    cursor.SetXPosition(24);
    termSm.ProcessString(addTabStop);
    expectedStops.push_back(24);
    expectedStops.sort();
    VERIFY_ARE_EQUAL(expectedStops, _GetTabStops());

    Log::Comment(L"Add tab that duplicates an item in the existing list.");
    cursor.SetXPosition(24);
    termSm.ProcessString(addTabStop);
    VERIFY_ARE_EQUAL(expectedStops, _GetTabStops());
}

void TerminalBufferTests::TestClearTabStop()
{
    auto& termTb = *term->_mainBuffer;
    auto& termSm = *term->_stateMachine;
    auto& cursor = termTb.GetCursor();

    const auto clearTabStops = L"\033[3g";
    const auto clearTabStop = L"\033[0g";
    const auto addTabStop = L"\033H";

    Log::Comment(L"Start with all tabs cleared.");
    {
        termSm.ProcessString(clearTabStops);

        VERIFY_IS_TRUE(_GetTabStops().empty());
    }

    Log::Comment(L"Try to clear nonexistent list.");
    {
        cursor.SetXPosition(0);
        termSm.ProcessString(clearTabStop);

        VERIFY_IS_TRUE(_GetTabStops().empty(), L"List should remain empty");
    }

    Log::Comment(L"Allocate 1 list item and clear it.");
    {
        cursor.SetXPosition(0);
        termSm.ProcessString(addTabStop);
        termSm.ProcessString(clearTabStop);

        VERIFY_IS_TRUE(_GetTabStops().empty());
    }

    Log::Comment(L"Allocate 1 list item and clear nonexistent.");
    {
        cursor.SetXPosition(1);
        termSm.ProcessString(addTabStop);

        Log::Comment(L"Free greater");
        cursor.SetXPosition(2);
        termSm.ProcessString(clearTabStop);
        VERIFY_IS_FALSE(_GetTabStops().empty());

        Log::Comment(L"Free less than");
        cursor.SetXPosition(0);
        termSm.ProcessString(clearTabStop);
        VERIFY_IS_FALSE(_GetTabStops().empty());

        // clear all tab stops
        termSm.ProcessString(clearTabStops);
    }

    Log::Comment(L"Allocate many (5) list items and clear head.");
    {
        std::list<til::CoordType> inputData = { 3, 5, 6, 10, 15, 17 };
        _SetTabStops(inputData, false);
        cursor.SetXPosition(inputData.front());
        termSm.ProcessString(clearTabStop);

        inputData.pop_front();
        VERIFY_ARE_EQUAL(inputData, _GetTabStops());

        // clear all tab stops
        termSm.ProcessString(clearTabStops);
    }

    Log::Comment(L"Allocate many (5) list items and clear middle.");
    {
        std::list<til::CoordType> inputData = { 3, 5, 6, 10, 15, 17 };
        _SetTabStops(inputData, false);
        cursor.SetXPosition(*std::next(inputData.begin()));
        termSm.ProcessString(clearTabStop);

        inputData.erase(std::next(inputData.begin()));
        VERIFY_ARE_EQUAL(inputData, _GetTabStops());

        // clear all tab stops
        termSm.ProcessString(clearTabStops);
    }

    Log::Comment(L"Allocate many (5) list items and clear tail.");
    {
        std::list<til::CoordType> inputData = { 3, 5, 6, 10, 15, 17 };
        _SetTabStops(inputData, false);
        cursor.SetXPosition(inputData.back());
        termSm.ProcessString(clearTabStop);

        inputData.pop_back();
        VERIFY_ARE_EQUAL(inputData, _GetTabStops());

        // clear all tab stops
        termSm.ProcessString(clearTabStops);
    }

    Log::Comment(L"Allocate many (5) list items and clear nonexistent item.");
    {
        std::list<til::CoordType> inputData = { 3, 5, 6, 10, 15, 17 };
        _SetTabStops(inputData, false);
        cursor.SetXPosition(0);
        termSm.ProcessString(clearTabStop);

        VERIFY_ARE_EQUAL(inputData, _GetTabStops());

        // clear all tab stops
        termSm.ProcessString(clearTabStops);
    }
}

void TerminalBufferTests::TestGetForwardTab()
{
    auto& termTb = *term->_mainBuffer;
    auto& termSm = *term->_stateMachine;
    const auto initialView = term->GetViewport();
    auto& cursor = termTb.GetCursor();

    const auto nextForwardTab = L"\033[I";

    std::list<til::CoordType> inputData = { 3, 5, 6, 10, 15, 17 };
    _SetTabStops(inputData, true);

    const auto coordScreenBufferSize = initialView.Dimensions();

    Log::Comment(L"Find next tab from before front.");
    {
        cursor.SetXPosition(0);

        auto coordCursorExpected = cursor.GetPosition();
        coordCursorExpected.x = inputData.front();

        termSm.ProcessString(nextForwardTab);
        const auto coordCursorResult = cursor.GetPosition();
        VERIFY_ARE_EQUAL(coordCursorExpected,
                         coordCursorResult,
                         L"Cursor advanced to first tab stop from sample list.");
    }

    Log::Comment(L"Find next tab from in the middle.");
    {
        cursor.SetXPosition(6);

        auto coordCursorExpected = cursor.GetPosition();
        coordCursorExpected.x = *std::next(inputData.begin(), 3);

        termSm.ProcessString(nextForwardTab);
        const auto coordCursorResult = cursor.GetPosition();
        VERIFY_ARE_EQUAL(coordCursorExpected,
                         coordCursorResult,
                         L"Cursor advanced to middle tab stop from sample list.");
    }

    Log::Comment(L"Find next tab from end.");
    {
        cursor.SetXPosition(30);

        auto coordCursorExpected = cursor.GetPosition();
        coordCursorExpected.x = coordScreenBufferSize.width - 1;

        termSm.ProcessString(nextForwardTab);
        const auto coordCursorResult = cursor.GetPosition();
        VERIFY_ARE_EQUAL(coordCursorExpected,
                         coordCursorResult,
                         L"Cursor advanced to end of screen buffer.");
    }

    Log::Comment(L"Find next tab from rightmost column.");
    {
        cursor.SetXPosition(coordScreenBufferSize.width - 1);

        auto coordCursorExpected = cursor.GetPosition();

        termSm.ProcessString(nextForwardTab);
        const auto coordCursorResult = cursor.GetPosition();
        VERIFY_ARE_EQUAL(coordCursorExpected,
                         coordCursorResult,
                         L"Cursor remains in rightmost column.");
    }
}

void TerminalBufferTests::TestGetReverseTab()
{
    auto& termTb = *term->_mainBuffer;
    auto& termSm = *term->_stateMachine;
    auto& cursor = termTb.GetCursor();

    const auto nextReverseTab = L"\033[Z";

    std::list<til::CoordType> inputData = { 3, 5, 6, 10, 15, 17 };
    _SetTabStops(inputData, true);

    Log::Comment(L"Find previous tab from before front.");
    {
        cursor.SetXPosition(1);

        auto coordCursorExpected = cursor.GetPosition();
        coordCursorExpected.x = 0;

        termSm.ProcessString(nextReverseTab);
        const auto coordCursorResult = cursor.GetPosition();
        VERIFY_ARE_EQUAL(coordCursorExpected,
                         coordCursorResult,
                         L"Cursor adjusted to beginning of the buffer when it started before sample list.");
    }

    Log::Comment(L"Find previous tab from in the middle.");
    {
        cursor.SetXPosition(6);

        auto coordCursorExpected = cursor.GetPosition();
        coordCursorExpected.x = *std::next(inputData.begin());

        termSm.ProcessString(nextReverseTab);
        const auto coordCursorResult = cursor.GetPosition();
        VERIFY_ARE_EQUAL(coordCursorExpected,
                         coordCursorResult,
                         L"Cursor adjusted back one tab spot from middle of sample list.");
    }

    Log::Comment(L"Find next tab from end.");
    {
        cursor.SetXPosition(30);

        auto coordCursorExpected = cursor.GetPosition();
        coordCursorExpected.x = inputData.back();

        termSm.ProcessString(nextReverseTab);
        const auto coordCursorResult = cursor.GetPosition();
        VERIFY_ARE_EQUAL(coordCursorExpected,
                         coordCursorResult,
                         L"Cursor adjusted to last item in the sample list from position beyond end.");
    }
}

void TerminalBufferTests::TestURLPatternDetection()
{
    using namespace std::string_view_literals;

    constexpr auto BeforeStr = L"<Before>"sv;
    constexpr auto UrlStr = L"https://www.contoso.com"sv;
    constexpr auto AfterStr = L"<After>"sv;
    constexpr auto urlStartX = BeforeStr.size();
    constexpr auto urlEndX = BeforeStr.size() + UrlStr.size() - 1;

    // This is off by default; turn it on for the test.
    auto originalDetectURLs = term->_detectURLs;
    auto restoreDetectUrls = wil::scope_exit([&]() {
        term->_detectURLs = originalDetectURLs;
    });
    term->_detectURLs = true;

    auto& termSm = *term->_stateMachine;
    termSm.ProcessString(fmt::format(FMT_COMPILE(L"{}{}{}"), BeforeStr, UrlStr, AfterStr));
    term->UpdatePatternsUnderLock();

    std::wstring result;

    result = term->GetHyperlinkAtBufferPosition(til::point{ urlStartX - 1, 0 });
    VERIFY_IS_TRUE(result.empty(), L"URL is not detected before the actual URL.");

    result = term->GetHyperlinkAtBufferPosition(til::point{ urlStartX, 0 });
    VERIFY_IS_TRUE(!result.empty(), L"A URL is detected at the start position.");
    VERIFY_ARE_EQUAL(result, UrlStr, L"Detected URL matches the given URL.");

    result = term->GetHyperlinkAtBufferPosition(til::point{ urlEndX, 0 });
    VERIFY_IS_TRUE(!result.empty(), L"A URL is detected at the end position.");
    VERIFY_ARE_EQUAL(result, UrlStr, L"Detected URL matches the given URL.");

    result = term->GetHyperlinkAtBufferPosition(til::point{ urlEndX + 1, 0 });
    VERIFY_IS_TRUE(result.empty(), L"URL is not detected after the actual URL.");

    Log::Comment(L"Test wrapped URL detection");
    {
        // Build a URL longer than the terminal width so it wraps
        // Terminal is 80 cols wide; pad before + URL must exceed 80
        constexpr auto WrapBefore = L"WRAP>"sv;
        const std::wstring longUrl = L"https://www.contoso.com/this-is-a-very-long-path/that-will-wrap-across-multiple-rows-in-the-terminal-buffer";
        const auto wrapUrlStartX = static_cast<til::CoordType>(WrapBefore.size());
        const auto totalLen = WrapBefore.size() + longUrl.size();
        // The URL should wrap to row 1
        VERIFY_IS_TRUE(totalLen > static_cast<size_t>(TerminalViewWidth), L"URL must exceed terminal width to wrap");

        // Move cursor to row 2 and write the wrapped URL
        termSm.ProcessString(L"\r\n\r\n");
        termSm.ProcessString(fmt::format(FMT_COMPILE(L"{}{}"), WrapBefore, longUrl));
        term->UpdatePatternsUnderLock();

        const auto cursorRow = term->_mainBuffer->GetCursor().GetPosition().y;
        const auto wrapRow0 = cursorRow - 1; // first row of wrapped text
        const auto wrapRow1 = cursorRow; // second row

        // Detect from start of URL (first row)
        result = term->GetHyperlinkAtBufferPosition(til::point{ wrapUrlStartX, wrapRow0 });
        VERIFY_IS_TRUE(!result.empty(), L"Wrapped URL is detected on the first row.");
        VERIFY_ARE_EQUAL(result, longUrl, L"Full wrapped URL is returned from first row.");

        // Detect from second row of URL
        result = term->GetHyperlinkAtBufferPosition(til::point{ 0, wrapRow1 });
        VERIFY_IS_TRUE(!result.empty(), L"Wrapped URL is detected on the second row.");
        VERIFY_ARE_EQUAL(result, longUrl, L"Full wrapped URL is returned from second row.");

        // Before the URL on the first row
        result = term->GetHyperlinkAtBufferPosition(til::point{ wrapUrlStartX - 1, wrapRow0 });
        VERIFY_IS_TRUE(result.empty(), L"URL is not detected before the wrapped URL.");
    }

    Log::Comment(L"Test URL detection after scrolling into history");
    {
        // Generate enough output to push the URLs into scrollback
        for (auto i = 0; i < TerminalViewHeight + 8; i++)
        {
            termSm.ProcessString(L"filler\r\n");
        }

        // Write a new URL at the current cursor position
        constexpr auto ScrollUrl = L"https://www.example.com/scrolled"sv;
        termSm.ProcessString(ScrollUrl);
        term->UpdatePatternsUnderLock();

        const auto scrollUrlRow = term->_mainBuffer->GetCursor().GetPosition().y;
        result = term->GetHyperlinkAtBufferPosition(til::point{ 0, scrollUrlRow });
        VERIFY_IS_TRUE(!result.empty(), L"URL is detected after scrolling.");
        VERIFY_ARE_EQUAL(result, ScrollUrl, L"Correct URL is returned after scrolling.");
    }

    Log::Comment(L"Test viewport-relative interval coordinates");
    {
        constexpr auto VpUrl = L"https://www.example.com/viewport"sv;
        termSm.ProcessString(L"\r\n");
        termSm.ProcessString(VpUrl);
        term->UpdatePatternsUnderLock();

        const auto vpUrlRow = term->_mainBuffer->GetCursor().GetPosition().y;
        const auto visStart = term->_VisibleStartIndex();
        const auto viewportRow = vpUrlRow - visStart;

        auto interval = term->GetHyperlinkIntervalFromViewportPosition(til::point{ 0, viewportRow });
        VERIFY_IS_TRUE(interval.has_value(), L"Interval is found via viewport position.");
        VERIFY_ARE_EQUAL(interval->start.y, viewportRow, L"Interval start row is viewport-relative.");
        VERIFY_ARE_EQUAL(interval->start.x, 0, L"Interval starts at column 0.");
    }
}
