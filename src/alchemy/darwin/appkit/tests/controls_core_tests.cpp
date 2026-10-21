// Host-only tests of the G3 control cores (src/alchemy/darwin/kit/completion/controls/):
// NSSegmentedControl segment layout and hit testing, NSPathControl path
// splitting and component placement, NSCollectionViewFlowLayout geometry
// (layout.cpp), and NSDatePicker element formatting and stepping
// (date_elements.cpp).
#include "alchemy/darwin/appkit/controls/layout.hpp"
#include "alchemy/darwin/appkit/controls/date_elements.hpp"

#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
namespace layout = alchemy::gui::completion_kit::controls::layout;
namespace elements = alchemy::gui::completion_kit::controls::date_elements;

void require(bool condition, const std::string& what) {
    if (!condition) throw std::runtime_error(what);
}
std::string narrow(const std::u16string& value) {
    std::string out;
    for (const auto c : value) out.push_back(c < 0x80 ? static_cast<char>(c) : '?');
    return out;
}
std::string spans_text(const std::vector<layout::Span>& spans) {
    std::string out;
    for (const auto& s : spans) out += "[" + std::to_string(static_cast<int>(s.x)) + "+" + std::to_string(static_cast<int>(s.width)) + "]";
    return out;
}

void segment_layout() {
    const std::vector<layout::SegmentSize> three{{0, 40}, {0, 60}, {0, 20}};
    // fit: content widths, the rest blank.
    auto spans = layout::segment_spans(three, 10, 200, layout::distribution::fit);
    require(spans_text(spans) == "[10+40][50+60][110+20]", "fit " + spans_text(spans));
    // fill: content plus an equal share of the spare 80.
    spans = layout::segment_spans(three, 0, 200, layout::distribution::fill);
    require(spans.size() == 3 && std::abs(spans[0].width - 66.67) < 1 && std::abs(spans[1].width - 86.67) < 1 &&
                spans[2].x + spans[2].width == 200, "fill " + spans_text(spans));
    // fill equally: one width each.
    spans = layout::segment_spans(three, 0, 90, layout::distribution::fill_equally);
    require(spans_text(spans) == "[0+30][30+30][60+30]", "fill equally keeps equal widths when short " + spans_text(spans));
    // fill proportionally: 240 shared 2:3:1.
    spans = layout::segment_spans(three, 0, 240, layout::distribution::fill_proportionally);
    require(spans_text(spans) == "[0+80][80+120][200+40]", "fill proportionally " + spans_text(spans));
    // Fixed widths stay fixed; dynamic ones share the rest.
    const std::vector<layout::SegmentSize> mixed{{50, 10}, {0, 30}, {0, 30}};
    spans = layout::segment_spans(mixed, 0, 170, layout::distribution::fill_equally);
    require(spans_text(spans) == "[0+50][50+60][110+60]", "fixed width segments " + spans_text(spans));
    // Short of room: dynamic segments shrink with their content.
    spans = layout::segment_spans(three, 0, 60, layout::distribution::fill);
    require(spans_text(spans) == "[0+20][20+30][50+10]", "shrinking " + spans_text(spans));
    // Workspace.nib's view switcher: two segments filling 82 points equally.
    const std::vector<layout::SegmentSize> two{{0, 36}, {0, 44}};
    spans = layout::segment_spans(two, 0, 82, layout::distribution::fill_equally);
    require(spans_text(spans) == "[0+41][41+41]", "two equal segments " + spans_text(spans));
    // Hit testing: boundaries belong to the right-hand segment; outside is -1.
    require(layout::segment_at(spans, 0) == 0 && layout::segment_at(spans, 40.9) == 0 && layout::segment_at(spans, 41) == 1 &&
                layout::segment_at(spans, 81.5) == 1 && layout::segment_at(spans, 82) == -1 && layout::segment_at(spans, -1) == -1,
            "segment hit testing");
    // Fitting widths.
    require(layout::segments_fitting_width(three, layout::distribution::fit) == 120, "fitting width (fit)");
    require(layout::segments_fitting_width(three, layout::distribution::fill_equally) == 180, "fitting width (equal: 3 x 60)");
    require(layout::segments_fitting_width(mixed, layout::distribution::fill) == 110, "fitting width with a fixed segment");
}

