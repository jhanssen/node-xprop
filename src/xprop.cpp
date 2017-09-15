#include <nan.h>
#include <node_buffer.h>
#include <xcb/xcb.h>
#include <xcb/xcb_icccm.h>
#include <unordered_map>
#include <unordered_set>
#include <string>
#include <sstream>
#include <vector>
#include <memory>
#include <assert.h>

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

    xcb_atom_t atom_wm_state;
    std::unordered_set<xcb_window_t> seen;

    struct Base
    {
        virtual ~Base() { }
        virtual void run(xcb_window_t win) const = 0;
    };

    struct Property : public Base
    {
        virtual void run(xcb_window_t win) const override;

        uint8_t mode;
        xcb_atom_t property, type;
        uint8_t format;
        std::vector<uint8_t> data;
    };

    struct Mapper : public Base
    {
        virtual void run(xcb_window_t win) const override;
    };

    struct Unmapper : public Base
    {
        virtual void run(xcb_window_t win) const override;
    };

    struct Remapper : public Base
    {
        virtual void run(xcb_window_t win) const override;
    };

    struct PropertyClearer : public Base
    {
        virtual void run(xcb_window_t win) const override;
    };

    struct Configurer : public Base
    {
        Configurer(uint32_t xv, uint32_t yv, uint32_t widthv, uint32_t heightv)
            : x(xv), y(yv), width(widthv), height(heightv)
        {
        }

        uint32_t x, y, width, height;

        virtual void run(xcb_window_t win) const override;
    };

    struct Overrider : public Base
    {
        Overrider(bool o) : on(o) { }

        bool on;

        virtual void run(xcb_window_t win) const override;
    };

    struct Pending
    {
        xcb_window_t window;
        std::shared_ptr<Base> base;
    };
    std::vector<std::pair<uint64_t, std::vector<Pending> > > pendingProperties;

    static bool baseFromValue(const v8::Local<v8::Value>& val, std::shared_ptr<Base>* base);
    std::unordered_map<std::vector<std::string>, std::vector<std::shared_ptr<Base> > > classProperties;

    static void pollCallback(uv_poll_t* handle, int status, int events);
} data;

void Data::Property::run(xcb_window_t win) const
{
    // printf("prop change\n");
    xcb_change_property(::data.conn, mode, win, property, type, format, (data.size() * 8) / format, &data[0]);
}

void Data::Mapper::run(xcb_window_t win) const
{
    const uint64_t key = (static_cast<uint64_t>(XCB_MAP_NOTIFY) << 32) | win;
    data.pendingProperties.push_back(std::make_pair(key, std::vector<Pending>()));
    xcb_map_window(data.conn, win);
    xcb_flush(data.conn);
    // printf("mapped\n");
}

void Data::Unmapper::run(xcb_window_t win) const
{
    const uint64_t key = (static_cast<uint64_t>(XCB_UNMAP_NOTIFY) << 32) | win;
    data.pendingProperties.push_back(std::make_pair(key, std::vector<Pending>()));
    xcb_unmap_window(data.conn, win);
    xcb_flush(data.conn);
    // printf("unmapped %u %llu\n", win, key);
}

void Data::Remapper::run(xcb_window_t win) const
{
    xcb_unmap_window(data.conn, win);
    xcb_flush(data.conn);
    xcb_map_window(data.conn, win);
    xcb_flush(data.conn);
}

void Data::Configurer::run(xcb_window_t win) const
{
    uint32_t values[4] = { x, y, width, height };
    xcb_configure_window(data.conn, win, XCB_CONFIG_WINDOW_X
                         | XCB_CONFIG_WINDOW_Y
                         | XCB_CONFIG_WINDOW_WIDTH
                         | XCB_CONFIG_WINDOW_HEIGHT,
                         values);
    xcb_flush(data.conn);
}

void Data::Overrider::run(xcb_window_t win) const
{
    uint32_t value[] = { on ? 1u : 0u };
    xcb_change_window_attributes(data.conn, win, XCB_CW_OVERRIDE_REDIRECT, value);
    xcb_flush(data.conn);
}

