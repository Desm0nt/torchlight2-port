// [TL2] Tap = click on the console menus (DM_TL2_POINTER, default off until checked on the phone; docs/tl2/TL2_PORT.md "Тап по пунктам меню").
// TL2 Switch keeps Panic Button's console UI "PUI" (RTTI N3PUI7ElementE / 4List / 6Button / 6Radial / 7Manager); the PC UI is gone from the
// code (CEGUI 0.6.2 is linked, but System / layout loading were dead-stripped: the static String objects for System::EventGUISheetChanged
// (main+0x7680168) and the layout handler's "GUILayout" (main+0x76826d0) are referenced only by their own static initializer 0x179d850;
// WindowManager::createWindow 0x17d5890 has no caller outside CEGUI; no PC GUI class in RTTI; the PAKs still carry MEDIA/UI/*.LAYOUT, unused).
// So a tap is a click through the console menus' own code, with no pad emulation:
//   * the item under the finger is found in the PUI tree (laid-out rects, below);
//   * the owning menu is the top of the console UI stack; its focus is moved the way its own navigation code moves it (SetHighlighted(old, 0),
//     focus index = item, SetHighlighted(new, 1): MainMenu 0x145954..0x1459b8, ButtonMenu 0x739690) or, for list menus, with the list's
//     own SetSelected (0x1a02460, what their Up / Down code calls);
//   * the activation is the menu's own: the UI manager's dispatch (vt slot 0 = 0x143800, hands the event to the top menu) gets the logical
//     Accept event {type 0 press, code 1} (IsAccept 0x1139f0), so the confirm branch of that menu acts on the item now focused (MainMenu
//     0x145b60: switch on the item id; it needs the button's enabled byte +0xc3). The outer quarters of a button row get the logical Left /
//     Right {0, 4} / {0, 5} instead (value rows: MainMenu item types 0x26..0x28 / 0x36, character create).
//   * button menus: one tap = focus + activate. List rows: a tap selects (= hover / details), a tap on the selected row activates.
// Codes: IsUp 0x113950 = 6, IsDown 0x1139a0 = 7, IsAccept 0x1139f0 = 1, IsBack 0x13acd0 = 0; event {u32 type, u32 code, u32 axis, f32 value}.
//
// PUI layout (main v5 build a0c6c2d2, offsets = main.raw; all checked word for word at the first poll):
//   PUI::Manager   static main+0x927a970 (GOT 0x23a3678; init 0x19fef80 from the UI setup 0x140f00 with 1920x1080): +0x2f8 / +0x2fc canvas
//                  width / height (0x19ff3d8), +0x308 root element (0x19ff450); Render 0x19f5090 draws the root twice, pass 0 then pass 1.
//   PUI::Element   vtable main+0x23651f8: +0x38 parent, +0x40 first child, +0x58 next sibling (children drawn first to last = bottom to top),
//                  +0x60 l,t,r,b laid-out rect in canvas units (Layout vt+0x70 = 0x19f5a90), +0x74 drawn (Render 0x19f57b0 returns before
//                  drawing it and its children while 0), +0x80 layer (1 = second pass, above every pass-0 element).
//   PUI::List      vtable main+0x23656d8 (Draw 0x1a01ab0): +0x84 l,t,r,b row area, +0x100 row height (int), +0x104 rows, +0x108 selected
//                  (SetSelected 0x1a02460, GetSelected 0x1a02600), +0x10c scroll, +0x116 shows the selection; row i: y = t + i*h - scroll.
//                  +0x110 max scroll; EnsureVisible 0x1a024b0 (SetSelected's tail) scrolls the least to show a row; SetRowHeight 0x1a02150
//                  (and SetCount 0x1a023a0) clamp +0x10c to 0..max and place the scroll thumb (+0xb4..+0xc0 in the track +0xa4..+0xb0).
//                  Scroll arrows (Draw 0x1a01bc0..0x1a01d98, elements drawn this frame by +0x74): +0x118 / +0x128 up (scroll > 0), +0x120 / +0x130
//                  down (scroll < max and a row selected). On Switch they are only indicators: the stick / D-pad moves the selection and
//                  EnsureVisible scrolls.
//   PUI::Button    vtable main+0x2365838: +0xc4 highlighted (SetHighlighted 0x1a05020), +0xc3 enabled (0x1a05060; Accept is refused while 0).
// Menus (CConsoleUIMenu subclasses, vt slot 0 = HandleInput(menu, player, event)):
//   button family A: MainMenu vt 0x230ddb8 (0x1457e0), Network vt 0x2309e28 (0x112ab0), CreateCharacter vt 0x232b008 (0x75fde0):
//                    items +0x48 (0x28-byte entries, id at +0), count +0x58, focus +0x5c, Button* array +0x98.
//   button family B: Options vt 0x23043d8 (0xc05f0), PlayerInvite vt 0x232a938 (0x73bb10), PartyMenu vt 0x232b2e8 (0x766fa0), base ButtonMenu
//                    (build 0x739210): items +0x30, count +0x38, focus +0x3c, Button* vector +0x50.
//   list menus:      any top menu holding the tapped PUI::List pointer in its first 0x400 bytes (CharacterList family 0x757310 / 0x7588b0 /
//                    0x762420 with the list at +0x40, Waypoints 0x765ab0, SkillsRespec 0x771580, PlayerInvite / PartyMenu lists): they read
//                    GetSelected when Accept comes.
//   title:           StartMenu vt 0x2304300 (empty HandleInput): a tap anywhere = its own "press +" vt+8 0xbfd40 for player 0 (what a controller's +
//                    reaches through 0x13eea0's receiver loop).
//   modal dialog:    UI manager +0x50 (CConsoleUIDialog vt 0x232a6f8, takes the input first, 0x143884): a tap on a choice prompt (+0x118 + 8 i,
//                    the appended Accept prompt +0x140) = the dialog's own choose 0x739060(dialog, i); hold-to-confirm choices stay with the pad.
//   radial menus:    CConsoleUIRadialMenu vt 0x230d6d8 (HandleInput 0x139b60; pushed by + as id 0xd, NPCs push 0xe..0x1d): segment under the
//                    finger (PUI::Radial +0xa8 x 0x118: centre +0x4c, size +0x30, disabled +0xf0) = its own SetSelected 0x19fe7f0 + "selection
//                    changed" 0x139770, then Accept (sub ring or panel). Tabs (what L / R do, 0x13a1d8..): index +0x44 + 0x137510, from the
//                    front end's tab buttons (dm_tl2_ui_tab). The in-game panels (class Panel vt 0x2303d60: PanelEquipment 0x23036f8, Skills,
//                    Quests, Merchant, ...) are hosted by it (+0x2d8, made by CreatePanel 0x13dfa0 from the segment's panel type; input vt+0x40
//                    while state +0x30 = 1) and show their rows in the radial menu's two PUI::Lists (+0x240 left / +0x270 right, setters
//                    0x13d0d0 / 0x13d190): a tap on a row = that panel's own focus move (PanelEquipment: active list +0x76 / +0x77, rebuild
//                    vt+0x58, prompts 0x78030; others: refresh vt+0x48), a tap on the selected row = Accept into the panel's own handler.
//                    dm_tl2_ui_open_panel(type) opens a panel straight away (+ then tab / segment / Accept, all the menu's own code).
//   list scroll:     a tap on a list's scroll arrow = one page; a vertical drag over a scrollable list (dm_tl2_ui_drag from Tl2Touch) = the rows
//                    follow the finger in row steps. Both write +0x10c and let the list's own SetRowHeight 0x1a02150 clamp it and place the thumb;
//                    a selection that left the shown rows is moved to the nearest shown row the owner's own way (panel focus move / SetSelected,
//                    like the stick on Switch), so the game's next EnsureVisible keeps the new scroll.
//   Not covered: custom-drawn grids (if a panel draws cells itself), hold-to-confirm dialog choices.
//
// "A menu is open" (dm_tl2_menu_open, published every npad read; clicks happen only while it holds, never in plain gameplay): the console UI
// manager (static main+0x74cfe70, GOT 0x23a3590) keeps its menu stack at +0x58 (array) with the top index at +0x60 (-1 = empty; pop
// 0x111e10). Its HandleInput 0x143800 gives the event to +0x80 (the gameplay HUD receiver) only while the stack is empty (0x1438f8), and
// first to the modal element +0x50 while that is drawn (+0x74). So: open = stack not empty or the modal drawn, and no loading screen (+0x90).
//   DM_TL2_POINTER=1   dm_tl2_ui_tap(x, y) (fractions of the game picture, any thread) queues a click; the npad read (the game's input
//                      update 0x13fd40 -> 0x13ea60, main thread, right before the game dispatches its own input events) performs it.
//   DM_TL2_UIDUMP=1    the file dm/ui-dump-now makes dm/tl2-ui-dump.tsv (visible PUI elements + the menu stack) and is deleted; =2 hidden
//                      elements too. A file dm/ui-tap-now holding "x y" queues a click (test without the front end; checked every 30 polls),
//                      dm/ui-panel-now "type" opens that panel (dm_tl2_ui_open_panel), dm/ui-tab-now "+1" / "-1" switches the radial tab,
//                      dm/ui-scroll-now "x y rows" scrolls the list under x y (picture fractions) by rows (+ = later rows).
#ifdef DM_GAME_TL2
#include "hle.h"
#include "dm.h"
#include "dm_input.h"
#include <errno.h>
#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <unistd.h>

