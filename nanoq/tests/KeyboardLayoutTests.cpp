// The on-screen keyboard's keys, Shift, and how far a field moves to clear the panel
// (src/ui/input/KeyboardLayout.h). How it draws and takes presses is driven through the running app.

#include "ui/input/KeyboardLayout.h"

#include <cmath>
#include <iostream>
#include <string>

using namespace nanoq::ui::keyboard;

namespace
{
int gFailures = 0;

void Check(bool ok, const std::string& what)
{
    if (!ok)
    {
        std::cerr << "[KeyboardLayoutTests] " << what << std::endl;
        ++gFailures;
    }
}

bool Near(float a, float b)
{
    return std::fabs(a - b) < 0.01f;
}

void TestEveryRowFillsTheWidth()
{
    for (const auto mode : {Mode::Letters, Mode::Symbols})
    {
        for (const auto& row : Rows(mode))
        {
            float units = 0.0f;

            for (const auto& key : row)
            {
                units += key.width;
            }

            Check(Near(units, kRowUnits), "a row is " + std::to_string(units) + " units wide");
        }
    }
}

// A person has to be able to type what the app asks for on either layer.
void TestEveryPrintableAsciiCharacterCanBeTyped()
{
    for (char32_t c = U'!'; c <= U'~'; ++c)
    {
        bool found = false;

        for (const auto mode : {Mode::Letters, Mode::Symbols})
        {
            for (const auto& row : Rows(mode))
            {
                for (const auto& key : row)
                {
                    found = found || (key.kind == KeyKind::Character && (key.lower == c || key.upper == c));
                }
            }
        }

        Check(found, "no key types " + Utf8(c));
    }
}

void TestKeysStayInsideThePanelAtTheSmallestScreen()
{
    const float height = PanelHeight(480.0f);
    Check(height < 240.0f, "the panel takes half of an 800x480 screen or more");

    for (const auto mode : {Mode::Letters, Mode::Symbols})
    {
        const auto keys = PlaceKeys(mode, 800.0f, 480.0f - height, height);

        for (std::size_t i = 0; i < keys.size(); ++i)
        {
            const auto& b = keys[i].box;
            Check(b.left >= 0.0f && b.right <= 800.0f && b.top >= 480.0f - height && b.bottom <= 480.0f,
                  "a key leaves the panel");
            Check(b.right - b.left >= 50.0f && b.bottom - b.top >= 36.0f, "a key is too small to tap");

            const float x = (b.left + b.right) * 0.5f;
            const float y = (b.top + b.bottom) * 0.5f;
            Check(KeyAt(keys, x, y) == static_cast<int>(i), "the middle of a key does not hit it");
        }
    }
}

void TestATapBetweenKeysStillLands()
{
    const float height = PanelHeight(480.0f);
    const auto keys = PlaceKeys(Mode::Letters, 800.0f, 480.0f - height, height);
    const float y = (keys[0].box.top + keys[0].box.bottom) * 0.5f;
    Check(KeyAt(keys, (keys[0].box.right + keys[1].box.left) * 0.5f, y) >= 0, "a tap between two keys is lost");
    Check(KeyAt(keys, 400.0f, 0.0f) == -1, "a tap above the panel hit a key");
}

void TestAWideScreenCentresTheKeys()
{
    const float height = PanelHeight(1080.0f);
    Check(Near(height, kMaxPanelHeight), "the panel grows without end");

    const auto keys = PlaceKeys(Mode::Letters, 1920.0f, 1080.0f - height, height);
    Check(Near(keys.front().box.left - kKeyGap * 0.5f, (1920.0f - kMaxKeysWidth) * 0.5f), "the keys are not centred");
}

void TestShift()
{
    Check(NextShift(Shift::Off) == Shift::Once, "first tap");
    Check(NextShift(Shift::Once) == Shift::Locked, "second tap");
    Check(NextShift(Shift::Locked) == Shift::Off, "third tap");
    Check(ShiftAfterCharacter(Shift::Once) == Shift::Off, "a capital ends Once");
    Check(ShiftAfterCharacter(Shift::Locked) == Shift::Locked, "a capital keeps Locked");
}

void TestFieldPan()
{
    const float panelTop = 255.0f;
    const float panelHeight = 225.0f;

    // Clear of the panel: nothing moves.
    Check(FieldPan(10.0f, 40.0f, panelTop, panelHeight) == 0.0f, "a field above the panel moved");

    // Under it: its foot ends up kFieldGap above the panel.
    const float pan = FieldPan(420.0f, 450.0f, panelTop, panelHeight);
    Check(Near(450.0f - pan + kFieldGap, panelTop), "a field under the panel did not clear it");
    Check(420.0f - pan >= 0.0f, "the field went off the top");

    // Taller than the space above the panel: its top stays on screen.
    Check(Near(FieldPan(40.0f, 400.0f, panelTop, panelHeight), 40.0f - kFieldGap), "a tall field lost its top");

    // Never by more than the panel, or the layer would leave a bare strip under the panel.
    Check(FieldPan(470.0f, 480.0f, panelTop, panelHeight) <= panelHeight, "the layer rose past the panel");
}

void TestUtf8()
{
    Check(Utf8(U'a') == "a", "ASCII");
    Check(Utf8(U'£') == "\xC2\xA3", "two bytes");
    Check(Utf8(U'€') == "\xE2\x82\xAC", "three bytes");
    Check(Utf8(U'\U0001F3B8') == "\xF0\x9F\x8E\xB8", "four bytes");
}
} // namespace

int main()
{
    TestEveryRowFillsTheWidth();
    TestEveryPrintableAsciiCharacterCanBeTyped();
    TestKeysStayInsideThePanelAtTheSmallestScreen();
    TestATapBetweenKeysStillLands();
    TestAWideScreenCentresTheKeys();
    TestShift();
    TestFieldPan();
    TestUtf8();

    if (gFailures == 0)
    {
        std::cout << "[KeyboardLayoutTests] ok" << std::endl;
    }

    return gFailures == 0 ? 0 : 1;
}
