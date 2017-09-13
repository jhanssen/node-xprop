#include <nan.h>
#include <node_buffer.h>
#include <xcb/xcb.h>
#include <xcb/xcb_icccm.h>
#include <unordered_map>
#include <string>
#include <sstream>
#include <vector>
#include <assert.h>

template<typename Out>
inline void split(const std::string &s, char delim, Out result)
{
    std::stringstream ss;
    ss.str(s);
    std::string item;
    while (std::getline(ss, item, delim)) {
        *(result++) = item;
    }
}


inline std::vector<std::string> split(const std::string &s, char delim)
{
    std::vector<std::string> elems;
    split(s, delim, std::back_inserter(elems));
    return elems;
}

namespace std {
template<>
struct hash<std::vector<std::string> >
{
    std::size_t operator()(const std::vector<std::string>& vec) const
    {
        std::size_t hash = 5381;
        int c;

        for (const std::string& stdstr : vec) {
            const char* str = stdstr.c_str();
            while ((c = *str++))
                hash = ((hash << 5) + hash) + c; /* hash * 33 + c */
        }
        return hash;
    }
};
} // namespace std

struct Data
{
    Data() : conn(0) { }

    void ensure();
    template<typename T>
    void forEachScreen(T cb);
    template<typename T>
    void forEachWindow(xcb_window_t parent, T cb);

    xcb_connection_t* conn;
    int screenCount;
    uv_poll_t poller;

    struct Property
    {
        uint8_t mode;
        xcb_atom_t property, type;
        uint8_t format;
        std::string data;
    };
    static bool propertyFromObject(const v8::Local<v8::Object>& obj, Property& prop);
    std::unordered_map<std::vector<std::string>, Property> classProperties;

    static void pollCallback(uv_poll_t* handle, int status, int events);
} data;

class Traverser
{
public:
    Traverser();
    ~Traverser() {}

    void traverse(xcb_window_t win);

    bool hasMore() const { return !mCookies.empty(); }
    void run();

private:
    std::unordered_map<xcb_window_t, xcb_get_property_cookie_t> mCookies;
    std::vector<std::vector<std::string> > mMatches;
    uint32_t mLevel;
};

inline Traverser::Traverser()
    : mLevel(0)
{
    // match everything by default
    for (const auto &cls : data.classProperties) {
        mMatches.push_back(cls.first);
    }
}

void Traverser::traverse(xcb_window_t win)
{
    auto cookie = xcb_icccm_get_wm_class(data.conn, win);
    mCookies[win] = cookie;
}

void Traverser::run()
{
    std::unordered_map<xcb_window_t, xcb_get_property_cookie_t> newCookies;

    std::vector<uint32_t> hasmatch;
    xcb_icccm_get_wm_class_reply_t wmclass;
    for (const auto& cookie : mCookies) {
        if (xcb_icccm_get_wm_class_reply(data.conn, cookie.second, &wmclass, nullptr)) {
            // printf("matching %s(%u) vs %zu candidates\n", wmclass.class_name, mLevel, mMatches.size());
            // check if we match any of the items in the level
            auto begin = mMatches.begin();
            auto it = mMatches.begin();
            while (it != mMatches.end()) {
                if (it->size() > mLevel && (*it)[mLevel] == wmclass.class_name) {
                    if (std::find(hasmatch.begin(), hasmatch.end(), it - begin) == hasmatch.end()) {
                        hasmatch.push_back(it - begin);
                    }
                    // if we've matched everything, query children
                    if (it->size() == mLevel + 1) {
                        printf("matched full thingy\n");
                    } else {
                        // printf("matched sub, querying children\n");
                        // start the next property run
                        data.forEachWindow(cookie.first, [&newCookies](xcb_connection_t* conn, xcb_window_t win) {
                                auto cookie = xcb_icccm_get_wm_class(conn, win);
                                newCookies[win] = cookie;
                            });
                    }
                } else {
                    printf("didn't match %s(%u)\n", wmclass.class_name, mLevel);
                }
                ++it;
            }
            xcb_icccm_get_wm_class_reply_wipe(&wmclass);
        }
    }
    std::sort(hasmatch.begin(), hasmatch.end());
    // take out all non-matches
    // printf("%u matches\n", mMatches.size());
    auto begin = mMatches.begin();
    uint32_t where = mMatches.size();
    for (int32_t i = static_cast<int32_t>(hasmatch.size()) - 1; i >= 0; --i) {
        uint32_t cnt = where - (hasmatch[i] + 1);
        // printf("iter %d where %u, hadmatch %u cnt %u\n", i, where, hasmatch[i], cnt);
        if (cnt > 0) {
            // printf("erasing from %u to %u (%u)\n", where - cnt, where, cnt);
            mMatches.erase(begin + where - cnt, begin + where);
        }
        where = hasmatch[i];
    }

    ++mLevel;
    std::swap(mCookies, newCookies);
    // printf("got %lu children for level %d\n", mCookies.size(), mLevel);
}

