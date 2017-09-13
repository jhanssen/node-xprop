/*global require*/
const xcb = require("..");
xcb.forWindow({ class: "i3-frame.XTerm", data: { property: xcb.atoms.WM_NAME, data: "Hey" } });