void path_split() {
    auto parts = layout::split_path("/Users/bob//Music/song.wav/", "Macintosh HD");
    require(parts.size() == 5, "component count " + std::to_string(parts.size()));
    require(parts[0].title == "Macintosh HD" && parts[0].path == "/", "root component");
    require(parts[1].title == "Users" && parts[1].path == "/Users", "first component");
    require(parts[3].title == "Music" && parts[3].path == "/Users/bob/Music", "repeated separators collapse");
    require(parts[4].title == "song.wav" && parts[4].path == "/Users/bob/Music/song.wav", "trailing separator ignored");
    parts = layout::split_path("/Volumes/Show Drive/Cues/a.mov", "Macintosh HD");
    require(parts.size() == 3 && parts[0].title == "Show Drive" && parts[0].path == "/Volumes/Show Drive" &&
                parts[2].path == "/Volumes/Show Drive/Cues/a.mov", "a mounted volume is its own root");
    parts = layout::split_path("/", "Disk");
    require(parts.size() == 1 && parts[0].title == "Disk" && parts[0].path == "/", "the root alone");
    parts = layout::split_path("docs/readme", "Disk");
    require(parts.size() == 2 && parts[0].title == "docs" && parts[1].path == "docs/readme", "relative path");
    // Placement: everything fits.
    const std::vector<double> widths{80, 50, 60};
    auto spans = layout::path_spans(widths, 4, 300, 14, 22);
    require(spans_text(spans) == "[4+80][98+50][162+60]", "path spans that fit " + spans_text(spans));
    // Short: the first component shrinks first, down to the minimum.
    spans = layout::path_spans(widths, 0, 168, 14, 22);
    require(spans_text(spans) == "[0+30][44+50][108+60]", "first component shrinks " + spans_text(spans));
    spans = layout::path_spans(widths, 0, 120, 14, 22);
    require(spans_text(spans) == "[0+0][0+22][36+60]", "then the first is hidden, the last stays whole " + spans_text(spans));
    spans = layout::path_spans(widths, 0, 70, 14, 22);
    require(spans_text(spans) == "[0+0][0+0][0+60]", "earlier components are hidden for the last " + spans_text(spans));
    spans = layout::path_spans(widths, 0, 40, 14, 22);
    require(spans_text(spans) == "[0+0][0+0][0+40]", "the last component alone truncates " + spans_text(spans));
}

void flow_layout() {
    layout::FlowLayout flow;
    flow.item = {100, 80};
    flow.interitem_spacing = 10;
    flow.line_spacing = 12;
    flow.section_inset = {20, 20, 20, 20};
    // 470 wide: room 430 holds exactly 4 columns (4 * 100 + 3 * 10).
    auto result = layout::flow_layout(flow, 7, 470);
    require(result.columns == 4, "columns " + std::to_string(result.columns));
    require(result.frames.size() == 7 && result.frames[0].origin.x == 20 && result.frames[0].origin.y == 20, "first item at the inset");
    require(result.frames[3].origin.x == 350 && result.frames[4].origin.x == 20 && result.frames[4].origin.y == 112, "row wrap");
    require(result.content.height == 20 + 80 + 12 + 80 + 20 && result.content.width == 470, "content size");
    // Justified rows: 500 wide leaves 30 spare for 3 gaps -> spacing 20.
    result = layout::flow_layout(flow, 5, 500);
    require(result.columns == 4 && result.frames[1].origin.x == 140 && result.frames[3].origin.x == 380, "justified spacing");
    require(result.frames[4].origin.x == 20 && result.frames[4].origin.y == 112, "the partial last row keeps the columns");
    // A single partial row keeps the minimum spacing.
    result = layout::flow_layout(flow, 2, 500);
    require(result.frames[1].origin.x == 130, "a lone partial row is not spread");
    // Hit testing.
    require(layout::item_at(result, {25, 25}) == 0 && layout::item_at(result, {125, 25}) == -1 && layout::item_at(result, {135, 30}) == 1,
            "flow item hit testing");
    // Header and footer.
    flow.header = 30;
    flow.footer = 10;
    result = layout::flow_layout(flow, 0, 300);
    require(result.frames.empty() && result.content.height == 30 + 20 + 20 + 10, "empty section with header and footer");
}