enum {
    P_MGR = 0x927a970, P_GOT_MGR = 0x23a3678,
    P_VT_ELEM = 0x23651f8, P_VT_LIST = 0x23656d8, P_VT_BUTTON = 0x2365838, P_VT_RADIAL = 0x2365628,
    P_LIST_DRAW = 0x1a01ab0, P_ELEM_DRAW = 0x19f5de0, P_BUTTON_DTOR = 0x1a046c0, P_RADIAL_DRAW = 0x19fce90,
    U_MGR = 0x74cfe70, U_GOT_MGR = 0x23a3590, U_STACK = 0x58, U_TOP = 0x60, U_MODAL = 0x50, U_LOADING = 0x90, U_HUD = 0x80,
    M_W = 0x2f8, M_H = 0x2fc, M_ROOT = 0x308,
    E_PARENT = 0x38, E_FIRST = 0x40, E_NEXT = 0x58, E_RECT = 0x60, E_DRAWN = 0x74, E_LAYER = 0x80,
    L_AREA = 0x84, L_ROWH = 0x100, L_COUNT = 0x104, L_SEL = 0x108, L_SCROLL = 0x10c, L_SHOWSEL = 0x116, L_MAXSCROLL = 0x110, L_ARROWS = 0x118,
    F_LIST_ROWH = 0x1a02150,
    B_ENABLED = 0xc3, B_HILITE = 0xc4,
    F_SETSEL = 0x1a02460, F_SETHI = 0x1a05020, F_DISPATCH = 0x143800, VT_UIMGR = 0x230d7f0,
    VT_MAIN = 0x230ddb8, VT_NETWORK = 0x2309e28, VT_CREATE = 0x232b008, VT_OPTIONS = 0x23043d8, VT_INVITE = 0x232a938, VT_PARTY = 0x232b2e8,
    EV_ACCEPT = 1, EV_LEFT = 4, EV_RIGHT = 5, EV_PLUS = 0xe,
    MAX_EL = 4096, MAX_DEPTH = 48,
    // radial menus + the in-game panels they host (docs/tl2/TL2_PORT.md "Радиальные меню и панели")
    VT_RADIAL = 0x230d6d8, VT_START = 0x2304300, VT_DIALOG = 0x232a6f8, VT_EQUIP = 0x23036f8,
    F_RADIAL_INPUT = 0x139b60, F_START_PRESS = 0xbfd40, F_DLG_DTOR = 0x737400, F_EQ_INPUT = 0x802d0, F_EQ_REBUILD = 0x81530,
    F_RA_SETSEL = 0x19fe7f0, F_RM_CHANGED = 0x139770, F_RM_TAB = 0x137510, F_RM_CLOSEPANEL = 0x13b850, F_SOUND = 0x113f00,
    F_DLG_CHOOSE = 0x739060, F_PANEL_LSEL = 0x13d0d0, F_PANEL_RSEL = 0x13d190, F_EQ_PROMPTS = 0x78030,
    R_TYPE = 0x2c, R_STATE = 0x30, R_TABS = 0x38, R_NTABS = 0x40, R_TAB = 0x44, R_NODE = 0x48, R_PUI = 0x78, R_TABSOK = 0x22e,
    R_LISTS = 0x238, R_LLIST = 0x240, R_RLIST = 0x270, R_PANEL = 0x2d8,
    RA_SEG = 0xa8, RA_SEGEND = 0xb0, RA_SEL = 0xc0, SEG_STRIDE = 0x118, SEG_SIZE = 0x30, SEG_POS = 0x4c, SEG_OFF = 0xf0,
    N_STRIDE = 0x40, N_TYPE = 4, N_VALID = 0x18, N_SUB = 0x20, TAB_STRIDE = 0x18, TAB_NODE = 0x10,
    D_MODE = 0x178, D_ELEM = 0x118, D_ELEM_A = 0x140, D_SINGLE = 0x138, D_ACT = 0x240, D_N = 0x250,
    PE_HAS_RIGHT = 0x74, PE_LEFT_ON = 0x76, PE_RIGHT_ON = 0x77,
    SND_FOCUS = 0x50, SND_TAB = 0x5b, SND_VIEW_ON = 0x53, SND_VIEW_OFF = 0x54,
    // radial tab headers: vector +0x108..+0x110 of 0x18-byte entries, the label element at +8 (0x13dce0 writes tab i's name into it)
    R_TABHDR = 0x108, R_TABHDR_END = 0x110, TABHDR_STRIDE = 0x18,
    // the panel lists' owner / pet view (PanelEquipment TOGGLE_PET_VIEW = code 2): left flag +0x22a with header elements +0x248..+0x268
    // (0x13d710), right flag +0x22b with +0x278..+0x298 (0x13d770, plus 0x13d7f0 for +0x288)
    R_VIEW_L = 0x22a, R_VIEW_R = 0x22b, R_VHDR_L = 0x248, R_VHDR_R = 0x278,
    F_VIEW_L = 0x13d710, F_VIEW_R = 0x13d770, F_VIEW_R2 = 0x13d7f0,
};
static const struct { uint32_t off, n, w[14]; } k_sites[] = {
    {0x1a02460, 14, {0xf81e0ff3, 0xa9017bfd, 0x910043fd, 0xaa0003f3, 0x3100043f, 0x5400006a, 0x12800001, 0x14000005, 0xb9410668 /* ldr w8,[x19,#0x104] */,
                     0x51000509, 0x6b01011f, 0x1a89c021, 0xaa1303e0, 0xb9010a61 /* str w1,[x19,#0x108] */}},
    {0x1a01dac, 4, {0xb9408e88 /* ldr w8,[x20,#0x8c] */, 0xf8484289 /* ldur x9,[x20,#0x84] */, 0xf90013e9, 0xb9002be8}},
    {0x1a01e08, 3, {0xb9410688 /* ldr w8,[x20,#0x104] */, 0x7100051f, 0x540003eb}},
    {0x1a01e94, 1, {0x39445a88 /* ldrb w8,[x20,#0x116] */}},
    // List SetRowHeight 0x1a02150 (clamps +0x10c to 0..+0x110, places the thumb) and its scroll arrows +0x118..+0x130 in Draw
    {0x1a02150, 4, {0xb9410408 /* ldr w8,[x0,#0x104] */, 0x1b017d08, 0xbd409000 /* ldr s0,[x0,#0x90] */, 0x1e220101}},
    {0x1a02188, 3, {0xb9010001 /* str w1,[x0,#0x100] */, 0xbd011004 /* str s4,[x0,#0x110] */, 0xbd010c03 /* str s3,[x0,#0x10c] */}},
    {0x1a01b84, 6, {0xf9408e88 /* ldr x8,[x20,#0x118] */, 0xb4001128, 0xf9409289 /* ldr x9,[x20,#0x120] */, 0xb40010e9, 0xf9409689, 0xb40010a9}},
    {0x1a01c60, 1, {0xf9408e88 /* ldr x8,[x20,#0x118] (up shown) */}}, {0x1a01d10, 1, {0xf9409288 /* ldr x8,[x20,#0x120] (down shown) */}},
    {0x1a05020, 6, {0xf9404808, 0x1e2e1000, 0x1e2703e1, 0x12000029, 0xb900c81f, 0x39031009 /* strb w9,[x0,#0xc4] */}},
    {0x19f57c0, 3, {0x3941d008 /* ldrb w8,[x0,#0x74] */, 0x34000508, 0xb9408008 /* ldr w8,[x0,#0x80] */}},
    {0x19f5ac0, 4, {0x1e252842, 0x2d431c05, 0xbd006002 /* str s2,[x0,#0x60] */, 0xbd400022}},
    {0x19ff3d8, 2, {0xb902fab4 /* str w20,[x21,#0x2f8] */, 0xb902feb3 /* str w19,[x21,#0x2fc] */}},
    {0x19ff450, 1, {0xf90186b6 /* str x22,[x21,#0x308] */}},
    {0x143884, 3, {0xf9402a60 /* ldr x0,[x19,#0x50] */, 0x3941d008 /* ldrb w8,[x0,#0x74] */, 0x34000108}},
    {0x1438f8, 4, {0xf9404260 /* ldr x0,[x19,#0x80] */, 0xb4000140, 0xb9406268 /* ldr w8,[x19,#0x60] */, 0x3100051f /* cmn w8,#1 */}},
    {0x143930, 2, {0xf9402e68 /* ldr x8,[x19,#0x58] */, 0xf8767900 /* ldr x0,[x8,x22,lsl #3] */}},
    {0x143800, 8, {0xf81c0ff7, 0xa90157f6, 0xa9024ff4, 0xa9037bfd, 0x9100c3fd, 0xaa0203f4, 0x2a0103f5, 0xaa0003f3}},   // UI manager HandleInput
    {0x1139f0, 4, {0x29402428, 0x7100053f, 0x1a9f17e9, 0x7100011f}},   // IsAccept: code 1, type 0
    {0x145830, 1, {0xb9805c18 /* ldrsw x24,[x0,#0x5c] */}}, {0x145890, 1, {0xf9404e68 /* ldr x8,[x19,#0x98] */}},   // MainMenu focus / buttons
    {0x739690, 8, {0xb9403808 /* ldr w8,[x0,#0x38] */, 0x34000268, 0xa9be4ff4, 0xa9017bfd, 0x910043fd, 0xb9803c08 /* ldrsw x8,[x0,#0x3c] */, 0x2a0103f4, 0xaa0003f3}},
    {0x114860, 8, {0xb9405808 /* ldr w8,[x0,#0x58] */, 0x34000268, 0xa9be4ff4, 0xa9017bfd, 0x910043fd, 0xb9805c08 /* ldrsw x8,[x0,#0x5c] */, 0x2a0103f4, 0xaa0003f3}},
    // CConsoleUIRadialMenu HandleInput 0x139b60: state +0x30 (1 = its panel +0x2d8 has the input), PUI::Radial +0x78, node +0x48
    {0x139b78, 1, {0xb9403008 /* ldr w8,[x0,#0x30] */}}, {0x139b94, 1, {0xf9403e80 /* ldr x0,[x20,#0x78] */}}, {0x139c04, 1, {0xf9416e80 /* ldr x0,[x20,#0x2d8] */}},
    // its tab switch (L / R: 0x13a1d8 checks +0x22e, 0x13a1f8 count +0x40, 0x13a214 index +0x44) and the tab apply 0x137510 (tabs +0x38, node +0x48)
    {0x13a1d8, 1, {0x3948ba88 /* ldrb w8,[x20,#0x22e] */}}, {0x13a1f8, 1, {0xb9404288 /* ldr w8,[x20,#0x40] */}}, {0x13a214, 1, {0xb9004688 /* str w8,[x20,#0x44] */}},
    {0x137510, 4, {0xa9be4ff4, 0xa9017bfd, 0x910043fd, 0xf9403c08 /* ldr x8,[x0,#0x78] */}},
    {0x137554, 1, {0xf9401e68 /* ldr x8,[x19,#0x38] */}}, {0x137558, 1, {0xb9804669 /* ldrsw x9,[x19,#0x44] */}}, {0x13756c, 1, {0xf9002668 /* str x8,[x19,#0x48] */}},
    {0x139770, 6, {0xd10143ff, 0xa902cfff, 0xa9047bfd, 0x910103fd, 0xa901ffff, 0xaa0003f3}},   // RadialMenu selection changed
    {0x13dfb4, 1, {0xf9416c00 /* ldr x0,[x0,#0x2d8] */}},   // CreatePanel 0x13dfa0 (panel type = node entry +4)
    {0x13b850, 4, {0xf81e0ff3, 0xa9017bfd, 0x910043fd, 0xaa0003f3}},   // close the hosted panel (Back from a panel)
    // panel lists hosted by the radial menu: 0x13d0d0 left +0x240 / 0x13d190 right +0x270, both behind +0x238
    {0x13d0e8, 1, {0xf9411e88 /* ldr x8,[x20,#0x238] */}}, {0x13d0f0, 1, {0xf9412280 /* ldr x0,[x20,#0x240] */}}, {0x13d1b0, 1, {0xf9413a80 /* ldr x0,[x20,#0x270] */}},
    // PUI::Radial: SetSelected 0x19fe7f0 (segments +0xa8..+0xb0 x 0x118, selected +0xc0, disabled +0xf0), Draw 0x19fce90 (size +0x30, centre +0x4c)
    {0x19fe824, 1, {0xb940c008 /* ldr w8,[x0,#0xc0] */}}, {0x19fe86c, 1, {0x3943c108 /* ldrb w8,[x8,#0xf0] */}}, {0x19fe878, 1, {0xb900c009 /* str w9,[x0,#0xc0] */}},
    {0x19fcf30, 1, {0xa94aa275 /* ldp x21,x8,[x19,#0xa8] */}}, {0x19fcfac, 1, {0xfd401b62 /* ldr d2,[x27,#0x30] */}}, {0x19fcfb4, 1, {0xfc44c364 /* ldur d4,[x27,#0x4c] */}},
    // PanelEquipment HandleInput 0x802d0: +0x77 right list active, +0x74 has a right list (+0x76 left active); prompts 0x78030
    {0x80368, 1, {0x3941dea8 /* ldrb w8,[x21,#0x77] */}}, {0x80370, 1, {0x3941d2a8 /* ldrb w8,[x21,#0x74] */}},
    {0x78030, 4, {0xd10183ff, 0xf90013f7, 0xa90357f6, 0xa9044ff4}},
    // CConsoleUIDialog (UI manager modal +0x50) HandleInput 0x738e90: mode +0x178, actions +0x240, count +0x250; choose 0x739060 (callback +0x270)
    {0x738ea8, 1, {0xb9417808 /* ldr w8,[x0,#0x178] */}}, {0x738ec0, 1, {0xb9425268 /* ldr w8,[x19,#0x250] */}}, {0x738edc, 1, {0xb9424101 /* ldr w1,[x8,#0x240] */}},
    {0x7390d8, 1, {0xf9413a88 /* ldr x8,[x20,#0x270] */}},
    {0xbfd78, 1, {0x39016288 /* strb w8,[x20,#0x58] */}},   // StartMenu "press +" 0xbfd40
    {0x113f00, 4, {0xd10183ff, 0xf90013f7, 0xa90357f6, 0xa9044ff4}},   // UI manager PlaySound(mgr, id)
    // tab header labels 0x13dce0: ldp [x0,#0x108] (vector), tab index +0x44 / tabs +0x38
    {0x13dd00, 1, {0xa950a408 /* ldp x8,x9,[x0,#0x108] */}}, {0x13dd38, 1, {0xb9804668 /* ldrsw x8,[x19,#0x44] */}}, {0x13dd40, 1, {0xf9401e68 /* ldr x8,[x19,#0x38] */}},
    // owner / pet view toggles and PanelEquipment's calls to them
    {0x13d71c, 1, {0x3908a808 /* strb w8,[x0,#0x22a] */}}, {0x13d720, 1, {0xf9412408 /* ldr x8,[x0,#0x248] */}}, {0x13d754, 1, {0xf9413408 /* ldr x8,[x0,#0x268] */}},
    {0x13d77c, 1, {0x3908ac08 /* strb w8,[x0,#0x22b] */}}, {0x13d780, 1, {0xf9413c08 /* ldr x8,[x0,#0x278] */}}, {0x13d7b4, 1, {0xf9414c08 /* ldr x8,[x0,#0x298] */}},
    {0x13d7f8, 1, {0xf9414408 /* ldr x8,[x0,#0x288] */}},
    {0x80818, 1, {0x9402f3d6 /* bl 0x13d770 */}}, {0x808dc, 1, {0x9402f38d /* bl 0x13d710 */}},
};

