# AppKit

**Covers:** NSApplication, NSWindow, NSView, NSEvent, resources (NSImage,
NSColor, NSCursor, pasteboards), standard controls, text, tables and
outlines, panels, documents, nib loading, asset catalogs and RTF; Carbon /
HIToolbox input. Also the additions installed by other entry points, still
called from their aggregators: QLab's controls (`controls/`) and windows
(`windows/`), Qt's (`appkit_extra.cpp`, `accessibility.cpp`,
`pasteboard.cpp`), Chrome's (`appkit_gaps.cpp`, `menus.cpp`,
`window_additions.cpp`, `spelling.cpp`, `selectors.cpp`), and
`application_services.cpp` and `bindings.cpp`. NSColor's named colours are
AppKit's (`resources.cpp`, `graphics.cpp`, `controls/appearance.cpp`).
Drawing (NSGraphicsContext, NSBezierPath, string and image drawing) is
CoreGraphics' `appkit_drawing.cpp`; NSFont and the text attribute keys are
CoreText's.

**Headless:** `appkit_headless.cpp` (alchemy.exe only), empty
`appkit_fx.cpp` in alchemy-fx (`darwin/shells.hpp`).

**Host dependencies:** the window server (`darwin/windowserver/`) and
`host::ui` (alerts, file dialogs, the shell, appearance and accessibility
settings, spell checking), `host::gpu` (image files, system text
rasterizing), `host::locale` (case mapping). On win32: user32, gdi32,
shell32, ole32, windowscodecs, comctl32 (TaskDialogIndirect), the Windows
Spell Checking API.

**Unsupported (explicit failures):** for example horizontal
NSCollectionViewFlowLayout scrolling, planar
NSBitmapImageRep data, and NSWindow frame autosave without NSUserDefaults.
Also -[NSSavePanel ok:] / -cancel: while the Windows file dialog is open,
-[NSImageRep CGImageForProposedRect:context:hints:] of a representation that
is not a bitmap, and +[NSColor colorWithColorSpace:components:count:] in
colour spaces other than gray, RGB and CMYK.

**Accessibility:** the NSAccessibility properties are stored on the object,
and this process's own AXUIElement / AXObserver client reads them
(`darwin/applicationservices/ax_client.cpp`): accessibility notifications
reach its observers. No assistive application on the host sees the tree.

**Unavailable, as documented:** remote notifications (no Apple Push
Notification service: -registerForRemoteNotifications fails through the
delegate with an NSCocoaErrorDomain error), and Finder tags in save panels
(-tagNames are the ones the application set); restorable state is encoded
and restored on request but not kept between launches.

**Toolbars and tabs (`window_tabs.cpp`):** a window's toolbar with items
and its tab bar show in a strip between the title bar and the content view
(the window grows below its top edge; empty, title-bar styling toolbars take
no space). Toolbar items are clicked, validated by -update and laid out by
kind; the size mode, style and centred items are kept, not drawn, and a
toolbar with items over a full-size content view stops explicitly. Window
tabs are real NSWindowTabGroups: tabs share a frame, the selected one is on
screen, the tab bar selects and closes tabs and offers -newWindowForTab:;
preferred tabbing joins windows with the same identifier, automatic tabbing
does not (Windows has no tab preference), and the tab overview and the
toolbar customization palette stop explicitly. -performWindowDragWithEvent:
runs the window server's move loop; NSWindowSharingNone keeps a window out
of screen capture.

**C functions (`c_functions.cpp`):** rectangle lists, tiled rectangles and
the classic bezels (gray edge sequences around a filled middle), nine-part
images, dotted frames, highlights, NSDrawBitmap and NSReadPixel draw and read
through the current context; NSCountWindows / NSWindowList list the
application's windows front to back; the depth helpers use AppKit's
NSWindowDepth encoding; NSRunAlertPanel and its forms run an NSAlert,
NSBeginAlertSheet a sheet; NSDisableScreenUpdates holds window presentation
until updates resume. There is no Services menu (item visibility is kept),
NSShowAnimationEffect draws nothing and tells its delegate at once, and
NSGetAlertPanel, NSCopyBits and NSGetWindowServerMemory stop explicitly.