void date_picker_elements() {
    // 2026-10-14 15:05:09 UTC; reference date is 2001-01-01.
    const auto civil = elements::Civil{2026, 10, 14, 15, 5, 9};
    auto shown = elements::format(elements::year_month_day | elements::hour_minute_second, civil);
    require(narrow(shown.text) == "10/14/2026, 3:05:09 PM", "date and time: " + narrow(shown.text));
    require(shown.fields.size() == 7, "fields " + std::to_string(shown.fields.size()));
    require(shown.fields[0].kind == elements::Field::month && shown.fields[0].start == 0 && shown.fields[0].length == 2, "month field");
    require(shown.fields[2].kind == elements::Field::year && narrow(shown.text.substr(shown.fields[2].start, shown.fields[2].length)) == "2026", "year field");
    require(shown.fields[6].kind == elements::Field::am_pm && narrow(shown.text.substr(shown.fields[6].start, shown.fields[6].length)) == "PM", "am/pm field");
    shown = elements::format(elements::year_month, civil);
    require(narrow(shown.text) == "10/2026" && shown.fields.size() == 2, "year and month: " + narrow(shown.text));
    shown = elements::format(elements::hour_minute, {2026, 1, 2, 0, 7, 0});
    require(narrow(shown.text) == "12:07 AM", "midnight hour: " + narrow(shown.text));
    shown = elements::format(elements::year_month_day, {2026, 1, 2, 0, 0, 0});
    require(narrow(shown.text) == "1/2/2026", "no zero padding: " + narrow(shown.text));
    // The field under a character index.
    shown = elements::format(elements::year_month_day, civil);
    require(elements::field_at(shown, 0) == 0 && elements::field_at(shown, 3) == 1 && elements::field_at(shown, 2) == 0 &&
                elements::field_at(shown, 9) == 2, "field at index");
    // Stepping wraps inside the field (no carry), days by the month's length.
    auto stepped = elements::step({2026, 2, 28, 23, 59, 59}, elements::Field::day, 1);
    require(stepped.day == 1 && stepped.month == 2, "day wraps within February");
    stepped = elements::step({2024, 2, 28, 0, 0, 0}, elements::Field::day, 1);
    require(stepped.day == 29, "leap February");
    stepped = elements::step({2026, 1, 31, 0, 0, 0}, elements::Field::month, 1);
    require(stepped.month == 2 && stepped.day == 28, "month step clamps the day");
    stepped = elements::step({2026, 12, 1, 0, 59, 0}, elements::Field::minute, 1);
    require(stepped.minute == 0 && stepped.hour == 0, "minutes wrap without carry");
    stepped = elements::step({2026, 12, 1, 0, 0, 0}, elements::Field::month, -1);
    require(stepped.month == 11, "month down");
    stepped = elements::step({2026, 12, 1, 1, 0, 0}, elements::Field::am_pm, 1);
    require(stepped.hour == 13, "AM/PM toggles by 12 hours");
    stepped = elements::step({2026, 12, 1, 23, 0, 0}, elements::Field::hour, 1);
    require(stepped.hour == 0, "hours wrap");
    stepped = elements::step({2026, 12, 1, 0, 0, 0}, elements::Field::year, -1);
    require(stepped.year == 2025, "year");
    // Typing digits into a field.
    auto typed = elements::type_digit({2026, 10, 14, 15, 5, 9}, elements::Field::month, 0, 1);
    require(typed.value.month == 1 && typed.pending == 1 && !typed.complete, "a first digit that may take another");
    typed = elements::type_digit(typed.value, elements::Field::month, typed.pending, 3);
    require(typed.value.month == 3 && typed.pending == 0 && typed.complete, "13 is not a month: 3 starts over and completes the field");
    typed = elements::type_digit({2026, 10, 14, 15, 5, 9}, elements::Field::day, 1, 2);
    require(typed.value.day == 12 && typed.pending == 0 && typed.complete, "two digits complete a day");
}

} // namespace

int main() {
    try {
        segment_layout();
        path_split();
        flow_layout();
        date_picker_elements();
    } catch (const std::exception& error) {
        std::cerr << "alchemy_gui_appkit_controls_unit: " << error.what() << '\n';
        return 1;
    }
    std::cout << "alchemy_gui_appkit_controls_unit: passed\n";
    return 0;
}