static uint8_t *g_base; static size_t g_size; static int g_ok = -1, g_mode, g_dump;
static char g_dir[1100];

// ---- safe guest reads (a stale pointer reads as absent, never faults)
int dm_guest_heap_readable(uint64_t a, size_t n);
static int rd(uint64_t a, void *out, size_t n) {
    if (a < 0x10000 || a >= (1ull << 48) || n > 4096) return 0;
    if (dm_guest_heap_readable(a, n)) { memcpy(out, (const void *)(uintptr_t)a, n); return 1; }
    uint64_t b = (uint64_t)(uintptr_t)g_base;
    if (a >= b && n <= g_size && a - b <= g_size - n) { memcpy(out, (const void *)(uintptr_t)a, n); return 1; }
    struct iovec l = {out, n}, r = {(void *)(uintptr_t)a, n};
    return syscall(SYS_process_vm_readv, getpid(), &l, 1UL, &r, 1UL, 0UL) == (ssize_t)n;
}
static uint64_t rq(uint64_t a) { uint64_t v = 0; return rd(a, &v, 8) ? v : 0; }
static uint64_t img(uint32_t off) { return (uint64_t)(uintptr_t)g_base + off; }

static int setup(void) {
    if (g_ok >= 0) return g_ok;
    g_ok = 0;
    const char *e = getenv("DM_TL2_POINTER"); g_mode = e && *e ? atoi(e) : 0;
    e = getenv("DM_TL2_UIDUMP"); g_dump = e && *e ? atoi(e) : 0;
    if (g_mode <= 0 && g_dump <= 0) return 0;
    for (int i = 0; i < g_nmod; i++) if (!strcmp(g_mod[i].name, "main")) { g_base = g_mod[i].base; g_size = g_mod[i].size; }
    if (!g_base || g_size < P_MGR + 0x340) { LOG("【红】[tl2ui] no main module: pointer off"); return 0; }
    for (size_t i = 0; i < sizeof k_sites / sizeof *k_sites; i++)
        if (memcmp(g_base + k_sites[i].off, k_sites[i].w, 4 * k_sites[i].n)) {
            LOG("【红】[tl2ui] main+%#x differs: not TL2 v5 main (build a0c6c2d2), pointer / dump off", k_sites[i].off); return 0; }
    if (rq(img(P_GOT_MGR)) != img(P_MGR) || rq(img(U_GOT_MGR)) != img(U_MGR) || rq(img(P_VT_LIST + 0x10)) != img(P_LIST_DRAW) || rq(img(P_VT_ELEM + 0x10)) != img(P_ELEM_DRAW) ||
        rq(img(P_VT_BUTTON + 8)) != img(P_BUTTON_DTOR) || rq(img(P_VT_RADIAL + 0x10)) != img(P_RADIAL_DRAW) || rq(img(VT_UIMGR)) != img(F_DISPATCH) ||
        rq(img(VT_MAIN)) != img(0x1457e0) || rq(img(VT_NETWORK)) != img(0x112ab0) || rq(img(VT_CREATE)) != img(0x75fde0) ||
        rq(img(VT_OPTIONS)) != img(0xc05f0) || rq(img(VT_INVITE)) != img(0x73bb10) || rq(img(VT_PARTY)) != img(0x766fa0)) {
        LOG("【红】[tl2ui] PUI vtables / GOT do not match: pointer / dump off"); return 0; }
    if (rq(img(VT_RADIAL)) != img(F_RADIAL_INPUT) || rq(img(VT_START + 8)) != img(F_START_PRESS) || rq(img(VT_DIALOG)) != img(F_DLG_DTOR) ||
        rq(img(VT_EQUIP + 0x40)) != img(F_EQ_INPUT) || rq(img(VT_EQUIP + 0x58)) != img(F_EQ_REBUILD)) {
        LOG("【红】[tl2ui] radial / start / dialog / panel vtables do not match: pointer / dump off"); return 0; }
    e = getenv("DM_RUNTIME_DIR"); snprintf(g_dir, sizeof g_dir, "%s", e && *e ? e : "dm");
    LOG("[tl2ui] on: pointer %d, dump %d (PUI manager main+%#x), game input thread tid %d", g_mode, g_dump, P_MGR, (int)gettid());
    return g_ok = 1;
}

// ---- snapshot of the PUI tree (visible part, or all with dump 2)
enum { K_ELEM, K_LIST, K_BUTTON, K_RADIAL, K_OTHER };
static const char *const k_kind[] = {"Element", "List", "Button", "Radial", "other"};
typedef struct {
    uint64_t a, vt; int parent, depth, kind, layer, vis;
    float r[4];
    float area[4]; int rowh, rows, sel, showsel; float scroll, maxs;   // List
    int hilite, enabled;                                            // Button
} el;
static el g_el[MAX_EL];
static int kind_of(uint64_t vt) {
    if (vt == img(P_VT_LIST)) return K_LIST;
    if (vt == img(P_VT_BUTTON)) return K_BUTTON;
    if (vt == img(P_VT_RADIAL)) return K_RADIAL;
    if (vt == img(P_VT_ELEM)) return K_ELEM;
    return K_OTHER;
}
static int snapshot(int all, float *cw, float *ch) {
    uint64_t mgr = img(P_MGR), root = rq(mgr + M_ROOT);
    int32_t wh[2] = {0, 0}; rd(mgr + M_W, wh, 8);
    *cw = wh[0] > 0 ? (float)wh[0] : 1920.f; *ch = wh[1] > 0 ? (float)wh[1] : 1080.f;
    if (!root) return 0;
    static struct { uint64_t a; int parent, depth, vis; } st[MAX_EL]; static uint64_t kids[512];   // the input thread only; off the guest stack
    int sp = 0, n = 0;
    st[sp].a = root; st[sp].parent = -1; st[sp].depth = 0; st[sp].vis = 1; sp++;
    while (sp > 0 && n < MAX_EL) {
        sp--; uint64_t a = st[sp].a; int parent = st[sp].parent, depth = st[sp].depth, pvis = st[sp].vis;
        uint8_t raw[0x120] = {0};
        if (!rd(a, raw, 0x88)) continue;   // the base element; a list's fields are read below
        el *e = &g_el[n]; memset(e, 0, sizeof *e);
        e->a = a; memcpy(&e->vt, raw, 8); e->parent = parent; e->depth = depth; e->kind = kind_of(e->vt);
        memcpy(e->r, raw + E_RECT, 16); memcpy(&e->layer, raw + E_LAYER, 4);
        e->vis = pvis && raw[E_DRAWN];
        if (!e->vis && !all) continue;
        if (e->kind == K_LIST) {
            if (!rd(a + 0x88, raw + 0x88, 0x120 - 0x88)) continue;
            memcpy(e->area, raw + L_AREA, 16); memcpy(&e->rowh, raw + L_ROWH, 4); memcpy(&e->rows, raw + L_COUNT, 4);
            memcpy(&e->sel, raw + L_SEL, 4); memcpy(&e->scroll, raw + L_SCROLL, 4); memcpy(&e->maxs, raw + L_MAXSCROLL, 4); e->showsel = raw[L_SHOWSEL];
        } else if (e->kind == K_BUTTON) {
            uint8_t hb[2] = {0, 0}; if (rd(a + B_ENABLED, hb, 2)) { e->enabled = hb[0]; e->hilite = hb[1]; }
        }
        int me = n++;
        if (depth + 1 >= MAX_DEPTH) continue;
        // children first to last; pushed in reverse so the DFS visits them in drawing order (later = on top)
        int nk = 0;
        for (uint64_t c = rq(a + E_FIRST); c && nk < 512; c = rq(c + E_NEXT)) { kids[nk++] = c; if (nk > 1 && c == kids[0]) break; }
        for (int k = nk - 1; k >= 0 && sp < (int)(sizeof st / sizeof *st); k--) { st[sp].a = kids[k]; st[sp].parent = me; st[sp].depth = depth + 1; st[sp].vis = e->vis; sp++; }
    }
    return n;
}
static int above(const el *x, int xi, const el *y, int yi) { int lx = x->layer == 1, ly = y->layer == 1; return lx != ly ? lx > ly : xi > yi; }
static int in_rect(const float *r, float x, float y) { return x >= r[0] && x < r[2] && y >= r[1] && y < r[3]; }
static int list_row_at(const el *e, float x, float y) {
    if (e->rowh <= 0 || e->rows <= 0 || !in_rect(e->area, x, y)) return -1;
    int i = (int)floorf((y - e->area[1] + e->scroll) / (float)e->rowh);
    return i >= 0 && i < e->rows ? i : -1;
}
// ---- dump
static void dump_stack(FILE *f);
static void dump(void) {
    float cw, ch; int n = snapshot(g_dump >= 2, &cw, &ch);
    char p[1200]; snprintf(p, sizeof p, "%s/tl2-ui-dump.tsv", g_dir);
    FILE *f = fopen(p, "w"); if (!f) { LOG("[tl2ui] dump: cannot write %s (errno %d)", p, errno); return; }
    fprintf(f, "# canvas %.0fx%.0f; idx\tdepth\tparent\taddr\tvt\tclass\tvis\tlayer\tl\tt\tr\tb\textra\n", cw, ch);
    for (int i = 0; i < n; i++) {
        const el *e = &g_el[i];
        fprintf(f, "%d\t%d\t%d\t%#llx\tmain+%#llx\t%s\t%d\t%d\t%.1f\t%.1f\t%.1f\t%.1f\t", i, e->depth, e->parent, (unsigned long long)e->a,
                (unsigned long long)(e->vt - (uint64_t)(uintptr_t)g_base), k_kind[e->kind], e->vis, e->layer, e->r[0], e->r[1], e->r[2], e->r[3]);
        if (e->kind == K_LIST) {
            fprintf(f, "area %.1f,%.1f,%.1f,%.1f rowh %d rows %d sel %d scroll %.1f max %.1f showsel %d", e->area[0], e->area[1], e->area[2], e->area[3],
                    e->rowh, e->rows, e->sel, e->scroll, e->maxs, e->showsel);
            for (int k = 0; k < 4; k++) {   // scroll arrows +0x118 up, +0x120 down, +0x128 up, +0x130 down
                uint64_t a = 0; float r[4] = {0}; uint8_t d = 0;
                if (rd(e->a + L_ARROWS + 8u * k, &a, 8) && a && rd(a + E_RECT, r, 16) && rd(a + E_DRAWN, &d, 1))
                    fprintf(f, " arrow+%#x %s %.1f,%.1f,%.1f,%.1f drawn %d", L_ARROWS + 8 * k, k & 1 ? "down" : "up", r[0], r[1], r[2], r[3], d);
            }
        }
        if (e->kind == K_BUTTON) fprintf(f, "hilite %d enabled %d", e->hilite, e->enabled);
        fputc('\n', f);
    }
    dump_stack(f);
    fclose(f);
    LOG("[tl2ui] dump: %d elements -> %s", n, p);
}

