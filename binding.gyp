{
  "targets": [
    {
      "include_dirs": [
	"<!(node -e \"require('nan')\")",
	"<!@(pkg-config xcb --cflags-only-I | sed s/-I//g)",
	"<!@(pkg-config xcb-icccm --cflags-only-I | sed s/-I//g)"
      ],
      "libraries": [
	"<!@(pkg-config xcb --libs)",
	"<!@(pkg-config xcb-icccm --libs)"
      ],
      "target_name": "xprop",
      "sources": [ "src/xprop.cpp" ]
    }
  ]
}
