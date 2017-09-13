#include <nan.h>
#include <xcb/xcb.h>
#include <xcb/xcb_icccm.h>

struct Data
{
    Data() : conn(0) { }

    void ensure();
    template<typename T>
    void forEachScreen(T cb);

    xcb_connection_t* conn;
    int screenCount;
    uv_poll_t poller;

    static void pollCallback(uv_poll_t* handle, int status, int events);
} data;

template<typename T>
void Data::forEachScreen(T cb)
{
    xcb_screen_iterator_t screen_iter = xcb_setup_roots_iterator(xcb_get_setup(conn));
    for (; screen_iter.rem != 0; xcb_screen_next(&screen_iter)) {
        cb(conn, screen_iter.data);
    }
}

void Data::ensure()
{
    if (conn)
        return;
    conn = xcb_connect(NULL, &screenCount);
    data.forEachScreen([](xcb_connection_t* conn, xcb_screen_t* screen) {
            xcb_window_t root = screen->root;
            printf("setting up on root %d\n", screen->root);
            uint32_t mask = XCB_CW_EVENT_MASK;
            uint32_t values[2] = { XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY, 0 };
            xcb_change_window_attributes_checked(conn, root, mask, values);
            xcb_flush(conn);
        });
    int fd = xcb_get_file_descriptor(conn);
    poller.data = &data;
    uv_poll_init(uv_default_loop(), &poller, fd);
    uv_poll_start(&poller, UV_READABLE, Data::pollCallback);
    printf("setup xcb listener\n");
}

void Data::pollCallback(uv_poll_t* handle, int status, int events)
{
    Data* data = static_cast<Data*>(handle->data);
    xcb_generic_event_t* event;
    while ((event = xcb_poll_for_event(data->conn))) {
        printf("got event %d\n", event->response_type & ~0x80);
        if ((event->response_type & ~0x80) == XCB_MAP_NOTIFY) {
            // window mapped
            xcb_map_notify_event_t* mapEvent = reinterpret_cast<xcb_map_notify_event_t*>(event);
            printf("mapped window %d\n", mapEvent->window);

            auto cookie = xcb_icccm_get_wm_class(data->conn, mapEvent->window);
            xcb_icccm_get_wm_class_reply_t wmclass;
            xcb_icccm_get_wm_class_reply(data->conn, cookie, &wmclass, nullptr);
            printf("class is %s\n", wmclass.class_name);
            xcb_icccm_get_wm_class_reply_wipe(&wmclass);
        } else if ((event->response_type & ~0x80) == XCB_REPARENT_NOTIFY) {
            xcb_reparent_notify_event_t* reparentEvent = reinterpret_cast<xcb_reparent_notify_event_t*>(event);
            printf("reparented window %d\n", reparentEvent->window);

            auto cookie = xcb_icccm_get_wm_class(data->conn, reparentEvent->window);
            xcb_icccm_get_wm_class_reply_t wmclass;
            xcb_icccm_get_wm_class_reply(data->conn, cookie, &wmclass, nullptr);
            printf("class is %s\n", wmclass.class_name);
            xcb_icccm_get_wm_class_reply_wipe(&wmclass);
        }
        // switch (event->response_type & ~0x80) {
        // }
        free(event);
    }
}

static void ForWindowClass(const v8::Local<v8::Value>& obj)
{
    Nan::HandleScope scope;
    data.ensure();
}

static void ForWindowName(const v8::Local<v8::Value>& obj)
{
    Nan::HandleScope scope;
    data.ensure();
}

static void ForWindow(const Nan::FunctionCallbackInfo<v8::Value>& args)
{
    if (args.Length() != 1 || !args[0]->IsObject()) {
        Nan::ThrowError("Needs one argument of type object");
        return;
    }

    v8::Local<v8::Object> obj = v8::Local<v8::Object>::Cast(args[0]);

    auto classStr = Nan::New("class").ToLocalChecked();
    auto nameStr = Nan::New("name").ToLocalChecked();
    // if we have a class key
    if ((obj->HasOwnProperty(Nan::GetCurrentContext(), classStr)).ToChecked()) {
        ForWindowClass(obj->Get(Nan::GetCurrentContext(), classStr).ToLocalChecked());
    }
    if ((obj->HasOwnProperty(Nan::GetCurrentContext(), nameStr)).ToChecked()) {
        ForWindowName(obj->Get(Nan::GetCurrentContext(), nameStr).ToLocalChecked());
    }
}