// ---- "a menu is open", published by the input thread every npad read
static _Atomic int g_menu_open;
static int menu_open_now(int *top_out) {
    uint64_t m = img(U_MGR); int32_t top = -1; rd(m + U_TOP, &top, 4); *top_out = top;
    uint64_t load = rq(m + U_LOADING); uint8_t v = 0;
    if (load && rd(load + E_DRAWN, &v, 1) && v) return 0;
    if (top >= 0 && top < 64 && rq(m + U_STACK) && rq(rq(m + U_STACK) + 8ull * top)) return 1;
    uint64_t modal = rq(m + U_MODAL); v = 0;
    return modal && rd(modal + E_DRAWN, &v, 1) && v;
}
int dm_tl2_menu_open(void);
int dm_tl2_menu_open(void) { return g_ok > 0 && g_mode > 0 && atomic_load_explicit(&g_menu_open, memory_order_acquire); }

// ---- tap queue (any thread -> the game's input thread)
static _Atomic uint64_t g_tap_xy; static _Atomic int g_tap_pending;
int dm_tl2_ui_tap(float x, float y);
int dm_tl2_ui_tap(float x, float y) {
    static int mode = -1; if (mode < 0) { const char *e = getenv("DM_TL2_POINTER"); mode = e && *e ? atoi(e) : 0; }   // before the first poll too
    { int dm_tl2_map_tap(float, float); if (!atomic_load_explicit(&g_menu_open, memory_order_acquire) && dm_tl2_map_tap(x, y)) return 1; }   // hle_tl2_map.c: minimap <-> big map
    if (g_ok == 0 || mode <= 0 || !(x >= 0 && x <= 1 && y >= 0 && y <= 1)) return 0;
    if (g_ok > 0 && !atomic_load_explicit(&g_menu_open, memory_order_acquire)) return 0;   // plain gameplay: the caller keeps the touch
    uint32_t xb, yb; memcpy(&xb, &x, 4); memcpy(&yb, &y, 4);
    atomic_store(&g_tap_xy, (uint64_t)yb << 32 | xb); atomic_store_explicit(&g_tap_pending, 1, memory_order_release);
    return 1;
}

