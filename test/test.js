/*global require*/
const xcb = require("..");
xcb.forWindow({ class: "i3-frame.XTerm", data: "unmap" });
xcb.forWindow({ class: "i3-frame.XTerm", data: { what: "property", property: xcb.atoms.WM_NAME, data: "Hey" } });
xcb.forWindow({ class: "i3-frame.XTerm", data: "map" });
xcb.start();