void Data::PropertyClearer::run(xcb_window_t win) const
{
    GrabServer grab(data.conn);
    xcb_list_properties_cookie_t listCookie = xcb_list_properties(data.conn, win);
    xcb_list_properties_reply_t* listReply = xcb_list_properties_reply(data.conn, listCookie, nullptr);
    if (!listReply)
        return;
    const int num = xcb_list_properties_atoms_length(listReply);
    xcb_atom_t* first = xcb_list_properties_atoms(listReply);
    if (first && num > 0) {
        const auto last = first + num;
        for (xcb_atom_t* atom = first; atom != last; ++atom) {
            switch (*atom) {
            case XCB_ATOM_WM_CLASS:
            case XCB_ATOM_WM_NAME:
            case XCB_ATOM_WM_NORMAL_HINTS:
                break;
            default:
                if (*atom != data.atom_wm_state) {
                    xcb_delete_property(data.conn, win, *atom);
                }
                break;
            }
        }
    }

    free(listReply);
}

class Changer
{
public:
    Changer(size_t off = 0) : mOffset(off) {}
    ~Changer() {}

    void change(xcb_window_t win, const std::shared_ptr<Data::Base>& b);
    void finish() { xcb_flush(data.conn); }

private:
    size_t mOffset;
};

void Changer::change(xcb_window_t win, const std::shared_ptr<Data::Base>& b)
{
    if (mOffset >= data.pendingProperties.size()) {
        // printf("changing...\n");
        b->run(win);
    } else {
        // printf("postponing change\n");
        data.pendingProperties.back().second.push_back(Data::Pending{ win, b });
    }
}

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
    Changer changer;

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
                        // printf("matched full thingy\n");
                        auto prop = data.classProperties.find(*it);
                        assert(prop != data.classProperties.end());
                        const auto& vec = prop->second;
                        for (const auto& base : vec) {
                            changer.change(cookie.first, base);
                        }
                    } else {
                        // printf("matched sub, querying children\n");
                        // start the next property run
                        data.forEachWindow(cookie.first, [&newCookies](xcb_connection_t* conn, xcb_window_t win) {
                                auto cookie = xcb_icccm_get_wm_class(conn, win);
                                newCookies[win] = cookie;
                            });
                    }
                // } else {
                //     printf("didn't match %s(%u)\n", wmclass.class_name, mLevel);
                }
                ++it;
            }
            xcb_icccm_get_wm_class_reply_wipe(&wmclass);
        }
    }
    changer.finish();

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
            // printf("setting up on root %d\n", screen->root);
            uint32_t mask = XCB_CW_EVENT_MASK;
            uint32_t values[2] = { XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY, 0 };
            xcb_change_window_attributes_checked(conn, root, mask, values);
            xcb_flush(conn);
        });

    xcb_intern_atom_cookie_t cookie = xcb_intern_atom(conn, 0, 8, "WM_STATE");
    xcb_intern_atom_reply_t* reply = xcb_intern_atom_reply(conn, cookie, nullptr);
    atom_wm_state = XCB_ATOM_NONE;
    if (reply) {
        atom_wm_state = reply->atom;
        free(reply);
    }

    int fd = xcb_get_file_descriptor(conn);
    //poller.data = &data;
    poller.data = 0;
    uv_poll_init(uv_default_loop(), &poller, fd);
    uv_poll_start(&poller, UV_READABLE, Data::pollCallback);
    // printf("setup xcb listener\n");
}