// ---- the click, on the game's input thread
static uint64_t top_menu(int *top_out) {
    uint64_t m = img(U_MGR); int32_t top = -1; rd(m + U_TOP, &top, 4); *top_out = top;
    return top >= 0 && top < 64 ? rq(rq(m + U_STACK) + 8ull * top) : 0;
}
static const char *menu_name(uint64_t vt) {
    static const struct { uint32_t vt; const char *n; } k[] = {
        {VT_MAIN, "MainMenu"}, {VT_NETWORK, "Network"}, {VT_CREATE, "CreateCharacter"}, {VT_OPTIONS, "Options"}, {VT_INVITE, "PlayerInvite"},
        {VT_PARTY, "PartyMenu"}, {0x232a788, "ButtonMenu"}, {0x232ac88, "MPCharacterSelect"}, {0x232ad60, "ChangeCharacter"}, {0x232b130, "ManageCharacters"},
        {0x232ae38, "CharacterList"}, {0x232b208, "Waypoints"}, {0x232b4e8, "SkillsRespec"}, {0x232af10, "Controls"}, {0x230d6d8, "RadialMenu"},
        {0x2304300, "StartMenu"}, {0x232a860, "Credits"}, {0x2309f50, "MoviePlayer"}, {0x232aa10, "Transition"}};
    for (size_t i = 0; i < sizeof k / sizeof *k; i++) if (vt == img(k[i].vt)) return k[i].n;
    return "?";
}
static void dispatch(uint32_t code, const char *what) {
    uint32_t ev[4] = {0 /* press */, code, 0, 0};
    int r = ((int (*)(uint64_t, int, const uint32_t *))img(F_DISPATCH))(img(U_MGR), 0, ev);
    LOG("[tl2ui]   %s: UI manager HandleInput(code %u) -> %s", what, code, (r & 1) ? "handled" : "not handled");
}
static void set_hi(uint64_t button, int on) { ((void (*)(uint64_t, int))img(F_SETHI))(button, on); }
// A button of the top menu's focus list: its index (-1 = not this menu's), the array and the focus field
static int menu_button(uint64_t menu, uint64_t vt, uint64_t button, uint64_t *arr_out, uint64_t *focus_at) {
    uint64_t arr = 0; int32_t n = 0;
    if (vt == img(VT_MAIN) || vt == img(VT_NETWORK) || vt == img(VT_CREATE)) { arr = rq(menu + 0x98); rd(menu + 0x58, &n, 4); *focus_at = menu + 0x5c; }
    else if (vt == img(VT_OPTIONS) || vt == img(VT_INVITE) || vt == img(VT_PARTY)) { arr = rq(menu + 0x50); rd(menu + 0x38, &n, 4); *focus_at = menu + 0x3c; }
    else return -1;
    if (!arr || n <= 0 || n > 256) return -1;
    for (int i = 0; i < n; i++) if (rq(arr + 8ull * i) == button) { *arr_out = arr; return i; }
    return -1;
}
static int menu_holds(uint64_t menu, uint64_t p) {
    uint64_t q[0x80]; if (!rd(menu, q, sizeof q)) return 0;
    for (int i = 1; i < 0x80; i++) if (q[i] == p) return 1;
    return 0;
}
// ---- radial menus, their hosted panels, the modal dialog, the title screen
static int32_t r32(uint64_t a) { int32_t v = 0; rd(a, &v, 4); return v; }
static uint8_t r8(uint64_t a) { uint8_t v = 0; rd(a, &v, 1); return v; }
static void sound(int id) { ((void (*)(uint64_t, int))img(F_SOUND))(img(U_MGR), id); }
static void call1(uint32_t f, uint64_t a) { ((void (*)(uint64_t))img(f))(a); }
static int is_text(uint64_t fn) { return fn >= img(0x40000) && fn < img(0x1f00000); }
static int drawn_chain(uint64_t e) {   // the element and every parent drawn (+0x74), i.e. on screen
    for (int d = 0; e && d < MAX_DEPTH; d++, e = rq(e + E_PARENT)) if (!r8(e + E_DRAWN)) return 0;
    return 1;
}
static int elem_hit(uint64_t e, float x, float y) {
    float r[4]; return e && drawn_chain(e) && rd(e + E_RECT, r, 16) && r[2] > r[0] && in_rect(r, x, y);
}
// Segment of the PUI::Radial under (x, y): its own box (centre +0x4c, size +0x30, canvas units, as Draw 0x19fce90 places it); else the
// nearest enabled centre within 1.5 segment sizes (the ring's gaps). -1 none.
static int radial_segment(uint64_t ra, float x, float y, int *nseg) {
    uint64_t s0 = rq(ra + RA_SEG), s1 = rq(ra + RA_SEGEND); int n = s1 > s0 ? (int)((s1 - s0) / SEG_STRIDE) : 0; *nseg = n;
    if (n <= 0 || n > 64) return -1;
    int best = -1; float bd = 1e30f;
    for (int i = 0; i < n; i++) {
        uint64_t s = s0 + (uint64_t)i * SEG_STRIDE; float wh[2], c[2];
        if (!rd(s + SEG_SIZE, wh, 8) || !rd(s + SEG_POS, c, 8) || r8(s + SEG_OFF)) continue;
        float dx = x - c[0], dy = y - c[1], d = dx * dx + dy * dy, lim = 1.5f * fmaxf(wh[0], wh[1]);
        if (fabsf(dx) <= wh[0] * .5f && fabsf(dy) <= wh[1] * .5f) return i;
        if (d < bd && d <= lim * lim) { bd = d; best = i; }
    }
    return best;
}
// The radial menu's own selection move (what its Left / Right / stick code does: PUI::Radial SetSelected, then RadialMenu "selection
// changed" 0x139770 = details + panel preview, focus sound)
static void radial_select(uint64_t menu, int seg) {
    uint64_t ra = rq(menu + R_PUI); int cur = r32(ra + RA_SEL);
    if (cur == seg) return;
    ((void (*)(uint64_t, int))img(F_RA_SETSEL))(ra, seg);
    if (r32(ra + RA_SEL) != cur) { call1(F_RM_CHANGED, menu); sound(SND_FOCUS); }
}
// Tab k of a radial menu, as its own L / R code does it (0x13a1f8..0x13a2b8: index +0x44, then 0x137510, sound 0x5b)
static int radial_tab(uint64_t menu, int k) {
    int n = r32(menu + R_NTABS);
    if (!r8(menu + R_TABSOK) || n < 2 || k < 0 || k >= n) return 0;
    if (r32(menu + R_TAB) == k && rq(menu + R_NODE) == rq(rq(menu + R_TABS) + (uint64_t)k * TAB_STRIDE + TAB_NODE)) return 1;
    *(volatile int32_t *)(uintptr_t)(menu + R_TAB) = k;
    call1(F_RM_TAB, menu); sound(SND_TAB);
    return 1;
}
static int panel_is_equipment(uint64_t panel) { return panel && rq(rq(panel) + 0x40) == img(F_EQ_INPUT); }
// Row r of a list hosted by the radial menu (right = the +0x270 list): the panel's own focus move. PanelEquipment keeps which list is
// active (+0x76 left / +0x77 right, both written as a halfword by its Left / Right code 0x80420 / 0x8058c) and rebuilds after a move (vt+0x58
// 0x81530, prompts 0x78030); the other panels read the lists' selection and refresh through vt+0x48 (what RadialMenu 0x139770 calls).
static void panel_row(uint64_t menu, uint64_t panel, int right, int row, int sel) {
    uint64_t pvt = rq(panel);
    if (panel_is_equipment(panel)) {
        int right_on = !r8(panel + PE_LEFT_ON) && r8(panel + PE_RIGHT_ON) && r8(panel + PE_HAS_RIGHT);
        if (right && !right_on && r8(panel + PE_HAS_RIGHT)) { *(volatile uint16_t *)(uintptr_t)(panel + PE_LEFT_ON) = 0x100; LOG("[tl2ui]   panel: right list made active (its Right code)"); }
        else if (!right && right_on) { *(volatile uint16_t *)(uintptr_t)(panel + PE_LEFT_ON) = 1; LOG("[tl2ui]   panel: left list made active (its Left code)"); }
    }
    if (row != sel) ((int (*)(uint64_t, int))img(right ? F_PANEL_RSEL : F_PANEL_LSEL))(menu, row);
    if (panel_is_equipment(panel)) { call1(F_EQ_REBUILD, panel); call1(F_EQ_PROMPTS, panel); }
    else { uint64_t f = rq(pvt + 0x48); if (is_text(f)) ((void (*)(uint64_t))(uintptr_t)f)(panel); }
    sound(SND_FOCUS);
}
// Tab header under (x, y): entry i of the radial menu's header vector (label element +8, also +0 / +0x10 when they are drawn elements)
static int is_elem(uint64_t e) { uint64_t vt = rq(e); return e && vt >= img(0x1f00000) && vt < img(0x2400000); }
static int tab_header_at(uint64_t menu, float x, float y) {
    uint64_t v0 = rq(menu + R_TABHDR), v1 = rq(menu + R_TABHDR_END); int n = v1 > v0 ? (int)((v1 - v0) / TABHDR_STRIDE) : 0;
    if (n > 16 || n > r32(menu + R_NTABS)) n = r32(menu + R_NTABS) < 16 ? r32(menu + R_NTABS) : 16;
    for (int i = 0; i < n; i++)
        for (int q = 0; q < 3; q++) { uint64_t e = rq(v0 + (uint64_t)i * TABHDR_STRIDE + 8u * q); if (is_elem(e) && elem_hit(e, x, y)) return i; }
    return -1;
}
// Owner / pet view header of a panel list (+0x248..+0x268 left, +0x278..+0x298 right) under (x, y): 0 left, 1 right, -1 none
static int view_header_at(uint64_t menu, float x, float y) {
    if (!rq(menu + R_LISTS)) return -1;
    for (int s = 0; s < 2; s++) for (int q = 0; q < 5; q++) { uint64_t e = rq(menu + (s ? R_VHDR_R : R_VHDR_L) + 8u * q); if (is_elem(e) && elem_hit(e, x, y)) return s; }
    return -1;
}
// PanelEquipment's TOGGLE_PET_VIEW (code 2) for that list, as its own code does it (0x807fc right: 0x13d770 + 0x13d7f0, 0x808cc left:
// 0x13d710; sound 0x53 / 0x54; then the rebuild 0x81530 + prompts 0x78030 its move path ends with)
static void panel_view_toggle(uint64_t menu, uint64_t panel, int right) {
    int on = r8(menu + (right ? R_VIEW_R : R_VIEW_L));
    if (right) { ((void (*)(uint64_t, int))img(F_VIEW_R))(menu, !on); ((void (*)(uint64_t, int))img(F_VIEW_R2))(menu, !on); }
    else ((void (*)(uint64_t, int))img(F_VIEW_L))(menu, !on);
    sound(r8(menu + (right ? R_VIEW_R : R_VIEW_L)) ? SND_VIEW_ON : SND_VIEW_OFF);
    call1(F_EQ_REBUILD, panel); call1(F_EQ_PROMPTS, panel);
}
static int radial_click(uint64_t menu, float ux, float uy, int lbest, int lrow) {
    uint64_t ra = rq(menu + R_PUI), panel = rq(menu + R_PANEL); int state = r32(menu + R_STATE);
    if (!ra || rq(ra) != img(P_VT_RADIAL)) { LOG("[tl2ui]   radial menu %#llx without its PUI::Radial", (unsigned long long)menu); return 1; }
    int th = tab_header_at(menu, ux, uy);
    if (th >= 0) {   // the tab bar: inventory / skill trees / pet / stash / vendor pages are the radial menu's tabs (panels have no tab code)
        LOG("[tl2ui]   radial menu tab header %d (tab %d of %d): its own L / R tab code", th, r32(menu + R_TAB), r32(menu + R_NTABS));
        if (!radial_tab(menu, th)) LOG("[tl2ui]   tabs locked here (+0x22e = 0) or one tab only");
        return 1;
    }
    int vh = lbest < 0 && state == 1 && panel_is_equipment(panel) ? view_header_at(menu, ux, uy) : -1;   // rows win: a header may span them
    if (vh >= 0) {
        LOG("[tl2ui]   %s list owner / pet header: PanelEquipment's own view toggle (was %d)", vh ? "right" : "left", r8(menu + (vh ? R_VIEW_R : R_VIEW_L)));
        panel_view_toggle(menu, panel, vh);
        return 1;
    }
    uint64_t lists = rq(menu + R_LISTS), ll = lists ? rq(menu + R_LLIST) : 0, rl = lists ? rq(menu + R_RLIST) : 0;
    if (lbest >= 0 && (g_el[lbest].a == ll || g_el[lbest].a == rl)) {   // a panel list row
        int right = g_el[lbest].a == rl, sel = g_el[lbest].sel;
        LOG("[tl2ui]   radial menu (type %d, state %d, panel vt main+%#llx): %s list row %d of %d (selected %d)", r32(menu + R_TYPE), state,
            (unsigned long long)(panel ? rq(panel) - img(0) : 0), right ? "right" : "left", lrow, g_el[lbest].rows, sel);
        if (state != 1) {   // the panel is only previewed: enter it as Accept on the ring does (leaf segment -> panel vt+0x38 / +0x20)
            dispatch(EV_ACCEPT, "enter the panel");
            if (r32(menu + R_STATE) != 1) { LOG("[tl2ui]   the panel did not take the input: nothing more"); return 1; }
            panel = rq(menu + R_PANEL);
        }
        int was_right = panel_is_equipment(panel) && !r8(panel + PE_LEFT_ON) && r8(panel + PE_RIGHT_ON);
        if (lrow == sel && (!panel_is_equipment(panel) || was_right == right)) { dispatch(EV_ACCEPT, "Accept on the selected row (the panel's own Accept)"); return 1; }
        panel_row(menu, panel, right, lrow, sel);
        LOG("[tl2ui]   selected (details refreshed); tap it again to activate");
        return 1;
    }
    if (lbest >= 0) return 0;   // a list the radial menu does not host: the generic list path
    int nseg, seg = radial_segment(ra, ux, uy, &nseg);
    if (seg < 0) {
        float r[4] = {0}; rd(ra + E_RECT, r, 16);
        LOG("[tl2ui]   radial menu: no segment under %.0f,%.0f (%d segments, radial rect %.0f,%.0f,%.0f,%.0f)", ux, uy, nseg, r[0], r[1], r[2], r[3]);
        return 0;   // another list / a button: the generic path below
    }
    if (state == 1 && seg == r32(ra + RA_SEL)) {   // the open panel's own segment (a skill node of the skill tree): Accept goes to the panel
        LOG("[tl2ui]   radial menu: segment %d is the open panel's: Accept into the panel (PanelSkills: learn / raise)", seg);
        dispatch(EV_ACCEPT, "Accept into the panel");
        return 1;
    }
    if (state == 1) { call1(F_RM_CLOSEPANEL, menu); LOG("[tl2ui]   panel closed (its Back path 0x13b850)"); }
    LOG("[tl2ui]   radial menu (type %d, tab %d/%d): segment %d of %d (selected %d)", r32(menu + R_TYPE), r32(menu + R_TAB), r32(menu + R_NTABS), seg, nseg, r32(ra + RA_SEL));
    radial_select(menu, seg);
    if (r32(ra + RA_SEL) != seg) { LOG("[tl2ui]   the radial refused the segment (disabled)"); return 1; }
    dispatch(EV_ACCEPT, "Accept on the segment (the radial menu's own: sub ring or panel)");
    return 1;
}
// The UI manager's modal dialog (+0x50, CConsoleUIDialog): its choices are the prompt elements +0x118 + 8 i (one-prompt layout +0x138 = 1: the
// first), the appended Accept prompt +0x140 (built by 0x7374e0); choosing = 0x739060(dialog, i), what its HandleInput does for an Accept
// choice or in mode +0x178 != 0. Hold-to-confirm choices (mode 0, not Accept) are left to the pad on purpose.
static int dialog_click(uint64_t dlg, float ux, float uy) {
    int n = r32(dlg + D_N), mode = r32(dlg + D_MODE), hit = -1;
    if (n <= 0 || n > 8) { LOG("[tl2ui] modal dialog %#llx: %d choices, nothing", (unsigned long long)dlg, n); return 1; }
    for (int i = 0; i < n && hit < 0; i++) {
        int act = r32(dlg + D_ACT + 4ull * i);
        uint64_t e = (act == 1 && i == n - 1 && drawn_chain(rq(dlg + D_ELEM_A))) ? rq(dlg + D_ELEM_A) : i < 4 ? rq(dlg + D_ELEM + 8ull * i) : 0;
        if (elem_hit(e, ux, uy)) hit = i;
    }
    if (hit < 0 && n == 1) hit = 0;   // a one-choice prompt (OK): anywhere
    if (hit < 0) { LOG("[tl2ui] modal dialog %#llx: tap %.0f,%.0f on none of its %d choices", (unsigned long long)dlg, ux, uy, n); return 1; }
    int act = r32(dlg + D_ACT + 4ull * hit);
    if (mode == 0 && act != 1) { LOG("[tl2ui] modal dialog: choice %d (action %d) is hold-to-confirm: left to the pad", hit, act); return 1; }
    LOG("[tl2ui] modal dialog %#llx: choice %d of %d (action %d, mode %d): the dialog's own choose 0x739060", (unsigned long long)dlg, hit, n, act, mode);
    ((void (*)(uint64_t, int))img(F_DLG_CHOOSE))(dlg, hit);
    return 1;
}

