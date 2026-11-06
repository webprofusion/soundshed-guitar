// Tap or scroll (src/ui/widgets/DragGesture.h): a press on a list stays a tap while a finger wanders a
// little, and what it becomes after that is decided once.

#include "ui/widgets/DragGesture.h"

#include <iostream>
#include <string>

namespace
{
using nanoq::ui::DragGesture;
using nanoq::ui::kDragSlop;
using State = DragGesture::State;

int gFailures = 0;

void Expect(bool passed, const std::string& what)
{
    if (!passed)
    {
        std::cerr << "[DragGestureTests] " << what << std::endl;
        ++gFailures;
    }
}

void WanderIsATap()
{
    DragGesture g;
    g.Press(100.0f, 50.0f);
    Expect(g.Current() == State::Waiting, "a press starts out waiting");
    Expect(g.Move(100.0f + kDragSlop - 1.0f, 50.0f) == State::Waiting, "a little wander along the list");
    Expect(g.Move(100.0f, 50.0f + kDragSlop - 1.0f) == State::Waiting, "a little wander across it");
    Expect(g.Move(100.0f - 6.0f, 50.0f - 6.0f) == State::Waiting, "a little wander both ways");
}

void AlongIsAScroll()
{
    DragGesture g;
    g.Press(100.0f, 50.0f);
    Expect(g.Move(100.0f - kDragSlop, 50.0f) == State::Scrolling, "up the list scrolls");
    Expect(g.ScrolledBy(100.0f - 40.0f) == 40.0f, "the offset grows as the pointer goes up the list");
    Expect(g.ScrolledBy(100.0f + 25.0f) == -25.0f, "and shrinks as it comes back past the press");

    g.Press(100.0f, 50.0f);
    Expect(g.Move(100.0f + kDragSlop, 50.0f) == State::Scrolling, "down the list scrolls");
}

void AcrossIsPassedOn()
{
    DragGesture g;
    g.Press(100.0f, 50.0f);
    Expect(g.Move(103.0f, 50.0f + kDragSlop) == State::Passed, "across the list isn't a scroll");
}

void DecidedOnce()
{
    DragGesture g;
    g.Press(100.0f, 50.0f);
    Expect(g.Move(100.0f, 70.0f) == State::Passed, "goes across first");
    Expect(g.Move(300.0f, 70.0f) == State::Passed, "and a later turn along the list doesn't scroll");

    g.Press(100.0f, 50.0f);
    Expect(g.Move(130.0f, 50.0f) == State::Scrolling, "goes along first");
    Expect(g.Move(130.0f, 300.0f) == State::Scrolling, "and a later turn across it keeps scrolling");
    Expect(g.Move(100.0f, 50.0f) == State::Scrolling, "even back where it started");
}

void NewPressStartsAgain()
{
    DragGesture g;
    g.Press(100.0f, 50.0f);
    g.Move(200.0f, 50.0f);
    g.Press(10.0f, 10.0f);
    Expect(g.Current() == State::Waiting, "the next press is a tap until it moves");
}
} // namespace

int main()
{
    WanderIsATap();
    AlongIsAScroll();
    AcrossIsPassedOn();
    DecidedOnce();
    NewPressStartsAgain();

    if (gFailures == 0)
    {
        std::cout << "[DragGestureTests] passed" << std::endl;
    }

    return gFailures == 0 ? 0 : 1;
}