void Data::pollCallback(uv_poll_t* handle, int status, int events)
{
    auto change = [](uint32_t type, xcb_window_t window) -> xcb_window_t {
        std::vector<uint64_t> keys;
        xcb_window_t real = XCB_WINDOW_NONE;
        keys.push_back((static_cast<uint64_t>(type) << 32) | window);
        data.forEachWindow(window, [&keys, &real, type](xcb_connection_t*, xcb_window_t win) {
                keys.push_back((static_cast<uint64_t>(type) << 32) | win);
                if (real == XCB_WINDOW_NONE)
                    real = win;
            });
        if (real == XCB_WINDOW_NONE)
            real = window;
        Changer changer(1);
        for (size_t i = 0; i < data.pendingProperties.size(); ++i) {
            const auto& p = data.pendingProperties[i];
            if (std::find(keys.begin(), keys.end(), p.first) != keys.end()) {
                for (const auto& item : p.second) {
                    changer.change(item.window, item.base);
                }
                data.pendingProperties.erase(data.pendingProperties.begin() + i);
                break;
            }
        }
        changer.finish();
        return real;
    };

    xcb_generic_event_t* event;
    while ((event = xcb_poll_for_event(data.conn))) {
        const auto eventType = event->response_type & ~0x80;
        if (eventType == XCB_MAP_NOTIFY) {
            // see if we have any pending changes for our window
            xcb_map_notify_event_t* mapEvent = reinterpret_cast<xcb_map_notify_event_t*>(event);
            const xcb_window_t real = change(XCB_MAP_NOTIFY, mapEvent->window);

            if (data.seen.find(real) == data.seen.end()) {
                Traverser traverser;
                traverser.traverse(mapEvent->window);
                while (traverser.hasMore()) {
                    traverser.run();
                }
                data.seen.insert(real);
            }
        } else if (eventType == XCB_UNMAP_NOTIFY) {
            // see if we have any pending changes for our window
            xcb_unmap_notify_event_t* unmapEvent = reinterpret_cast<xcb_unmap_notify_event_t*>(event);
            change(XCB_UNMAP_NOTIFY, unmapEvent->window);
        } else if (eventType == XCB_REPARENT_NOTIFY) {
            // reparent might mean unmap?
#warning maybe check what our parent is. if we are being reparented into a window manager frame then this is probably a map instead of an unmap

            // see if we have any pending changes for our window
            xcb_reparent_notify_event_t* reparentEvent = reinterpret_cast<xcb_reparent_notify_event_t*>(event);
            change(XCB_UNMAP_NOTIFY, reparentEvent->window);
        } else if (eventType == XCB_DESTROY_NOTIFY) {
            xcb_destroy_notify_event_t* destroyEvent = reinterpret_cast<xcb_destroy_notify_event_t*>(event);
            data.seen.erase(destroyEvent->window);
        }
        // printf("got event %d\n", eventType);
        free(event);
    }
}

bool Data::baseFromValue(const v8::Local<v8::Value>& val, std::shared_ptr<Base>* base)
{
    if (val->IsObject()) {
        Nan::HandleScope scope;
        auto ctx = Nan::GetCurrentContext();

        v8::Local<v8::Object> obj = v8::Local<v8::Object>::Cast(val);

        auto whatStr = Nan::New("what").ToLocalChecked();
        if (!obj->Has(whatStr)) {
            Nan::ThrowError("Needs a what");
            return false;
        }

        const std::string what = std::string(*v8::String::Utf8Value(obj->Get(ctx, whatStr).ToLocalChecked()));
        if (what == "override_redirect") {
            auto onStr = Nan::New("on").ToLocalChecked();
            if (!obj->Has(onStr)) {
                Nan::ThrowError("Needs at least on");
                return false;
            }
            const bool on = v8::Local<v8::Boolean>::Cast(obj->Get(ctx, onStr).ToLocalChecked())->Value();
            *base = std::make_shared<Overrider>(on);
            return true;
        } else if (what == "property") {
            // property?

            std::shared_ptr<Property> prop = std::make_shared<Property>();

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
                prop->mode = v8::Local<v8::Number>::Cast(obj->Get(ctx, modeStr).ToLocalChecked())->Value();
                switch (prop->mode) {
                case XCB_PROP_MODE_REPLACE:
                case XCB_PROP_MODE_PREPEND:
                case XCB_PROP_MODE_APPEND:
                    break;
                default:
                    Nan::ThrowError("Invalid mode");
                    return false;
                }
            } else {
                prop->mode = XCB_PROP_MODE_REPLACE;
            }
            prop->property = atom(obj->Get(ctx, propertyStr).ToLocalChecked());
            if (obj->Has(typeStr)) {
                prop->type = atom(obj->Get(ctx, typeStr).ToLocalChecked());
            } else {
                prop->type = XCB_ATOM_STRING;
            }
            if (obj->Has(formatStr)) {
                prop->format = v8::Local<v8::Int32>::Cast(obj->Get(ctx, formatStr).ToLocalChecked())->Value();
                switch (prop->format) {
                case 8:
                case 16:
                case 32:
                    break;
                default:
                    Nan::ThrowError("Invalid format");
                    return false;
                }
            } else {
                prop->format = 8;
            }
            auto dataval = obj->Get(ctx, dataStr).ToLocalChecked();
            if (node::Buffer::HasInstance(dataval)) {
                const size_t len = node::Buffer::Length(dataval);
                const char* ptr = node::Buffer::Data(dataval);
                prop->data.assign(reinterpret_cast<const uint8_t*>(ptr), reinterpret_cast<const uint8_t*>(ptr) + len);
            } else {
                // assume Utf8String
                v8::String::Utf8Value str(dataval);
                prop->data.assign(reinterpret_cast<const uint8_t*>(*str), reinterpret_cast<const uint8_t*>(*str) + str.length());
            }
            // if type is ATOM then try to internalize the data string
            if (prop->type == XCB_ATOM_ATOM) {
                xcb_intern_atom_cookie_t cookie = xcb_intern_atom(data.conn, 0, prop->data.size(), reinterpret_cast<char*>(&prop->data[0]));
                xcb_intern_atom_reply_t* reply = xcb_intern_atom_reply(data.conn, cookie, nullptr);
                xcb_atom_t atom = XCB_ATOM_NONE;
                if (reply) {
                    atom = reply->atom;
                    free(reply);
                }
                prop->data.resize(sizeof(xcb_atom_t));
                memcpy(&prop->data[0], &atom, sizeof(xcb_atom_t));
            }
            *base = prop;
            return true;
        } else if (what == "configure") {
            auto xStr = Nan::New("x").ToLocalChecked();
            auto yStr = Nan::New("y").ToLocalChecked();
            auto widthStr = Nan::New("width").ToLocalChecked();
            auto heightStr = Nan::New("height").ToLocalChecked();

            uint32_t x = 0, y = 0, w = 0, h = 0;
            if (obj->Has(xStr)) {
                x = v8::Local<v8::Uint32>::Cast(obj->Get(ctx, xStr).ToLocalChecked())->Value();
            }
            if (obj->Has(yStr)) {
                y = v8::Local<v8::Uint32>::Cast(obj->Get(ctx, yStr).ToLocalChecked())->Value();
            }
            if (obj->Has(widthStr)) {
                w = v8::Local<v8::Uint32>::Cast(obj->Get(ctx, widthStr).ToLocalChecked())->Value();
            }
            if (obj->Has(heightStr)) {
                h = v8::Local<v8::Uint32>::Cast(obj->Get(ctx, heightStr).ToLocalChecked())->Value();
            }
            *base = std::make_shared<Configurer>(x, y, w, h);
            return true;
        } else {
            Nan::ThrowError("Invalid what");
            return false;
        }
    } else if (val->IsString()) {
        // runner
        v8::String::Utf8Value str(val);
        if (!strncmp(*str, "map", str.length())) {
            *base = std::make_shared<Mapper>();
            return true;
        }
        if (!strncmp(*str, "unmap", str.length())) {
            *base = std::make_shared<Unmapper>();
            return true;
        }
        if (!strncmp(*str, "clear", str.length())) {
            *base = std::make_shared<PropertyClearer>();
            return true;
        }
        if (!strncmp(*str, "remap", str.length())) {
            *base = std::make_shared<Remapper>();
            return true;
        }
        Nan::ThrowError("Unknown data type");
        return false;
    } else {
        Nan::ThrowError("Data needs to be an object or string");
        return false;
    }
}