// ---- list scroll (scroll arrows, vertical drags)
// Scroll a list by rows (+ = later rows): +0x10c, then the list's own SetRowHeight 0x1a02150 with its row height (clamp 0..max, thumb). A
// selected row that left the shown rows is moved to the nearest shown one the owner's way (hosted panel list: the panel's focus move as for a
// tap; a list of the top menu: its SetSelected), so the game's own EnsureVisible (SetSelected's tail) agrees with the new scroll.
static struct { uint64_t list; float want; int polls; } g_watch;
static int list_scroll(uint64_t menu, uint64_t list, int rows, const char *why) {
    int rowh = r32(list + L_ROWH), n = r32(list + L_COUNT), sel = r32(list + L_SEL);
    float sc = 0, mx = 0, top = 0, bot = 0; rd(list + L_SCROLL, &sc, 4); rd(list + L_MAXSCROLL, &mx, 4); rd(list + L_AREA + 4, &top, 4); rd(list + L_AREA + 12, &bot, 4);
    if (rq(list) != img(P_VT_LIST) || rowh <= 0 || n <= 0 || n > 100000 || !rows) {
        LOG("[tl2ui] scroll (%s) list %#llx %+d rows: nothing (row height %d, rows %d)", why, (unsigned long long)list, rows, rowh, n); return 0; }
    float ns = sc + (float)rows * (float)rowh;
    if (ns > mx) ns = mx;
    if (ns < 0) ns = 0;
    if (fabsf(ns - sc) < .5f) {
        LOG("[tl2ui] scroll (%s) list %#llx %+d rows: already at the %s (scroll %.0f, max %.0f)", why, (unsigned long long)list, rows, rows > 0 ? "end" : "top", sc, mx); return 0; }
    *(volatile float *)(uintptr_t)(list + L_SCROLL) = ns;
    ((void (*)(uint64_t, int))img(F_LIST_ROWH))(list, rowh);   // its own clamp + thumb
    rd(list + L_SCROLL, &ns, 4);
    int first = (int)ceilf(ns / (float)rowh - .01f), last = (int)floorf((ns + bot - top) / (float)rowh + .01f) - 1;
    if (last > n - 1) last = n - 1;
    if (last < first) last = first;
    int to = sel < 0 ? sel : sel < first ? first : sel > last ? last : sel;
    const char *how = "selection kept";
    if (to != sel) {
        uint64_t vt = menu ? rq(menu) : 0, panel = 0;
        int hosted = vt == img(VT_RADIAL) && rq(menu + R_LISTS) && (rq(menu + R_LLIST) == list || rq(menu + R_RLIST) == list);
        if (hosted && r32(menu + R_STATE) == 1 && (panel = rq(menu + R_PANEL))) {
            panel_row(menu, panel, rq(menu + R_RLIST) == list, to, sel); how = "selection moved by the panel's own focus move"; }
        else if (!hosted && menu && menu_holds(menu, list)) { ((void (*)(uint64_t, int))img(F_SETSEL))(list, to); how = "selection moved by the list's SetSelected"; }
        else how = hosted ? "panel only previewed: selection kept" : "list not held by the top menu: selection kept";
    }
    float now = 0; rd(list + L_SCROLL, &now, 4);
    LOG("[tl2ui] scroll (%s) list %#llx %+d rows: scroll %.0f -> %.0f (max %.0f, row %d, %d rows, shown %d..%d), selected %d -> %d (%s)%s", why,
        (unsigned long long)list, rows, sc, now, mx, rowh, n, first, last, sel, r32(list + L_SEL), how, fabsf(now - ns) > .5f ? " [the game moved the scroll again]" : "");
    g_watch.list = list; g_watch.want = now; g_watch.polls = 20;
    return 1;
}
static void watch_step(void) {   // a scroll the game undid afterwards (e.g. a panel re-selecting every frame) shows up in the log
    if (!g_watch.list || --g_watch.polls > 0) return;
    float now = 0; rd(g_watch.list + L_SCROLL, &now, 4);
    if (fabsf(now - g_watch.want) > .5f)
        LOG("[tl2ui] scroll: list %#llx scroll is %.0f 20 polls later (was set to %.0f): reset by the game", (unsigned long long)g_watch.list, now, g_watch.want);
    g_watch.list = 0;
}
// Scroll arrow of a shown list under (ux, uy) (padded: the arrows are small; a row under the point wins): +1 down / -1 up, 0 none; *li = its g_el index
static int arrow_at(int n, float ux, float uy, int *li) {
    const float pad = 36;
    for (int i = n - 1; i >= 0; i--) {
        const el *e = &g_el[i];
        if (e->kind != K_LIST || !e->vis || e->rowh <= 0 || list_row_at(e, ux, uy) >= 0) continue;
        for (int k = 0; k < 4; k++) {
            uint64_t a = rq(e->a + L_ARROWS + 8u * k); float r[4];
            if (!is_elem(a) || !r8(a + E_DRAWN) || !rd(a + E_RECT, r, 16) || !(r[2] > r[0] && r[3] > r[1])) continue;
            if (ux >= r[0] - pad && ux < r[2] + pad && uy >= r[1] - pad && uy < r[3] + pad) { *li = i; return k & 1 ? 1 : -1; }
        }
    }
    return 0;
}
static int list_page(const el *e) { int p = e->rowh > 0 ? (int)((e->area[3] - e->area[1]) / (float)e->rowh) : 1; return p < 1 ? 1 : p; }
// Topmost shown list whose row area holds (ux, uy); scrollable only (max scroll > 0) when want_scroll. -1 none.
static int list_at(int n, float ux, float uy, int want_scroll) {
    int best = -1;
    for (int i = 0; i < n; i++) {
        const el *e = &g_el[i];
        if (e->kind != K_LIST || !e->vis || e->rowh <= 0 || e->rows <= 0 || !in_rect(e->area, ux, uy) || (want_scroll && !(e->maxs > .5f))) continue;
        if (best < 0 || above(e, i, &g_el[best], best)) best = i;
    }
    return best;
}

void dm_tl2_ui_frame_to_canvas(float fx, float fy, float cw, float ch, float *cx, float *cy);   // hle_tl2_icons.c: the one canvas <-> picture mapping
void dm_tl2_ui_canvas_to_frame(float cx, float cy, float cw, float ch, float *fx, float *fy);
static void click(float fx, float fy) {
    int top; uint64_t menu = top_menu(&top);
    float cw, ch; int n = snapshot(0, &cw, &ch);
    float ux, uy; dm_tl2_ui_frame_to_canvas(fx, fy, cw, ch, &ux, &uy); int best = -1, row = -1, radial = -1;
    uint64_t modal = rq(img(U_MGR) + U_MODAL);
    if (modal && r8(modal + E_DRAWN) && rq(modal) == img(VT_DIALOG)) { dialog_click(modal, ux, uy); return; }   // HandleInput 0x143884: the modal first
    if (!menu) { LOG("[tl2ui] tap %.3f,%.3f: no menu on the console UI stack (top %d): nothing", fx, fy, top); return; }
    uint64_t vt = rq(menu);
    if (vt == img(VT_START)) {   // title "press +": its HandleInput is empty; the + of a controller reaches it as vt+8 (0x13eea0 -> 0xbfd40)
        LOG("[tl2ui] tap %.3f,%.3f on the title screen: its own \"press +\" 0xbfd40 for player 0", fx, fy);
        ((int (*)(uint64_t, int))img(F_START_PRESS))(menu, 0);
        return;
    }
    for (int i = 0; i < n; i++) {
        const el *e = &g_el[i];
        if (e->kind == K_LIST) { int r = list_row_at(e, ux, uy); if (r >= 0 && (best < 0 || above(e, i, &g_el[best], best))) { best = i; row = r; } }
        else if (e->kind == K_BUTTON && in_rect(e->r, ux, uy) && e->r[2] > e->r[0] && (best < 0 || above(e, i, &g_el[best], best))) { best = i; row = -1; }
        else if (e->kind == K_RADIAL && in_rect(e->r, ux, uy)) radial = i;
    }
    const char *mn = menu_name(vt);
    unsigned long long vto = (unsigned long long)(vt - img(0));
    { int li = -1, dir = arrow_at(n, ux, uy, &li);   // a list's scroll arrow: one page
      if (dir) {
          LOG("[tl2ui] tap %.3f,%.3f = UI %.0f,%.0f on the %s scroll arrow of list %#llx (menu %s)", fx, fy, ux, uy, dir > 0 ? "down" : "up", (unsigned long long)g_el[li].a, mn);
          list_scroll(menu, g_el[li].a, dir * list_page(&g_el[li]), "arrow");
          return;
      } }
    if (vt == img(VT_RADIAL)) {
        LOG("[tl2ui] tap %.3f,%.3f = UI %.0f,%.0f on the radial menu %#llx", fx, fy, ux, uy, (unsigned long long)menu);
        if (radial_click(menu, ux, uy, best >= 0 && g_el[best].kind == K_LIST ? best : -1, row)) return;
    }
    if (best < 0) {
        LOG("[tl2ui] tap %.3f,%.3f = UI %.0f,%.0f, menu %s %#llx (vt main+%#llx): no button / list row under it%s", fx, fy, ux, uy, mn,
            (unsigned long long)menu, vto, radial >= 0 ? " (a radial menu: not handled)" : "");
        return;
    }
    const el *t = &g_el[best];
    if (t->kind == K_BUTTON) {
        uint64_t arr = 0, focus_at = 0; int idx = menu_button(menu, vt, t->a, &arr, &focus_at);
        if (idx < 0) {
            LOG("[tl2ui] tap %.3f,%.3f = UI %.0f,%.0f: button %#llx is not in the focus list of the top menu %s (vt main+%#llx): nothing", fx, fy, ux, uy,
                (unsigned long long)t->a, mn, vto);
            return;
        }
        int32_t cur = -1; rd(focus_at, &cur, 4);
        LOG("[tl2ui] tap %.3f,%.3f = UI %.0f,%.0f -> %s item %d (focus was %d)%s", fx, fy, ux, uy, mn, idx, cur, t->enabled ? "" : ", greyed: Accept refused by the menu");
        if (cur != idx) {   // the menu's own focus move: what its navigation code does (MainMenu 0x145954..0x1459b8, ButtonMenu 0x739690)
            if (cur >= 0) { uint64_t ob = rq(arr + 8ull * cur); if (ob) set_hi(ob, 0); }
            *(volatile int32_t *)(uintptr_t)focus_at = idx;
            set_hi(t->a, 1);
        }
        float w = t->r[2] - t->r[0], rel = w > 0 ? (ux - t->r[0]) / w : .5f;
        if (rel < .25f) dispatch(EV_LEFT, "left quarter: Left");
        else if (rel > .75f) dispatch(EV_RIGHT, "right quarter: Right");
        else dispatch(EV_ACCEPT, "Accept");
        return;
    }
    if (!menu_holds(menu, t->a)) {
        LOG("[tl2ui] tap %.3f,%.3f = UI %.0f,%.0f: list %#llx row %d is not held by the top menu %s (vt main+%#llx): nothing", fx, fy, ux, uy,
            (unsigned long long)t->a, row, mn, vto);
        return;
    }
    LOG("[tl2ui] tap %.3f,%.3f = UI %.0f,%.0f -> %s list %#llx row %d of %d (selected %d)", fx, fy, ux, uy, mn, (unsigned long long)t->a, row, t->rows, t->sel);
    if (row != t->sel) { ((void (*)(uint64_t, int))img(F_SETSEL))(t->a, row); LOG("[tl2ui]   selected with the list's SetSelected (tap it again to activate)"); }
    else dispatch(EV_ACCEPT, "Accept on the selected row");
}