**Status items and sounds (`status_items.cpp`):** each visible NSStatusItem
is an icon in the host's notification area (the window server's status
items; cloaked runs keep them off the taskbar): the button's image (template
images in the appearance's text color) or its title drawn as text, with the
button's tool tip. A click opens the item's menu, or sends the button's
action. Custom item views stop explicitly. NSSound plays through
AVAudioPlayer (WAVE, AIFF, CAF): play, pause, resume, stop, volume, time,
loops and the delegate; +soundNamed: finds named, bundled and
/System/Library/Sounds sounds (the system sounds are not installed).

**Layout guides and safe areas (`windows/auto_layout.cpp`):** NSLayoutGuide
is a constraint item like a view: a custom guide owned by a view gets its
frame from the solution, and removing it drops the constraints that refer
to it. A view's safe area is the part below a full-size content view's
title bar, within its superview's safe area, less the additional insets of
the view and its superviews; -safeAreaLayoutGuide, -layoutMarginsGuide (20
points inside the safe area, the standard spacing to a superview's edge)
and NSWindow's -contentLayoutGuide follow their insets in every layout
pass. -constraintsAffectingLayoutForOrientation: lists the NSLayoutConstraints
on that axis (not the engine's own content-size and autoresizing ones);
-hasAmbiguousLayout and -exerciseAmbiguityInLayout stop explicitly (the
solver always settles on one solution).

**Split views (`containers.cpp`, `split_view_controller.cpp`):** a resized
NSSplitView gives the change to the panes of the lowest holding priority;
divider drags ask the delegate for their limits. NSSplitViewController's
view is its split view, which it delegates; items' views load lazily and
take their preferred fraction (sidebars 0.15, content lists 0.28 / 0.33),
within their minimum and maximum thickness (inspectors 270). Collapsing
and the sidebar / inspector toggles apply at once; sidebars collapse below
the inline threshold and expand above it. The standard sidebar and
content-list thicknesses are not published, so they are unspecified;
archived split view controllers and items, and accessory view controllers,
stop explicitly.

**Text layout (`controls/text_layout.cpp`):** NSLayoutManager lays its
storage out with CoreText's framesetter (shaping, fallback, bidi, line
breaking and paragraph styles are CoreText's), container by container:
glyphs in character order (control and null glyphs for characters without
one), line fragments, used rectangles, the extra line fragment, point and
range queries, the line fragment and enclosing rectangle enumerations,
and drawing of glyphs, backgrounds and underlines into the current
context. NSTextView itself still draws in its storage's first attributes
(text.cpp), so the manager's geometry is for the storage's real
attributes. Exclusion paths, custom typesetters (the typesetter-facing
setters) and text blocks stop explicitly. NSTextStorage's attribute runs,
paragraphs, words and characters are immutable snapshots. NSTextList
formats its markers (the bullet characters are chosen here; the
documentation names only the keywords).

**Text actions (`controls/text_actions.cpp`, `controls/text_finder.cpp`):**
NSText's and NSTextView's attribute actions (alignment, font and colour
changes, underline, super- and subscript, outline, the font and ruler
pasteboards) change a rich view's selection through its storage (its
typing attributes with nothing selected, a plain view's whole text), ask
-shouldChangeTextInRange:... and end with -didChangeText. RTF goes through
rtf.cpp's subset (fonts, sizes, bold, italic, underline, colour, alignment,
indents) both ways; flattened RTFD stops, -writeRTFDToFile: writes a TXT.rtf
directory. Find (NSTextFinder and the find panel actions) searches the find
pasteboard's string; the find bar and the replace actions stop explicitly.
NSFontManager's Font menu actions record the change and -convertFont:
applies it (traits, sizes, weights). NSFontPanel (`controls/font_panel.cpp`)
is a real floating panel built from AppKit's controls: family and typeface
pop-ups, a size field and pop-up, a sample line; choosing sends -changeFont:
through -modifyFontViaPanel:, and -panelConvertFont: applies only what was
chosen (its modes, collections, favourites and effects are not provided).
The styles panel, TextKit 2, vertical layout and the link / list / table
panels stop explicitly. NSSecureTextField shows bullets (or nothing at all
with -setEchosBullets:NO), its field editor too, refuses copy and cut, and
records secure event input while edited (Carbon's EnableSecureEventInput
count, which IsSecureEventInputEnabled answers): this process's record only,
no keyboard input is shielded from other programs on the host.
NSTextAttachmentCell sizes and draws its image on the baseline. NSTextField
and NSTextInputContext keep their newer settings; the one keyboard input
source is the host's layout.

**Path items, transition layouts:** NSPathControl's -pathItems are
NSPathControlItems viewing its component cells (an item's title or image
changes its component); -setPathItems: makes a component per item.
NSCollectionViewTransitionLayout interpolates its current and next layouts'
attributes by -transitionProgress (elements only one layout has fade);
NSCollectionView's interactive transitions are not provided.

**Presentation, table editing, rotation:** NSViewController presents as a
sheet (a window attached to the presenter's), a modal window (a modal
session: input goes to it alone, the call does not block), a popover
(NSPopover) or through an animator object, and dismisses the same way;
child transitions swap views at once. NSTableView edits a cell in place:
the cell view's text field in a view-based table, the window's field editor
over the cell in a cell-based one (a double click edits an editable column
without a double action); the text reaches the data source unless Escape
cancelled it. Views rotate (-setFrameRotation:, -setFrameCenterRotation:,
-setBoundsRotation:, -rotateByAngle:): drawing, coordinate conversion and hit
testing follow, rotated bounds report their bounding rectangle, and clipping
is to that rectangle; rotating a layer-backed view stops explicitly.

**What the audited applications reference:** an application's own
NSSharingService (-initWithTitle:image:alternateImage:handler:) performs by
telling its delegate and running the handler, and NSSharingServicePicker
shows the services its delegate offers as a menu at the rect (no system
sharing services exist: none offered, or a dismissed menu, tells the
delegate nil). NSPrintPanel objects keep their options, job style, help
anchor, button title and accessory controllers; running one still stops.
NSPrintInfo keeps a print info dictionary (its attributes mirrored, plus job
disposition, saving URL and page range) and has no printer unless one is
set; NSPrintOperation, NSPrinter and NSPDFImageRep are the Quartz layer's
(coregraphics/appkit_printing.cpp: printed pages are images, text is not
selectable).
NSScreen aligns rects to its pixels; NSScreen, NSWindow and NSView make
CADisplayLinks at the display's rate (QuartzCore). Each window has its own
NSDockTile (kept, never shown: Windows has no dock). NSAlert and NSImage keep
their delegates; an image that cannot draw asks -imageDidNotDraw:inRect: for
a substitute. NSImage and NSImageRep list the types the host decodes (TIFF,
PNG, JPEG, GIF, BMP). NSCollectionView keeps its drag operation masks
(dragging items out still stops). NSWorkspace opens the running application
itself (activating it); launching another stops. Swipe tracking (no trackpad
gestures reach the window server) and dictionary lookups (no Dictionary
service) stop explicitly. -toggleTouchBarCustomizationPalette: is left
unanswered, so a menu item with it stays disabled: there is no Touch Bar.

**Colour panel:** NSColorPanel is a floating utility panel built from
AppKit's own controls (`controls/color_panel.cpp`): a swatch, a picker popup
and slider rows for the Gray, RGB and HSB pickers, an opacity row while it
shows alpha, and the accessory view under them. The other modes are kept by
-mode but shown with the RGB sliders. A change sets the colour, posts
NSColorPanelColorDidChangeNotification, updates the active NSColorWell and
sends the panel's action (-changeColor: unless set); -setColor: only posts
the notification; closing the panel deactivates the well. Colour lists and
dragging colours stop explicitly.

**Switches and level indicators** (`controls/indicators.cpp`): NSSwitch is
a pill with a knob that a click toggles (sending its action). NSLevelIndicator
forwards to its NSLevelIndicatorCell: relevancy, continuous and discrete
capacity and rating styles, warning and critical colours (inverted when the
critical level is the lower), tick marks, rating images, and editing by click
or drag when editable. The default range (0 to 2) is unverified.

**Speech** (`application_services.cpp`): NSSpeechSynthesizer speaks through
AVSpeechSynthesizer over the host's voices (host::speech): the voices and
their attributes, rate (words per minute) and volume, the word and finish
delegate callbacks, stopping and pausing at once or at a word. The pitch base
is kept, not applied; writing speech to a file stops explicitly; without host
voices the initializers return nil.

**macOS 11 toolbar items:** NSSearchToolbarItem's search field is its view
(begin / end search interaction move first responder); NSMenuToolbarItem pops
its menu up under the item (with the indicator chevron);
NSSharingServicePickerToolbarItem shows a sharing service picker for its
delegate's items; NSTrackingSeparatorToolbarItem is drawn as a separator (it
does not follow the split view's divider). NSHapticFeedbackManager's performer
does nothing (no Force Touch trackpad); NSHelpManager keeps context help but
shows none, registers no help books, and opening or searching one stops.

**Toolbar groups and grid layouts:** NSToolbarItemGroup shows its subitems in
the toolbar strip as one segmented control (the group's view unless it has
its own), selected by its selection mode; a chosen segment sends the
subitem's action, else the group's; the control representation is kept (groups
show expanded). NSCollectionViewGridLayout lays items out in a grid (column and
row limits, minimum and maximum item sizes, spacing, margins); items past the
row limit are hidden and the background colours are kept, not drawn.
NSCollectionView has sections (the flow layout stacks them, each with the
delegate's metrics, header and footer).

**Diffable data sources** (`controls/diffable.cpp`):
NSDiffableDataSourceSnapshot keeps section and item identifiers (compared
with -isEqual:) and is built, queried and edited through its documented
calls; duplicate or unknown identifiers raise. NSCollectionViewDiffableDataSource
and NSTableViewDiffableDataSource become their view's data source and make
items / cell views / row views / section header views through their provider
blocks (table section headers are rows before each section). Applying a
snapshot reloads the view: differences are not animated.

**Compositional layout** (`controls/compositional.cpp`):
NSCollectionViewCompositionalLayout builds its sections from a section or a
section provider block (its environment's container is the collection view's
size) and stacks them vertically with the configuration's spacing. Fractional,
absolute and estimated dimensions (estimated sizes are used as given: no
self-sizing); horizontal, vertical, `count` (equal shares), nested and custom
groups repeat down a section; item content insets and leading / top edge
spacing; anchored supplementary items, boundary headers and footers (section
and layout), and background decoration items drawn with the view class
registered for their kind, behind the items. Orthogonal scrolling sections lay
their groups out in one row that does not scroll. Horizontal scroll direction
and pinned boundary items fail as unsupported. The NSRectAlignment and
NSDirectionalRectEdge raw values follow their documented order (not verified).

**Tree controller** (`bindings.cpp`, with NSObjectController and
NSArrayController): NSTreeNode (represented object, parent, child nodes and
mutableChildNodes through KVC, index paths, descendants, sorting) and
NSTreeController. Its arranged objects are a tree of NSTreeNodes built from
the content (the top level) and each object's children key path, cut at the
leaf key path, sorted at every level by the sort descriptors; model objects'
children are observed, so changes made elsewhere rebuild the tree. Selection
is by index paths (selectionIndexPaths, selectedNodes, selectedObjects, the
selection proxy). Insertions, removals and moves edit the model through
-mutableArrayValueForKeyPath: (the content for the top level); the last
index of an insertion counts in the model's order, and a move's destination
is a path in the tree without the moved nodes. The count key path is kept but
not used (children are read at once). A tree controller decoded from a nib
fails explicitly: its archive keys are not documented.

**Table and outline bindings** (`containers.cpp`'s `install_table_bindings`,
`bindings.cpp`): NSTableView's "content" (an array controller's
arrangedObjects) and "selectionIndexes", NSOutlineView's "content" (a tree
controller's arrangedObjects: its NSTreeNodes are the items) and
"selectionIndexPaths" (expanding the selected items' ancestors), and a
column's "value" ("arrangedObjects.<key path>", resolved per row, with the
binding's value transformer and placeholders; edits go back through it).
While content is bound the table asks it, not its data source (-dataSource
still answers the application's). The user's selection goes back to the
selection binding. Expanded outline items stay expanded across new content
(by represented object). Without a column binding a row's object value is the
row's object (an outline item's represented object), and view-based cell
views get it as their objectValue (as with a data source answering object
values). Model objects' own key changes show when the table next reads them;
the other table bindings (sortDescriptors, enabled, font, ...) are not
provided.

**Token fields** (`controls/token_field.cpp`): NSTokenField's value is an
array of represented objects drawn as pills (plain text for
NSTokenStyleNone). Editing uses the field editor on the tokens' editing
strings; when editing ends (or the string value is set) the text is split at
the tokenizing characters (a comma by default), and the delegate maps pieces
to objects and decides which new ones to add. Tokens made while typing,
completions and token menus are not provided.

**Browsers** (`controls/browser.cpp`): NSBrowser shows a hierarchy in
columns from an item-based, passive or active delegate (an active delegate
fills a one-column NSMatrix; -matrixInColumn: is not provided). Selecting a branch loads the next column; up to
-maxVisibleColumns columns share the width, titled columns draw their
titles; clicks and the arrow keys select and send the action (a double click
the double action); paths and the selection index path follow the selection.
Single selection only; columns do not scroll vertically.

**Matrices** (`controls/matrix.cpp`): NSMatrix keeps a row-major grid of
cells of one size (made from the prototype, the cell class or NSActionCell)
with intercell spacing, laid out from the top left; rows and columns are
added, inserted, removed, renewed and sorted. The radio, highlight, list and
track modes select as AppKit documents: one selected radio cell whose state
is on, highlight-mode clicks moving a cell to its next state, list-mode
selections extended by dragging (by rectangle or in row order), Shift and
Command. Clicks, Space on the key cell and cells' key equivalents send the
cell's or the matrix's action with the matrix as sender (a double click then
the double action); arrow keys move the key cell. The NSControl value API
reads the selected cell; -setEnabled: and -setFont: apply to every cell.
The mode values follow the constants' declaration order (unverified). Not
provided: editing text in cells (-selectText... stop explicitly), cells'
own tracking in track mode, hover tooltips (they are kept), and NSMatrix in
nibs.

**Nib keys:** the decoders read the keys Interface Builder archives for the
controls, cells, tables, scroll views, boxes and menus QLab's nibs use; each
decoder names the keys it leaves out and why (design-time values, features
not modelled such as Writing Tools or text completion). The control-level
copies of cell settings are applied to the cell; NSControlContinuous and
NSStepper's range keys are not (archived as the complement of the cell's
flag, and as zeros). Kept but without effect: table styles and automatic
row heights, pop-up arrow positions, button periodic delays (buttons do not
repeat while held), scroll-view magnification bounds (documents draw at
magnification 1; another magnification stops explicitly).
NSUserDefaultsController applies changes immediately even when a nib says
NSAppliesImmediately NO (deferred application is not modelled).

## Moved from darwin/identities/

* `constants_and_shells.cpp`: pasteboard, document and notification constants, NSBitsPerPixelFromDepth, NSStatusBar/NSStackView shells (the support kit).
* `base_classes.cpp`, `input_sources.cpp`, `color_alpha.cpp`, `carbon_events.cpp`, `class_shells.cpp`: headless, every build: the base classes and NSApp, TIS keys, +[NSColor ignoresAlpha], Carbon events (none delivered), and the AppKit class shells.