static void ForWindowClass(const v8::Local<v8::Value>& cls, const v8::Local<v8::Value>& val)
{
    if (!cls->IsString()) {
        Nan::ThrowError("Class needs to be a string");
        return;
    }
    Nan::HandleScope scope;
    data.ensure();

    const std::string clsstr = *v8::String::Utf8Value(cls);

    std::shared_ptr<Data::Base> base;
    if (!Data::baseFromValue(val, &base))
        return;
    // if we have an existing window, handle that here
    data.classProperties[split(clsstr, '.')].push_back(base);
}

static void Start(const Nan::FunctionCallbackInfo<v8::Value>& args)
{
    GrabServer grab(data.conn);
    Traverser traverser;
    data.forEachScreen([&traverser](xcb_connection_t*, xcb_screen_t* screen) {
            data.forEachWindow(screen->root, [&traverser](xcb_connection_t*, xcb_window_t win) {
                    xcb_window_t real = XCB_WINDOW_NONE;
                    data.forEachWindow(win, [&real](xcb_connection_t*, xcb_window_t win) {
                            if (real == XCB_WINDOW_NONE)
                                real = win;
                        });
                    if (real == XCB_WINDOW_NONE)
                        real = win;
                    if (data.seen.find(real) == data.seen.end()) {
                        data.seen.insert(real);
                        traverser.traverse(win);
                    }
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
    exports->Set(Nan::New("start").ToLocalChecked(),
                 Nan::New<v8::FunctionTemplate>(Start)->GetFunction());
    exports->Set(Nan::New("atoms").ToLocalChecked(), getAtoms());
}

NODE_MODULE(xprop, Initialize)