static v8::Local<v8::Object> getAtoms()
{
    Nan::EscapableHandleScope scope;
    auto iso = v8::Isolate::GetCurrent();
    v8::Local<v8::Object> obj = v8::Object::New(iso);
    obj->Set(Nan::New("ANY").ToLocalChecked(), v8::Number::New(iso, XCB_ATOM_ANY));
    obj->Set(Nan::New("PRIMARY").ToLocalChecked(), v8::Number::New(iso, XCB_ATOM_PRIMARY));
    obj->Set(Nan::New("SECONDARY").ToLocalChecked(), v8::Number::New(iso, XCB_ATOM_SECONDARY));
    obj->Set(Nan::New("ARC").ToLocalChecked(), v8::Number::New(iso, XCB_ATOM_ARC));
    obj->Set(Nan::New("ATOM").ToLocalChecked(), v8::Number::New(iso, XCB_ATOM_ATOM));
    obj->Set(Nan::New("BITMAP").ToLocalChecked(), v8::Number::New(iso, XCB_ATOM_BITMAP));
    obj->Set(Nan::New("CARDINAL").ToLocalChecked(), v8::Number::New(iso, XCB_ATOM_CARDINAL));
    obj->Set(Nan::New("COLORMAP").ToLocalChecked(), v8::Number::New(iso, XCB_ATOM_COLORMAP));
    obj->Set(Nan::New("CURSOR").ToLocalChecked(), v8::Number::New(iso, XCB_ATOM_CURSOR));
    obj->Set(Nan::New("CUT_BUFFER0").ToLocalChecked(), v8::Number::New(iso, XCB_ATOM_CUT_BUFFER0));
    obj->Set(Nan::New("CUT_BUFFER1").ToLocalChecked(), v8::Number::New(iso, XCB_ATOM_CUT_BUFFER1));
    obj->Set(Nan::New("CUT_BUFFER2").ToLocalChecked(), v8::Number::New(iso, XCB_ATOM_CUT_BUFFER2));
    obj->Set(Nan::New("CUT_BUFFER3").ToLocalChecked(), v8::Number::New(iso, XCB_ATOM_CUT_BUFFER3));
    obj->Set(Nan::New("CUT_BUFFER4").ToLocalChecked(), v8::Number::New(iso, XCB_ATOM_CUT_BUFFER4));
    obj->Set(Nan::New("CUT_BUFFER5").ToLocalChecked(), v8::Number::New(iso, XCB_ATOM_CUT_BUFFER5));
    obj->Set(Nan::New("CUT_BUFFER6").ToLocalChecked(), v8::Number::New(iso, XCB_ATOM_CUT_BUFFER6));
    obj->Set(Nan::New("CUT_BUFFER7").ToLocalChecked(), v8::Number::New(iso, XCB_ATOM_CUT_BUFFER7));
    obj->Set(Nan::New("DRAWABLE").ToLocalChecked(), v8::Number::New(iso, XCB_ATOM_DRAWABLE));
    obj->Set(Nan::New("FONT").ToLocalChecked(), v8::Number::New(iso, XCB_ATOM_FONT));
    obj->Set(Nan::New("INTEGER").ToLocalChecked(), v8::Number::New(iso, XCB_ATOM_INTEGER));
    obj->Set(Nan::New("PIXMAP").ToLocalChecked(), v8::Number::New(iso, XCB_ATOM_PIXMAP));
    obj->Set(Nan::New("POINT").ToLocalChecked(), v8::Number::New(iso, XCB_ATOM_POINT));
    obj->Set(Nan::New("RECTANGLE").ToLocalChecked(), v8::Number::New(iso, XCB_ATOM_RECTANGLE));
    obj->Set(Nan::New("RESOURCE_MANAGER").ToLocalChecked(), v8::Number::New(iso, XCB_ATOM_RESOURCE_MANAGER));
    obj->Set(Nan::New("RGB_COLOR_MAP").ToLocalChecked(), v8::Number::New(iso, XCB_ATOM_RGB_COLOR_MAP));
    obj->Set(Nan::New("RGB_BEST_MAP").ToLocalChecked(), v8::Number::New(iso, XCB_ATOM_RGB_BEST_MAP));
    obj->Set(Nan::New("RGB_BLUE_MAP").ToLocalChecked(), v8::Number::New(iso, XCB_ATOM_RGB_BLUE_MAP));
    obj->Set(Nan::New("RGB_DEFAULT_MAP").ToLocalChecked(), v8::Number::New(iso, XCB_ATOM_RGB_DEFAULT_MAP));
    obj->Set(Nan::New("RGB_GRAY_MAP").ToLocalChecked(), v8::Number::New(iso, XCB_ATOM_RGB_GRAY_MAP));
    obj->Set(Nan::New("RGB_GREEN_MAP").ToLocalChecked(), v8::Number::New(iso, XCB_ATOM_RGB_GREEN_MAP));
    obj->Set(Nan::New("RGB_RED_MAP").ToLocalChecked(), v8::Number::New(iso, XCB_ATOM_RGB_RED_MAP));
    obj->Set(Nan::New("STRING").ToLocalChecked(), v8::Number::New(iso, XCB_ATOM_STRING));
    obj->Set(Nan::New("VISUALID").ToLocalChecked(), v8::Number::New(iso, XCB_ATOM_VISUALID));
    obj->Set(Nan::New("WINDOW").ToLocalChecked(), v8::Number::New(iso, XCB_ATOM_WINDOW));
    obj->Set(Nan::New("WM_COMMAND").ToLocalChecked(), v8::Number::New(iso, XCB_ATOM_WM_COMMAND));
    obj->Set(Nan::New("WM_HINTS").ToLocalChecked(), v8::Number::New(iso, XCB_ATOM_WM_HINTS));
    obj->Set(Nan::New("WM_CLIENT_MACHINE").ToLocalChecked(), v8::Number::New(iso, XCB_ATOM_WM_CLIENT_MACHINE));
    obj->Set(Nan::New("WM_ICON_NAME").ToLocalChecked(), v8::Number::New(iso, XCB_ATOM_WM_ICON_NAME));
    obj->Set(Nan::New("WM_ICON_SIZE").ToLocalChecked(), v8::Number::New(iso, XCB_ATOM_WM_ICON_SIZE));
    obj->Set(Nan::New("WM_NAME").ToLocalChecked(), v8::Number::New(iso, XCB_ATOM_WM_NAME));
    obj->Set(Nan::New("WM_NORMAL_HINTS").ToLocalChecked(), v8::Number::New(iso, XCB_ATOM_WM_NORMAL_HINTS));
    obj->Set(Nan::New("WM_SIZE_HINTS").ToLocalChecked(), v8::Number::New(iso, XCB_ATOM_WM_SIZE_HINTS));
    obj->Set(Nan::New("WM_ZOOM_HINTS").ToLocalChecked(), v8::Number::New(iso, XCB_ATOM_WM_ZOOM_HINTS));
    obj->Set(Nan::New("MIN_SPACE").ToLocalChecked(), v8::Number::New(iso, XCB_ATOM_MIN_SPACE));
    obj->Set(Nan::New("NORM_SPACE").ToLocalChecked(), v8::Number::New(iso, XCB_ATOM_NORM_SPACE));
    obj->Set(Nan::New("MAX_SPACE").ToLocalChecked(), v8::Number::New(iso, XCB_ATOM_MAX_SPACE));
    obj->Set(Nan::New("END_SPACE").ToLocalChecked(), v8::Number::New(iso, XCB_ATOM_END_SPACE));
    obj->Set(Nan::New("SUPERSCRIPT_X").ToLocalChecked(), v8::Number::New(iso, XCB_ATOM_SUPERSCRIPT_X));
    obj->Set(Nan::New("SUPERSCRIPT_Y").ToLocalChecked(), v8::Number::New(iso, XCB_ATOM_SUPERSCRIPT_Y));
    obj->Set(Nan::New("SUBSCRIPT_X").ToLocalChecked(), v8::Number::New(iso, XCB_ATOM_SUBSCRIPT_X));
    obj->Set(Nan::New("SUBSCRIPT_Y").ToLocalChecked(), v8::Number::New(iso, XCB_ATOM_SUBSCRIPT_Y));
    obj->Set(Nan::New("UNDERLINE_POSITION").ToLocalChecked(), v8::Number::New(iso, XCB_ATOM_UNDERLINE_POSITION));
    obj->Set(Nan::New("UNDERLINE_THICKNESS").ToLocalChecked(), v8::Number::New(iso, XCB_ATOM_UNDERLINE_THICKNESS));
    obj->Set(Nan::New("STRIKEOUT_ASCENT").ToLocalChecked(), v8::Number::New(iso, XCB_ATOM_STRIKEOUT_ASCENT));
    obj->Set(Nan::New("STRIKEOUT_DESCENT").ToLocalChecked(), v8::Number::New(iso, XCB_ATOM_STRIKEOUT_DESCENT));
    obj->Set(Nan::New("ITALIC_ANGLE").ToLocalChecked(), v8::Number::New(iso, XCB_ATOM_ITALIC_ANGLE));
    obj->Set(Nan::New("X_HEIGHT").ToLocalChecked(), v8::Number::New(iso, XCB_ATOM_X_HEIGHT));
    obj->Set(Nan::New("QUAD_WIDTH").ToLocalChecked(), v8::Number::New(iso, XCB_ATOM_QUAD_WIDTH));
    obj->Set(Nan::New("WEIGHT").ToLocalChecked(), v8::Number::New(iso, XCB_ATOM_WEIGHT));
    obj->Set(Nan::New("POINT_SIZE").ToLocalChecked(), v8::Number::New(iso, XCB_ATOM_POINT_SIZE));
    obj->Set(Nan::New("RESOLUTION").ToLocalChecked(), v8::Number::New(iso, XCB_ATOM_RESOLUTION));
    obj->Set(Nan::New("COPYRIGHT").ToLocalChecked(), v8::Number::New(iso, XCB_ATOM_COPYRIGHT));
    obj->Set(Nan::New("NOTICE").ToLocalChecked(), v8::Number::New(iso, XCB_ATOM_NOTICE));
    obj->Set(Nan::New("FONT_NAME").ToLocalChecked(), v8::Number::New(iso, XCB_ATOM_FONT_NAME));
    obj->Set(Nan::New("FAMILY_NAME").ToLocalChecked(), v8::Number::New(iso, XCB_ATOM_FAMILY_NAME));
    obj->Set(Nan::New("FULL_NAME").ToLocalChecked(), v8::Number::New(iso, XCB_ATOM_FULL_NAME));
    obj->Set(Nan::New("CAP_HEIGHT").ToLocalChecked(), v8::Number::New(iso, XCB_ATOM_CAP_HEIGHT));
    obj->Set(Nan::New("WM_CLASS").ToLocalChecked(), v8::Number::New(iso, XCB_ATOM_WM_CLASS));
    obj->Set(Nan::New("WM_TRANSIENT_FOR").ToLocalChecked(), v8::Number::New(iso, XCB_ATOM_WM_TRANSIENT_FOR));
    return scope.Escape(obj);
}

static void Initialize(v8::Local<v8::Object> exports)
{
    exports->Set(Nan::New("forWindow").ToLocalChecked(),
                 Nan::New<v8::FunctionTemplate>(ForWindow)->GetFunction());
    exports->Set(Nan::New("atoms").ToLocalChecked(), getAtoms());
}

NODE_MODULE(xprop, Initialize)