class GrabServer
{
public:
    GrabServer(xcb_connection_t* conn)
        : mConn(conn)
    {
        xcb_grab_server(mConn);
    }
    ~GrabServer()
    {
        xcb_ungrab_server(mConn);
        xcb_flush(mConn);
    }

private:
    xcb_connection_t* mConn;
};

template<typename T>
inline void Data::forEachScreen(T cb)
{
    xcb_screen_iterator_t screen_iter = xcb_setup_roots_iterator(xcb_get_setup(conn));
    for (; screen_iter.rem != 0; xcb_screen_next(&screen_iter)) {
        cb(conn, screen_iter.data);
    }
}

template<typename T>
inline void Data::forEachWindow(xcb_window_t parent, T cb)
{
    xcb_query_tree_cookie_t cookie = xcb_query_tree(conn, parent);
    xcb_query_tree_reply_t* reply = xcb_query_tree_reply(conn, cookie, nullptr);
    if (!reply)
        return;
    const int num = xcb_query_tree_children_length(reply);
    if (!num) {
        free(reply);
        return;
    }
    xcb_window_t* children = xcb_query_tree_children(reply);
    for (int i = 0; i < num; ++i) {
        cb(conn, children[i]);
    }
    free(reply);
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
        //printf("got event %d\n", event->response_type & ~0x80);
        /*
        if ((event->response_type & ~0x80) == XCB_MAP_NOTIFY) {
            // window mapped
            xcb_map_notify_event_t* mapEvent = reinterpret_cast<xcb_map_notify_event_t*>(event);
            printf("mapped window %d\n", mapEvent->window);

            auto cookie = xcb_icccm_get_wm_class(data->conn, mapEvent->window);
            xcb_icccm_get_wm_class_reply_t wmclass;
            if (xcb_icccm_get_wm_class_reply(data->conn, cookie, &wmclass, nullptr)) {
                printf("class is %s\n", wmclass.class_name);
                xcb_icccm_get_wm_class_reply_wipe(&wmclass);
            }
        } else */
        if ((event->response_type & ~0x80) == XCB_REPARENT_NOTIFY) {
            xcb_reparent_notify_event_t* reparentEvent = reinterpret_cast<xcb_reparent_notify_event_t*>(event);
            Traverser traverser;
            traverser.traverse(reparentEvent->parent);
            while (traverser.hasMore()) {
                traverser.run();
            }


            // printf("reparented window %d\n", reparentEvent->window);

            // auto cookie = xcb_icccm_get_wm_class(data->conn, reparentEvent->window);
            // xcb_icccm_get_wm_class_reply_t wmclass;
            // if (xcb_icccm_get_wm_class_reply(data->conn, cookie, &wmclass, nullptr)) {
            //     printf("class is %s\n", wmclass.class_name);
            //     xcb_icccm_get_wm_class_reply_wipe(&wmclass);
            // }
        }
        // switch (event->response_type & ~0x80) {
        // }
        free(event);
    }
}

