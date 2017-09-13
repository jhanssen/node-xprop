/*global require*/
const xcb = require("..");
xcb.forWindow({ class: "XTerm", data: { property: xcb.atoms.WM_NAME, data: "Hey" } });