// A radial node's entries (0x40 bytes: id +0, panel type +4 = CreatePanel 0x13dfa0's switch, valid while +0x18 != 0, sub ring +0x20)
static int node_find(uint64_t node, int type, int *idx, int *sub) {
    uint64_t a = rq(node);
    for (int i = 0; a && i < 48; i++) {
        uint64_t e = a + (uint64_t)i * N_STRIDE;
        if (!rq(e + N_VALID)) break;
        if (r32(e + N_TYPE) == type && !rq(e + N_SUB)) { *idx = i; *sub = -1; return 1; }
        uint64_t s = rq(e + N_SUB), b = s ? rq(s) : 0;
        for (int j = 0; b && j < 48; j++) {
            uint64_t f = b + (uint64_t)j * N_STRIDE;
            if (!rq(f + N_VALID)) break;
            if (r32(f + N_TYPE) == type && !rq(f + N_SUB)) { *idx = i; *sub = j; return 1; }
        }
    }
    return 0;
}
static void dump_stack(FILE *f) {
    int top; top_menu(&top); uint64_t st = rq(img(U_MGR) + U_STACK);
    fprintf(f, "# console UI stack top %d\n", top);
    for (int i = 0; i <= top && i < 64; i++) {
        uint64_t me = rq(st + 8ull * i), vt = rq(me);
        fprintf(f, "# stack %d: %#llx vt main+%#llx %s\n", i, (unsigned long long)me, (unsigned long long)(vt - img(0)), menu_name(vt));
        if (vt != img(VT_RADIAL)) continue;
        uint64_t ra = rq(me + R_PUI), panel = rq(me + R_PANEL), tabs = rq(me + R_TABS);
        fprintf(f, "#   radial type %d state %d tab %d/%d tabs_ok %d node %#llx panel %#llx vt main+%#llx lists %#llx left %#llx right %#llx\n",
                r32(me + R_TYPE), r32(me + R_STATE), r32(me + R_TAB), r32(me + R_NTABS), r8(me + R_TABSOK), (unsigned long long)rq(me + R_NODE),
                (unsigned long long)panel, (unsigned long long)(panel ? rq(panel) - img(0) : 0), (unsigned long long)rq(me + R_LISTS),
                (unsigned long long)rq(me + R_LLIST), (unsigned long long)rq(me + R_RLIST));
        for (int k = 0; k < r32(me + R_NTABS) && k < 16; k++) {
            uint64_t node = rq(tabs + (uint64_t)k * TAB_STRIDE + TAB_NODE), a = rq(node);
            fprintf(f, "#   tab %d kind %d node %#llx types:", k, r32(tabs + (uint64_t)k * TAB_STRIDE), (unsigned long long)node);
            for (int j = 0; a && j < 48 && rq(a + (uint64_t)j * N_STRIDE + N_VALID); j++)
                fprintf(f, " %d%s", r32(a + (uint64_t)j * N_STRIDE + N_TYPE), rq(a + (uint64_t)j * N_STRIDE + N_SUB) ? "+" : "");
            fputc('\n', f);
        }
        uint64_t h0 = rq(me + R_TABHDR), h1 = rq(me + R_TABHDR_END);
        for (int j = 0; h1 > h0 && j < (int)((h1 - h0) / TABHDR_STRIDE) && j < 16; j++)
            for (int q = 0; q < 3; q++) {
                uint64_t e = rq(h0 + (uint64_t)j * TABHDR_STRIDE + 8u * q); float r[4] = {0};
                if (is_elem(e) && rd(e + E_RECT, r, 16)) fprintf(f, "#   tab header %d.%d %#llx rect %.1f,%.1f,%.1f,%.1f drawn %d\n", j, q, (unsigned long long)e, r[0], r[1], r[2], r[3], drawn_chain(e));
            }
        if (rq(me + R_LISTS)) {
            fprintf(f, "#   view left %d right %d\n", r8(me + R_VIEW_L), r8(me + R_VIEW_R));
            for (int s = 0; s < 2; s++) for (int q = 0; q < 5; q++) {
                uint64_t e = rq(me + (s ? R_VHDR_R : R_VHDR_L) + 8u * q); float r[4] = {0};
                if (is_elem(e) && rd(e + E_RECT, r, 16)) fprintf(f, "#   view header %s +%#x %#llx rect %.1f,%.1f,%.1f,%.1f drawn %d\n", s ? "right" : "left",
                                                                (s ? R_VHDR_R : R_VHDR_L) + 8 * q, (unsigned long long)e, r[0], r[1], r[2], r[3], drawn_chain(e));
            }
        }
        uint64_t s0 = rq(ra + RA_SEG), s1 = rq(ra + RA_SEGEND);
        for (int j = 0; s1 > s0 && j < (int)((s1 - s0) / SEG_STRIDE) && j < 64; j++) {
            uint64_t s = s0 + (uint64_t)j * SEG_STRIDE; float wh[2] = {0}, c[2] = {0}; rd(s + SEG_SIZE, wh, 8); rd(s + SEG_POS, c, 8);
            fprintf(f, "#   segment %d centre %.1f,%.1f size %.1fx%.1f off %d%s\n", j, c[0], c[1], wh[0], wh[1], r8(s + SEG_OFF), j == r32(ra + RA_SEL) ? " (selected)" : "");
        }
    }
    uint64_t modal = rq(img(U_MGR) + U_MODAL);
    if (modal && r8(modal + E_DRAWN)) {
        int n = r32(modal + D_N);
        fprintf(f, "# modal %#llx vt main+%#llx mode %d choices %d:", (unsigned long long)modal, (unsigned long long)(rq(modal) - img(0)), r32(modal + D_MODE), n);
        for (int i = 0; i < n && i < 8; i++) fprintf(f, " %d", r32(modal + D_ACT + 4ull * i));
        fputc('\n', f);
    }
}

// ---- requests from the front end (any thread): open a panel of the in-game radial menu, switch the radial menu's tab
static _Atomic int g_req_panel = -1, g_req_tab;   // panel type (CreatePanel 0x13dfa0 numbering), tab delta
int dm_tl2_ui_open_panel(int type);
int dm_tl2_ui_open_panel(int type) {
    static int mode = -1; if (mode < 0) { const char *e = getenv("DM_TL2_POINTER"); mode = e && *e ? atoi(e) : 0; }
    if (g_ok == 0 || mode <= 0 || type < 0 || type > 0x40) return 0;
    atomic_store_explicit(&g_req_panel, type, memory_order_release);
    return 1;
}
int dm_tl2_ui_tab(int delta);
int dm_tl2_ui_tab(int delta) {
    if (g_ok <= 0 || g_mode <= 0 || !delta || !atomic_load_explicit(&g_menu_open, memory_order_acquire)) return 0;
    atomic_store_explicit(&g_req_tab, delta, memory_order_release);
    return 1;
}
static struct { int type, step, sub; uint64_t t0, t1; } g_op = {-1, 0, -1, 0, 0};
static void tab_step(int delta) {
    int top; uint64_t menu = top_menu(&top);
    if (!menu || rq(menu) != img(VT_RADIAL)) { LOG("[tl2ui] tab %+d: the top menu is not a radial menu (%s): nothing", delta, menu ? menu_name(rq(menu)) : "-"); return; }
    int n = r32(menu + R_NTABS), k = ((r32(menu + R_TAB) + delta) % n + n) % n;
    if (n < 2 || !r8(menu + R_TABSOK)) { LOG("[tl2ui] tab %+d: this radial menu has %d tabs (switch %s)", delta, n, r8(menu + R_TABSOK) ? "on" : "off"); return; }
    LOG("[tl2ui] tab %+d: %d -> %d of %d (the radial menu's own L / R code)", delta, r32(menu + R_TAB), k, n);
    radial_tab(menu, k);
}
static void op_end(const char *what) { LOG("[tl2ui] open panel type %d: %s (%.0f ms)", g_op.type, what, (now_ns() - g_op.t0) / 1e6); g_op.type = -1; }
// Open panel type T: the + button's own event to the UI manager when no menu is up (it pushes the game radial menu 0xd, HandleInput
// 0x143a5c), then in that radial menu: the tab holding a segment of type T (its own tab apply), that segment (its own selection) and Accept.
static void op_step(void) {
    if (g_op.type < 0) return;
    if (now_ns() - g_op.t0 > 3000000000ull) { op_end("timeout"); return; }
    int top; uint64_t menu = top_menu(&top);
    uint64_t modal = rq(img(U_MGR) + U_MODAL), load = rq(img(U_MGR) + U_LOADING);
    if (modal && r8(modal + E_DRAWN)) { op_end("a dialog is up: nothing"); return; }
    if (load && r8(load + E_DRAWN)) { op_end("loading screen: nothing"); return; }
    if (!menu) {
        if (g_op.step == 0) { dispatch(EV_PLUS, "open the game radial menu (+)"); g_op.step = 1; }
        return;   // wait for the push (same frame normally)
    }
    if (rq(menu) != img(VT_RADIAL) || r32(menu + R_TYPE) != 0) { op_end(g_op.step ? "the + menu that came up is not the game radial menu" : "another menu is open: nothing"); return; }
    uint64_t ra = rq(menu + R_PUI), tabs = rq(menu + R_TABS);
    if (g_op.step == 1) { g_op.step = 2; g_op.t1 = now_ns(); return; }   // just pushed: let it lay out / open for a moment
    if ((g_op.step == 2 || g_op.step == 3) && now_ns() - g_op.t1 < 150000000ull) return;
    if (g_op.step == 3) {   // inside the sub ring: its entry
        if (r32(menu + R_STATE) == 1) { op_end("opened"); return; }
        LOG("[tl2ui] open panel type %d: sub ring entry %d", g_op.type, g_op.sub);
        radial_select(menu, g_op.sub);
        dispatch(EV_ACCEPT, "Accept on the entry");
        op_end(r32(menu + R_STATE) == 1 ? "opened" : "Accept given, the panel did not open");
        return;
    }
    int n = r32(menu + R_NTABS), k = -1, idx = -1, sub = -1;
    int cur = r32(menu + R_TAB);
    for (int t = 0; t < (n > 0 ? n : 1) && t < 16 && k < 0; t++) {
        int kk = n > 0 ? (cur + t) % n : 0;
        uint64_t node = n > 0 ? rq(tabs + (uint64_t)kk * TAB_STRIDE + TAB_NODE) : rq(menu + R_NODE);
        if (node && node_find(node, g_op.type, &idx, &sub)) k = kk;
    }
    if (k < 0) { op_end("no segment of that type in the game radial menu's tabs"); return; }
    if (r32(menu + R_STATE) == 1) call1(F_RM_CLOSEPANEL, menu);
    if (n > 0 && (k != cur || rq(menu + R_NODE) != rq(tabs + (uint64_t)k * TAB_STRIDE + TAB_NODE))) {
        if (!r8(menu + R_TABSOK)) { op_end("its tab is not the current one and tabs are locked"); return; }
        *(volatile int32_t *)(uintptr_t)(menu + R_TAB) = k; call1(F_RM_TAB, menu); sound(SND_TAB);
    }
    LOG("[tl2ui] open panel type %d: tab %d segment %d%s", g_op.type, k, idx, sub >= 0 ? " (sub ring)" : "");
    radial_select(menu, idx);
    if (r32(ra + RA_SEL) != idx) { op_end("the segment is disabled"); return; }
    dispatch(EV_ACCEPT, "Accept on the segment");
    if (sub >= 0) { g_op.sub = sub; g_op.step = 3; g_op.t1 = now_ns(); return; }
    op_end(r32(menu + R_STATE) == 1 ? "opened" : "Accept given, the panel did not open");
}