bool Data::propertyFromObject(const v8::Local<v8::Object>& obj, Property& prop)
{
    Nan::HandleScope scope;
    auto ctx = Nan::GetCurrentContext();

    auto atom = [](const v8::Local<v8::Value>& val) -> xcb_atom_t {
        if (val->IsNumber()) {
            return v8::Local<v8::Int32>::Cast(val)->Value();
        } else {
            v8::String::Utf8Value str(val);
            xcb_intern_atom_cookie_t cookie = xcb_intern_atom(data.conn, 0, str.length(), *str);
            xcb_intern_atom_reply_t* reply = xcb_intern_atom_reply(data.conn, cookie, nullptr);
            xcb_atom_t atom = XCB_ATOM_NONE;
            if (reply) {
                atom = reply->atom;
                free(reply);
            }
            return atom;
        }
    };

    auto modeStr = Nan::New("mode").ToLocalChecked();
    auto propertyStr = Nan::New("property").ToLocalChecked();
    auto typeStr = Nan::New("type").ToLocalChecked();
    auto formatStr = Nan::New("format").ToLocalChecked();
    auto dataStr = Nan::New("data").ToLocalChecked();

    // required properties
    if (!obj->Has(propertyStr) || !obj->Has(dataStr)) {
        Nan::ThrowError("Needs at least property and data");
        return false;
    }

    if (obj->Has(modeStr)) {
        prop.mode = v8::Local<v8::Number>::Cast(obj->Get(ctx, modeStr).ToLocalChecked())->Value();
        switch (prop.mode) {
        case XCB_PROP_MODE_REPLACE:
        case XCB_PROP_MODE_PREPEND:
        case XCB_PROP_MODE_APPEND:
            break;
        default:
            Nan::ThrowError("Invalid mode");
            return false;
        }
    } else {
        prop.mode = XCB_PROP_MODE_REPLACE;
    }
    prop.property = atom(obj->Get(ctx, propertyStr).ToLocalChecked());
    if (obj->Has(typeStr)) {
        prop.type = atom(obj->Get(ctx, typeStr).ToLocalChecked());
    } else {
        prop.type = XCB_ATOM_STRING;
    }
    if (obj->Has(formatStr)) {
        auto val = v8::Local<v8::Int32>::Cast(obj->Get(ctx, formatStr).ToLocalChecked())->Value();
        switch (val) {
        case 8:
        case 16:
        case 32:
            break;
        default:
            Nan::ThrowError("Invalid format");
            return false;
        }
    } else {
        prop.format = 8;
    }
    auto data = obj->Get(ctx, dataStr).ToLocalChecked();
    if (node::Buffer::HasInstance(data)) {
        const size_t len = node::Buffer::Length(data);
        const char* ptr = node::Buffer::Data(data);
        prop.data = std::string(ptr, len);
    } else {
        // assume Utf8String
        v8::String::Utf8Value str(data);
        prop.data = std::string(*str, str.length());
    }
    return true;
}

static void ForWindowClass(const v8::Local<v8::Value>& cls, const v8::Local<v8::Value>& val)
{
    if (!cls->IsString()) {
        Nan::ThrowError("Class needs to be a string");
        return;
    }
    if (!val->IsObject()) {
        Nan::ThrowError("Data needs to be an object");
        return;
    }
    Nan::HandleScope scope;
    data.ensure();

    const std::string clsstr = *v8::String::Utf8Value(cls);
    v8::Local<v8::Object> obj = v8::Local<v8::Object>::Cast(val);

    Data::Property prop;
    if (!Data::propertyFromObject(obj, prop))
        return;
    // if we have an existing window, handle that here
    data.classProperties[split(clsstr, '.')] = std::move(prop);

    GrabServer grab(data.conn);
    Traverser traverser;
    data.forEachScreen([&traverser](xcb_connection_t*, xcb_screen_t* screen) {
            data.forEachWindow(screen->root, [&traverser](xcb_connection_t*, xcb_window_t win) {
                    traverser.traverse(win);
                });
        });
    while (traverser.hasMore()) {
        traverser.run();
    }
}

static void ForWindow(const Nan::FunctionCallbackInfo<v8::Value>& args)
{
    if (args.Length() != 1 || !args[0]->IsObject()) {
        Nan::ThrowError("Needs one argument of type object");
        return;
    }

    v8::Local<v8::Object> obj = v8::Local<v8::Object>::Cast(args[0]);

    auto classStr = Nan::New("class").ToLocalChecked();
    auto dataStr = Nan::New("data").ToLocalChecked();
    if (!obj->Has(dataStr) || !obj->Has(classStr)) {
        Nan::ThrowError("Needs a class and data property");
        return;
    }
    ForWindowClass(obj->Get(Nan::GetCurrentContext(), classStr).ToLocalChecked(),
                   obj->Get(Nan::GetCurrentContext(), dataStr).ToLocalChecked());
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
