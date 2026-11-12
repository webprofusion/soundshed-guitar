// Tap or scroll (src/ui/widgets/DragGesture.h): a press on a list stays a tap while a finger wanders a
// little, and what it becomes after that is decided once. And a finger's hold on a chain card
// (src/ui/widgets/TouchHold.h): rested long enough it picks the card up; moved sooner it scrolls.

#include "ui/widgets/DragGesture.h"
#include "ui/widgets/TouchHold.h"

#include <chrono>
#include <iostream>
#include <string>
#include <thread>

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

void HoldPicksUp()
{
    using nanoq::ui::TouchHold;
    TouchHold::Reset();
    TouchHold::Press(100.0f, 50.0f);
    Expect(!TouchHold::Tick() && !TouchHold::Held(), "a fresh press is not held yet");
    TouchHold::Move(100.0f + TouchHold::kSlop - 1.0f, 50.0f); // a finger's wander
    std::this_thread::sleep_for(std::chrono::duration<double>(TouchHold::kHoldSeconds + 0.03));
    Expect(TouchHold::Tick(), "a press rested past the hold is picked up");
    Expect(TouchHold::Held(), "and stays held");
    Expect(!TouchHold::Tick(), "the hold takes once");
    TouchHold::Move(300.0f, 50.0f);
    Expect(TouchHold::Held(), "moving a held card keeps it held: it is being dragged");
    TouchHold::Reset();
    Expect(!TouchHold::Held(), "the release lets it go");
}

void SwipeIsNoHold()
{
    using nanoq::ui::TouchHold;
    TouchHold::Reset();
    TouchHold::Press(100.0f, 50.0f);
    TouchHold::Move(100.0f + TouchHold::kSlop + 4.0f, 50.0f);
    std::this_thread::sleep_for(std::chrono::duration<double>(TouchHold::kHoldSeconds + 0.03));
    Expect(!TouchHold::Tick() && !TouchHold::Held(), "a press that moved before the hold is a swipe, never picked up");
    TouchHold::Reset();
}

int main()
{
    WanderIsATap();
    AlongIsAScroll();
    AcrossIsPassedOn();
    DecidedOnce();
    NewPressStartsAgain();
    HoldPicksUp();
    SwipeIsNoHold();

    if (gFailures == 0)
    {
        std::cout << "[DragGestureTests] passed" << std::endl;
    }

    return gFailures == 0 ? 0 : 1;
}