// ---- list scroll requests from the front end (any thread) and the shown scrollable lists published for its hit test
// dm_tl2_ui_drag(x0, y0, dy, phase): a vertical drag that went down at x0 y0 (picture fractions), dy = the finger's total y move since then
// (fraction of the picture height, + = down); phase 0 start, 1 move, 2 end. Only the latest state is kept; the input thread follows it.
static _Atomic uint32_t g_drag_gen, g_drag_end, g_drag_dy; static _Atomic uint64_t g_drag_xy;
static _Atomic uint64_t g_scroll_xy; static _Atomic int g_scroll_rows;
static struct { uint32_t gen; uint64_t list; int applied, rowh; float ux, uy, x0, y0; } g_drag;
enum { MAX_PUB = 8 };
static float g_pub[MAX_PUB][4]; static _Atomic int g_npub;   // scrollable list row areas in picture fractions (input thread writes)
static uint64_t pack_xy(float x, float y) { uint32_t xb, yb; memcpy(&xb, &x, 4); memcpy(&yb, &y, 4); return (uint64_t)yb << 32 | xb; }
static void unpack_xy(uint64_t v, float *x, float *y) { uint32_t xb = (uint32_t)v, yb = (uint32_t)(v >> 32); memcpy(x, &xb, 4); memcpy(y, &yb, 4); }
int dm_tl2_ui_list_hit(float x, float y);
int dm_tl2_ui_list_hit(float x, float y) {   // is (x, y) on a shown list that can scroll (the front end then turns a vertical drag into a scroll)
    if (g_ok <= 0 || g_mode <= 0 || !atomic_load_explicit(&g_menu_open, memory_order_acquire)) return 0;
    int n = atomic_load_explicit(&g_npub, memory_order_acquire);
    for (int i = 0; i < n && i < MAX_PUB; i++) if (x >= g_pub[i][0] && x < g_pub[i][2] && y >= g_pub[i][1] && y < g_pub[i][3]) return 1;
    return 0;
}
int dm_tl2_ui_drag(float x0, float y0, float dy, int phase);
int dm_tl2_ui_drag(float x0, float y0, float dy, int phase) {
    if (g_ok <= 0 || g_mode <= 0 || !(x0 >= 0 && x0 <= 1 && y0 >= 0 && y0 <= 1) || !isfinite(dy)) return 0;
    uint32_t db; memcpy(&db, &dy, 4);
    if (phase == 0) { atomic_store(&g_drag_xy, pack_xy(x0, y0)); atomic_store(&g_drag_dy, 0); atomic_fetch_add_explicit(&g_drag_gen, 1, memory_order_release); return 1; }
    atomic_store_explicit(&g_drag_dy, db, memory_order_release);
    if (phase == 2) atomic_store_explicit(&g_drag_end, atomic_load(&g_drag_gen), memory_order_release);
    return 1;
}
int dm_tl2_ui_scroll(float x, float y, int rows);
int dm_tl2_ui_scroll(float x, float y, int rows) {   // scroll the list under (x, y) by rows (+ = later rows); the test file ui-scroll-now
    if (g_ok <= 0 || g_mode <= 0 || !rows || !(x >= 0 && x <= 1 && y >= 0 && y <= 1) || !atomic_load_explicit(&g_menu_open, memory_order_acquire)) return 0;
    atomic_store(&g_scroll_xy, pack_xy(x, y)); atomic_store_explicit(&g_scroll_rows, rows, memory_order_release);
    return 1;
}
static void publish_lists(int open) {
    if (!open) { atomic_store_explicit(&g_npub, 0, memory_order_release); return; }
    float cw, ch; int n = snapshot(0, &cw, &ch), k = 0;
    for (int i = 0; i < n && k < MAX_PUB; i++) {
        const el *e = &g_el[i];
        if (e->kind != K_LIST || e->rowh <= 0 || e->rows <= 0 || !(e->maxs > .5f) || !(e->area[2] > e->area[0])) continue;
        float f[4]; dm_tl2_ui_canvas_to_frame(e->area[0], e->area[1], cw, ch, &f[0], &f[1]); dm_tl2_ui_canvas_to_frame(e->area[2], e->area[3], cw, ch, &f[2], &f[3]);
        memcpy(g_pub[k++], f, sizeof f);
    }
    atomic_store_explicit(&g_npub, k, memory_order_release);
}
static void scroll_step(int open) {
    int rows = atomic_exchange_explicit(&g_scroll_rows, 0, memory_order_acquire);
    if (rows && open) {
        float x, y, cw, ch, ux, uy; unpack_xy(atomic_load(&g_scroll_xy), &x, &y);
        int n = snapshot(0, &cw, &ch); dm_tl2_ui_frame_to_canvas(x, y, cw, ch, &ux, &uy);
        int li = list_at(n, ux, uy, 0), top;
        if (li < 0) LOG("[tl2ui] scroll %.3f,%.3f = UI %.0f,%.0f %+d rows: no list under it", x, y, ux, uy, rows);
        else list_scroll(top_menu(&top), g_el[li].a, rows, "request");
    }
    uint32_t gen = atomic_load_explicit(&g_drag_gen, memory_order_acquire);
    if (gen != g_drag.gen) {   // a new drag: the list under its start point
        g_drag.gen = gen; g_drag.list = 0; g_drag.applied = 0;
        float cw, ch; unpack_xy(atomic_load(&g_drag_xy), &g_drag.x0, &g_drag.y0);
        int n = snapshot(0, &cw, &ch); dm_tl2_ui_frame_to_canvas(g_drag.x0, g_drag.y0, cw, ch, &g_drag.ux, &g_drag.uy);
        int li = open ? list_at(n, g_drag.ux, g_drag.uy, 1) : -1;
        if (li < 0) LOG("[tl2ui] drag at %.3f,%.3f = UI %.0f,%.0f: no scrollable list under it%s", g_drag.x0, g_drag.y0, g_drag.ux, g_drag.uy, open ? "" : " (no menu open)");
        else { g_drag.list = g_el[li].a; g_drag.rowh = g_el[li].rowh;
               LOG("[tl2ui] drag start at UI %.0f,%.0f on list %#llx (%d rows of %d, scroll %.0f of %.0f, selected %d)", g_drag.ux, g_drag.uy, (unsigned long long)g_drag.list,
                   g_el[li].rows, g_el[li].rowh, g_el[li].scroll, g_el[li].maxs, g_el[li].sel); }
    }
    if (!g_drag.list) return;
    int ended = atomic_load_explicit(&g_drag_end, memory_order_acquire) == g_drag.gen;
    if (open && drawn_chain(g_drag.list) && rq(g_drag.list) == img(P_VT_LIST)) {
        uint32_t db = atomic_load_explicit(&g_drag_dy, memory_order_acquire); float dy; memcpy(&dy, &db, 4);
        float cw, ch, ux, uy; int32_t wh[2] = {0, 0}; rd(img(P_MGR) + M_W, wh, 8);
        cw = wh[0] > 0 ? (float)wh[0] : 1920.f; ch = wh[1] > 0 ? (float)wh[1] : 1080.f;
        dm_tl2_ui_frame_to_canvas(g_drag.x0, g_drag.y0 + dy, cw, ch, &ux, &uy);
        int want = (int)lroundf(-(uy - g_drag.uy) / (float)g_drag.rowh);   // finger up = later rows; one row per row height moved
        if (want != g_drag.applied) { int top; list_scroll(top_menu(&top), g_drag.list, want - g_drag.applied, "drag"); g_drag.applied = want; }
    } else if (!ended) { LOG("[tl2ui] drag: list %#llx gone (menu %s)", (unsigned long long)g_drag.list, open ? "open" : "closed"); g_drag.list = 0; return; }
    if (ended) { LOG("[tl2ui] drag end: %+d rows on list %#llx", g_drag.applied, (unsigned long long)g_drag.list); g_drag.list = 0; }
}

// ---- per npad read (hle_hid.c fill(), after the front end's buttons; the buttons are left alone)
void dm_tl2_ui_pump(uint64_t *buttons);
void dm_tl2_ui_pump(uint64_t *buttons) {
    (void)buttons;
    { void dm_tl2_map_pump(void); dm_tl2_map_pump(); }   // hle_tl2_map.c (own gate DM_TL2_MAP): map tap requests, the map's hit rect
    if (g_ok < 0) setup();
    if (g_ok <= 0) return;
    if (g_mode > 0) {
        int top, open = menu_open_now(&top); static int was = -1, said;
        if (open != was && said < 2000) {
            said++; uint64_t me = top_menu(&top);
            LOG("[tl2ui] menu %s (console UI stack top %d %s)", open ? "open: taps click menu items" : "closed: taps stay with the touch layout", top,
                me ? menu_name(rq(me)) : "-");
        }
        was = open; atomic_store_explicit(&g_menu_open, open, memory_order_release);
        if (atomic_exchange_explicit(&g_tap_pending, 0, memory_order_acquire)) {
            uint64_t xy = atomic_load(&g_tap_xy); float x, y; uint32_t xb = (uint32_t)xy, yb = (uint32_t)(xy >> 32); memcpy(&x, &xb, 4); memcpy(&y, &yb, 4);
            if (!open) LOG("[tl2ui] tap %.3f,%.3f: no menu open any more, dropped", x, y);
            else click(x, y);
        }
        int d = atomic_exchange_explicit(&g_req_tab, 0, memory_order_acquire);
        if (d && open) tab_step(d);
        int pt = atomic_exchange_explicit(&g_req_panel, -1, memory_order_acquire);
        if (pt >= 0) { if (g_op.type >= 0) LOG("[tl2ui] open panel type %d: replaced by type %d", g_op.type, pt);
                       g_op.type = pt; g_op.step = 0; g_op.sub = -1; g_op.t0 = now_ns(); LOG("[tl2ui] open panel type %d requested", pt); }
        op_step();
        scroll_step(open); watch_step();
        static unsigned pub; if (open ? !(pub++ % 4) : atomic_load_explicit(&g_npub, memory_order_relaxed) != 0) publish_lists(open);
    }
    static unsigned tick; if (++tick % 30) return;
    char t[1200];
    if (g_dump > 0) { snprintf(t, sizeof t, "%s/ui-dump-now", g_dir); if (!access(t, F_OK)) { remove(t); dump(); } }
    if (g_mode > 0) {
        snprintf(t, sizeof t, "%s/ui-tap-now", g_dir); FILE *f = fopen(t, "r");
        if (f) {
            float x = -1, y = -1; int ok = fscanf(f, "%f %f", &x, &y) == 2; fclose(f); remove(t);
            if (!ok || !dm_tl2_ui_tap(x, y)) LOG("[tl2ui] %s: want \"x y\" in 0..1 while a menu is open", t);
        }
        snprintf(t, sizeof t, "%s/ui-panel-now", g_dir); f = fopen(t, "r");   // "type": open that panel of the game radial menu
        if (f) { int v = -1, ok = fscanf(f, "%d", &v) == 1; fclose(f); remove(t); if (!ok || !dm_tl2_ui_open_panel(v)) LOG("[tl2ui] %s: want a panel type", t); }
        snprintf(t, sizeof t, "%s/ui-tab-now", g_dir); f = fopen(t, "r");     // "+1" / "-1": the radial menu's tab
        if (f) { int v = 0, ok = fscanf(f, "%d", &v) == 1; fclose(f); remove(t); if (!ok || !dm_tl2_ui_tab(v)) LOG("[tl2ui] %s: want +1 / -1 while a menu is open", t); }
        snprintf(t, sizeof t, "%s/ui-scroll-now", g_dir); f = fopen(t, "r");  // "x y rows": scroll the list under x y (+ rows = later rows)
        if (f) { float x = -1, y = -1; int v = 0, ok = fscanf(f, "%f %f %d", &x, &y, &v) == 3; fclose(f); remove(t);
                 if (!ok || !dm_tl2_ui_scroll(x, y, v)) LOG("[tl2ui] %s: want \"x y rows\" (x y in 0..1, rows != 0) while a menu is open", t); }
    }
}
#endif
