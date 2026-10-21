#include "controls_fixture.h"
#include <stdio.h>
#include <string.h>
/* AppKit controls guest fixture (owner: AppKit). ARM64 guest subclasses of
 * the host controls with instance variables, the way wxOSX subclasses them
 * (wxNSSlider, wxNSComboBox ...): super calls into host methods that call
 * back into guest overrides, double arguments and NSSize returns, the combo
 * box's delegate, and HIShape regions through pointers and double
 * arguments. Creates no window (no window server needed). */

static int failures;
static void check(int ok, const char *what) {
    if (!ok) { printf("controls_fixture: FAIL %s\n", what); failures++; }
}
static NSString *text(const char *bytes) { return [NSString stringWithUTF8String:bytes]; }

@interface FixtureSlider : NSSlider { @public int sets; double requested; }
@end
@implementation FixtureSlider
- (void)setDoubleValue:(double)value { sets++; requested = value; [super setDoubleValue:value]; }
@end

@interface FixtureCombo : NSComboBox { @public int padding[3]; }
@end
@implementation FixtureCombo
@end

@interface FixtureComboDelegate : NSObject { @public int changes; }
@end
@implementation FixtureComboDelegate
- (void)comboBoxSelectionDidChange:(id)notification { changes++; }
@end

@interface FixtureTarget : NSObject { @public int actions; double seen; }
@end
@implementation FixtureTarget
- (void)changed:(id)sender { actions++; seen = [sender doubleValue]; }
@end

int main(void) {
    [NSApplication sharedApplication];
    puts("controls_fixture: start");

    FixtureSlider *slider = [[FixtureSlider alloc] initWithFrame:NSMakeRect(0, 0, 200, 24)];
    [slider setMaxValue:10];
    [slider setDoubleValue:15];
    check(slider->sets == 1 && slider->requested == 15 && [slider doubleValue] == 10, "guest -setDoubleValue: override and clamping");
    [slider setNumberOfTickMarks:5];
    check([slider closestTickMarkValueToValue:6.0] == 5.0 && ![slider isVertical], "tick marks");
    FixtureTarget *target = [[FixtureTarget alloc] init];
    [slider setTarget:target];
    [slider setAction:@selector(changed:)];
    check([slider sendAction:@selector(changed:) to:target] && target->actions == 1 && target->seen == 10, "target-action");

    NSStepper *stepper = [[NSStepper alloc] initWithFrame:NSMakeRect(0, 0, 19, 28)];
    [stepper setMaxValue:3];
    [stepper setDoubleValue:2];
    check([stepper valueWraps] && [stepper doubleValue] == 2, "stepper");
    puts("controls_fixture: sliders ok");

    FixtureCombo *combo = [[FixtureCombo alloc] initWithFrame:NSMakeRect(0, 0, 150, 24)];
    FixtureComboDelegate *delegate = [[FixtureComboDelegate alloc] init];
    [combo setDelegate:delegate];
    [combo addItemWithObjectValue:text("Blender 4.5")];
    [combo addItemWithObjectValue:text("Blender 4.4")];
    [combo selectItemAtIndex:1];
    check([combo numberOfItems] == 2 && [combo indexOfSelectedItem] == 1 && delegate->changes == 1 &&
          strcmp([[combo stringValue] UTF8String], "Blender 4.4") == 0, "combo box selection");
    puts("controls_fixture: combo ok");

    NSSize paper = [[NSPrintInfo sharedPrintInfo] paperSize];
    check(paper.width == 612 && paper.height == 792, "print info paper size");
    NSPasteboardItem *item = [[NSPasteboardItem alloc] init];
    [item setString:text("license") forType:text("public.utf8-plain-text")];
    check(strcmp([[item stringForType:text("public.utf8-plain-text")] UTF8String], "license") == 0, "pasteboard item");

    CGRect left = CGRectMake(0, 0, 10, 10), right = CGRectMake(10, 0, 10, 10), bounds;
    HIShapeRef square = HIShapeCreateWithRect(&left);
    HIMutableShapeRef region = HIShapeCreateMutableCopy(square);
    check(HIShapeUnionWithRect(region, &right) == 0 && HIShapeIsRectangular(region), "HIShapeUnionWithRect");
    check(HIShapeOffset(region, 2.5, -1) == 0 && HIShapeGetBounds(region, &bounds) == &bounds && bounds.origin.x == 2.5 &&
          bounds.origin.y == -1 && bounds.size.width == 20 && bounds.size.height == 10, "HIShapeOffset / HIShapeGetBounds");
    CGPoint inside = CGPointMake(20, 0), outside = CGPointMake(1, 0);
    check(HIShapeContainsPoint(region, &inside) && !HIShapeContainsPoint(region, &outside), "HIShapeContainsPoint");
    HIShapeRef overlap = HIShapeCreateIntersection(square, region);
    HIShapeGetBounds(overlap, &bounds);
    check(bounds.origin.x == 2.5 && bounds.size.width == 7.5 && bounds.size.height == 9, "HIShapeCreateIntersection");
    HIShapeDifference(region, region, region);
    check(HIShapeIsEmpty(region), "HIShapeDifference");
    CFRelease(overlap);
    CFRelease(region);
    CFRelease(square);
    puts("controls_fixture: hishape ok");

    if (failures) return 1;
    puts("controls_fixture: done");
    return 0;
}
