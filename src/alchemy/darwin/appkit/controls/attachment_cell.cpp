#include "alchemy/darwin/appkit/controls/g3.hpp"

// NSTextAttachmentCell: the cell that sizes and draws a text attachment, an
// image cell whose size is its image's. -cellFrameForTextContainer:... is
// its size at -cellBaselineOffset (the origin: the image sits on the
// baseline); drawing draws the image into the frame (the character-index and
// layout-manager forms draw the same); highlighting marks the cell
// highlighted. It wants to track the mouse but takes no action of its own
// (-trackMouse:... answers NO): clicks are the text view's delegate's
// business. NSTextAttachment does not make one by itself (its
// -attachmentCell is what was set).
namespace alchemy::gui::completion_kit::controls {

void install_attachment_cell(AppKit& kit) {
    using namespace appkit;
    auto* const k = &kit;
    const auto cell = kit.declare_class("NSTextAttachmentCell", "NSCell");
    struct AttachmentState final : ObjectPayload { Address attachment{}; };   // not retained (as AppKit: assign)
    const auto state_of = [k](Address self) { return cell_extension<AttachmentState>(*k, self); };   // (a cell: CellState carries it)
    kit.method(cell, "init", [k](CpuState& s, GuestMemory&) { s.x[0] = k->send_id(s.x[0], "initImageCell:", {0}); });
    kit.method(cell, "attachment", [state_of](CpuState& s, GuestMemory&) { s.x[0] = state_of(s.x[0])->attachment; });
    kit.method(cell, "setAttachment:", [state_of](CpuState& s, GuestMemory&) { state_of(s.x[0])->attachment = s.x[2]; }, "v@:@");
    const auto size_of = [k](Address self) -> Size {
        const auto image = k->send_id(self, "image");
        return image ? result_size(k->send(image, "size")) : Size{};
    };
    kit.method(cell, "cellSize", [size_of](CpuState& s, GuestMemory&) { return_size(s, size_of(s.x[0])); }, "{CGSize=dd}@:");
    kit.method(cell, "cellBaselineOffset", [](CpuState& s, GuestMemory&) { return_point(s, {0, 0}); }, "{CGPoint=dd}@:");
    // -cellFrameForTextContainer:proposedLineFragment:glyphPosition:characterIndex:
    kit.method(cell, "cellFrameForTextContainer:proposedLineFragment:glyphPosition:characterIndex:", [k, size_of](CpuState& s, GuestMemory&) {
        const auto offset = result_point(k->send(s.x[0], "cellBaselineOffset"));
        return_rect(s, {offset, size_of(s.x[0])});
    }, "{CGRect={CGPoint=dd}{CGSize=dd}}@:@{CGRect={CGPoint=dd}{CGSize=dd}}{CGPoint=dd}Q");
    const auto draw = [k](Address self, Rect frame) {
        const auto image = k->send_id(self, "image");
        if (!image) return;
        CallRegs registers;
        put_rect(registers, 0, frame);
        static_cast<void>(k->send(image, "drawInRect:", registers));
    };
    kit.method(cell, "drawWithFrame:inView:", [draw](CpuState& s, GuestMemory&) { draw(s.x[0], rect_arg(s)); }, "v@:{CGRect={CGPoint=dd}{CGSize=dd}}@");
    kit.method(cell, "drawWithFrame:inView:characterIndex:", [draw](CpuState& s, GuestMemory&) { draw(s.x[0], rect_arg(s)); },
               "v@:{CGRect={CGPoint=dd}{CGSize=dd}}@Q");
    kit.method(cell, "drawWithFrame:inView:characterIndex:layoutManager:", [draw](CpuState& s, GuestMemory&) { draw(s.x[0], rect_arg(s)); },
               "v@:{CGRect={CGPoint=dd}{CGSize=dd}}@Q@");
    kit.method(cell, "highlight:withFrame:inView:", [k](CpuState& s, GuestMemory&) {
        static_cast<void>(k->send_id(s.x[0], "setHighlighted:", {s.x[2] & 0xff}));
        if (s.x[3]) k->invalidate_view(s.x[3]);
    }, "v@:B{CGRect={CGPoint=dd}{CGSize=dd}}@");
    kit.method(cell, "wantsToTrackMouse", [](CpuState& s, GuestMemory&) { s.x[0] = 1; }, "B@:");
    kit.method(cell, "wantsToTrackMouseForEvent:inRect:ofView:atCharacterIndex:", [](CpuState& s, GuestMemory&) { s.x[0] = 1; },
               "B@:@{CGRect={CGPoint=dd}{CGSize=dd}}@Q");
    kit.method(cell, "trackMouse:inRect:ofView:untilMouseUp:", [](CpuState& s, GuestMemory&) { s.x[0] = 0; },
               "B@:@{CGRect={CGPoint=dd}{CGSize=dd}}@B");
    kit.method(cell, "trackMouse:inRect:ofView:atCharacterIndex:untilMouseUp:", [](CpuState& s, GuestMemory&) { s.x[0] = 0; },
               "B@:@{CGRect={CGPoint=dd}{CGSize=dd}}@QB");
}

} // namespace alchemy::gui::completion_kit::controls
