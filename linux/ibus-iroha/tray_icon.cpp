#include "tray_icon.h"

#include <gio/gio.h>
#include <pango/pangocairo.h>
#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

namespace iroha::ibus {

namespace {

constexpr char kItemPath[] = "/StatusNotifierItem";
constexpr char kMenuPath[] = "/MenuBar";
constexpr char kItemInterface[] = "org.kde.StatusNotifierItem";
constexpr char kMenuInterface[] = "com.canonical.dbusmenu";
constexpr char kWatcherName[] = "org.kde.StatusNotifierWatcher";
constexpr char kWatcherPath[] = "/StatusNotifierWatcher";
constexpr char kSourcesSchema[] = "org.gnome.desktop.input-sources";

// StatusNotifierItem（KDE の仕様。Ubuntu の AppIndicator の拡張機能が受ける）
const char kItemXml[] = R"xml(<node>
 <interface name="org.kde.StatusNotifierItem">
  <property name="Category" type="s" access="read"/>
  <property name="Id" type="s" access="read"/>
  <property name="Title" type="s" access="read"/>
  <property name="Status" type="s" access="read"/>
  <property name="WindowId" type="i" access="read"/>
  <property name="IconThemePath" type="s" access="read"/>
  <property name="IconName" type="s" access="read"/>
  <property name="IconPixmap" type="a(iiay)" access="read"/>
  <property name="OverlayIconName" type="s" access="read"/>
  <property name="OverlayIconPixmap" type="a(iiay)" access="read"/>
  <property name="AttentionIconName" type="s" access="read"/>
  <property name="AttentionIconPixmap" type="a(iiay)" access="read"/>
  <property name="AttentionMovieName" type="s" access="read"/>
  <property name="ToolTip" type="(sa(iiay)ss)" access="read"/>
  <property name="ItemIsMenu" type="b" access="read"/>
  <property name="Menu" type="o" access="read"/>
  <method name="ContextMenu"><arg name="x" type="i" direction="in"/><arg name="y" type="i" direction="in"/></method>
  <method name="Activate"><arg name="x" type="i" direction="in"/><arg name="y" type="i" direction="in"/></method>
  <method name="SecondaryActivate"><arg name="x" type="i" direction="in"/><arg name="y" type="i" direction="in"/></method>
  <method name="Scroll"><arg name="delta" type="i" direction="in"/><arg name="orientation" type="s" direction="in"/></method>
  <signal name="NewTitle"/>
  <signal name="NewIcon"/>
  <signal name="NewAttentionIcon"/>
  <signal name="NewOverlayIcon"/>
  <signal name="NewToolTip"/>
  <signal name="NewStatus"><arg name="status" type="s"/></signal>
 </interface>
</node>)xml";

// アイコンのメニュー（dbusmenu。libdbusmenu と同じ形）
const char kMenuXml[] = R"xml(<node>
 <interface name="com.canonical.dbusmenu">
  <property name="Version" type="u" access="read"/>
  <property name="TextDirection" type="s" access="read"/>
  <property name="Status" type="s" access="read"/>
  <property name="IconThemePath" type="as" access="read"/>
  <method name="GetLayout">
   <arg type="i" name="parentId" direction="in"/>
   <arg type="i" name="recursionDepth" direction="in"/>
   <arg type="as" name="propertyNames" direction="in"/>
   <arg type="u" name="revision" direction="out"/>
   <arg type="(ia{sv}av)" name="layout" direction="out"/>
  </method>
  <method name="GetGroupProperties">
   <arg type="ai" name="ids" direction="in"/>
   <arg type="as" name="propertyNames" direction="in"/>
   <arg type="a(ia{sv})" name="properties" direction="out"/>
  </method>
  <method name="GetProperty">
   <arg type="i" name="id" direction="in"/>
   <arg type="s" name="name" direction="in"/>
   <arg type="v" name="value" direction="out"/>
  </method>
  <method name="Event">
   <arg type="i" name="id" direction="in"/>
   <arg type="s" name="eventId" direction="in"/>
   <arg type="v" name="data" direction="in"/>
   <arg type="u" name="timestamp" direction="in"/>
  </method>
  <method name="EventGroup">
   <arg type="a(isvu)" name="events" direction="in"/>
   <arg type="ai" name="idErrors" direction="out"/>
  </method>
  <method name="AboutToShow">
   <arg type="i" name="id" direction="in"/>
   <arg type="b" name="needUpdate" direction="out"/>
  </method>
  <method name="AboutToShowGroup">
   <arg type="ai" name="ids" direction="in"/>
   <arg type="ai" name="updatesNeeded" direction="out"/>
   <arg type="ai" name="idErrors" direction="out"/>
  </method>
  <signal name="ItemsPropertiesUpdated">
   <arg type="a(ia{sv})" name="updatedProps"/>
   <arg type="a(ias)" name="removedProps"/>
  </signal>
  <signal name="LayoutUpdated">
   <arg type="u" name="revision"/>
   <arg type="i" name="parent"/>
  </signal>
  <signal name="ItemActivationRequested">
   <arg type="i" name="id"/>
   <arg type="u" name="timestamp"/>
  </signal>
 </interface>
</node>)xml";

// dbusmenu は「_」を下線つきの文字（ニーモニック）の印に読むので、文字としての「_」は重ねる
std::string MenuLabel(const std::string& label) {
    std::string out;
    for (char c : label) {
        out += c;
        if (c == '_') out += '_';
    }
    return out;
}

// 入力モードの記号を描いたアイコン（大きさごと。上のバーは暗い色なので白い文字。背景は透明）。
// 形式は StatusNotifierItem の IconPixmap（幅・高さ・ARGB32 をネットワークバイト順に並べたもの、乗算済みでない値）
GVariant* IconPixmaps(const std::string& symbol) {
    GVariantBuilder builder;
    g_variant_builder_init(&builder, G_VARIANT_TYPE("a(iiay)"));
    for (int size : {16, 22, 24, 32, 48, 64}) {
        cairo_surface_t* surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, size, size);
        cairo_t* cr = cairo_create(surface);
        PangoLayout* layout = pango_cairo_create_layout(cr);
        PangoFontDescription* font = pango_font_description_from_string("Sans Bold");
        pango_font_description_set_absolute_size(font, std::round(size * 0.78) * PANGO_SCALE);
        pango_layout_set_font_description(layout, font);
        pango_font_description_free(font);
        pango_layout_set_text(layout, symbol.c_str(), -1);
        PangoRectangle ink;
        pango_layout_get_pixel_extents(layout, &ink, nullptr);
        cairo_set_source_rgb(cr, 1, 1, 1);
        cairo_move_to(cr, (size - ink.width) / 2.0 - ink.x, (size - ink.height) / 2.0 - ink.y);
        pango_cairo_show_layout(cr, layout);
        g_object_unref(layout);
        cairo_destroy(cr);
        cairo_surface_flush(surface);
        const unsigned char* data = cairo_image_surface_get_data(surface);
        const int stride = cairo_image_surface_get_stride(surface);
        std::string bytes;
        bytes.reserve(static_cast<size_t>(size) * size * 4);
        for (int y = 0; y < size; ++y) {
            const auto* row = reinterpret_cast<const uint32_t*>(data + y * stride);
            for (int x = 0; x < size; ++x) {
                const uint32_t pixel = row[x];
                const uint32_t alpha = pixel >> 24;
                auto channel = [alpha](uint32_t value) -> char {
                    return static_cast<char>(alpha == 0 ? 0 : std::min<uint32_t>(255, value * 255 / alpha));
                };
                bytes += static_cast<char>(alpha);
                bytes += channel((pixel >> 16) & 0xff);
                bytes += channel((pixel >> 8) & 0xff);
                bytes += channel(pixel & 0xff);
            }
        }
        cairo_surface_destroy(surface);
        GVariant* array = g_variant_new_fixed_array(G_VARIANT_TYPE_BYTE, bytes.data(), bytes.size(), 1);
        g_variant_builder_add(&builder, "(ii@ay)", size, size, array);
    }
    return g_variant_builder_end(&builder);
}

GVariant* EmptyPixmaps() { return g_variant_new_array(G_VARIANT_TYPE("(iiay)"), nullptr, 0); }

} // namespace

struct TrayIcon::Impl {
    Activate activate;
    Logger log;
    GDBusConnection* connection = nullptr;
    GDBusNodeInfo* itemInfo = nullptr;
    GDBusNodeInfo* menuInfo = nullptr;
    guint itemRegistration = 0;
    guint menuRegistration = 0;
    guint ownId = 0;
    guint watcherWatch = 0;
    bool nameAcquired = false;
    std::string busName;
    GSettings* sources = nullptr;
    gulong sourcesHandler = 0;
    // 環境変数 IROHA_TRAY_ICON（-1 なら無し）
    int forced = -1;
    std::vector<menu::Item> items;
    std::string itemsSignature;
    std::string symbol = menu::ModeSymbol(false);
    guint32 revision = 1;

    void Log(const std::string& message) const {
        if (log) log(message);
    }

    // ---- 出すかどうか ----

    bool ShouldShow() const {
        if (forced >= 0) return forced == 1;
        if (!sources) return false;
        std::vector<std::pair<std::string, std::string>> list;
        GVariant* value = g_settings_get_value(sources, "sources");
        GVariantIter iter;
        g_variant_iter_init(&iter, value);
        const gchar* type = nullptr;
        const gchar* name = nullptr;
        while (g_variant_iter_next(&iter, "(&s&s)", &type, &name)) list.emplace_back(type, name);
        g_variant_unref(value);
        return OnlyIrohaInputSource(list);
    }

    void Evaluate() {
        if (!connection) return;
        const bool show = ShouldShow();
        if (show && ownId == 0) {
            Log("tray icon: show");
            ownId = g_bus_own_name_on_connection(connection, busName.c_str(), G_BUS_NAME_OWNER_FLAGS_NONE,
                                                 NameAcquired, NameLost, this, nullptr);
        } else if (!show && ownId != 0) {
            Log("tray icon: hide");
            g_bus_unown_name(ownId); // 名前が消えると、受け手がアイコンを外す
            ownId = 0;
            nameAcquired = false;
        }
    }

    // 受け手（StatusNotifierWatcher）にアイコンを知らせる
    void Register() {
        if (!connection || !nameAcquired) return;
        g_dbus_connection_call(connection, kWatcherName, kWatcherPath, kWatcherName, "RegisterStatusNotifierItem",
                               g_variant_new("(s)", busName.c_str()), nullptr, G_DBUS_CALL_FLAGS_NONE, -1, nullptr,
                               Registered, this);
    }

    static void Registered(GObject* source, GAsyncResult* result, gpointer data) {
        GError* error = nullptr;
        GVariant* reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), result, &error);
        if (reply) {
            g_variant_unref(reply);
        } else {
            static_cast<Impl*>(data)->Log(std::string("tray icon: cannot register (") + (error ? error->message : "") +
                                          ")");
            if (error) g_error_free(error);
        }
    }

    static void NameAcquired(GDBusConnection*, const gchar*, gpointer data) {
        Impl* impl = static_cast<Impl*>(data);
        impl->nameAcquired = true;
        impl->Register();
    }

    static void NameLost(GDBusConnection*, const gchar*, gpointer data) {
        static_cast<Impl*>(data)->nameAcquired = false;
    }

    // GNOME Shell が起動し直したときなど、受け手が現れたら知らせ直す
    static void WatcherAppeared(GDBusConnection*, const gchar*, const gchar*, gpointer data) {
        static_cast<Impl*>(data)->Register();
    }

    static void SourcesChanged(GSettings*, gchar*, gpointer data) { static_cast<Impl*>(data)->Evaluate(); }

    // ---- アイコン（StatusNotifierItem） ----

    GVariant* ItemProperty(const std::string& name) const {
        if (name == "Category") return g_variant_new_string("ApplicationStatus");
        if (name == "Id") return g_variant_new_string("iroha");
        if (name == "Title") return g_variant_new_string("iroha");
        if (name == "Status") return g_variant_new_string("Active");
        if (name == "WindowId") return g_variant_new_int32(0);
        if (name == "IconPixmap") return IconPixmaps(symbol);
        if (name == "OverlayIconPixmap" || name == "AttentionIconPixmap") return EmptyPixmaps();
        if (name == "IconThemePath" || name == "IconName" || name == "OverlayIconName" ||
            name == "AttentionIconName" || name == "AttentionMovieName") {
            return g_variant_new_string("");
        }
        if (name == "ToolTip") {
            const std::string text = symbol == menu::ModeSymbol(true) ? "英数" : "かな";
            return g_variant_new("(s@a(iiay)ss)", "", EmptyPixmaps(), "iroha", text.c_str());
        }
        if (name == "ItemIsMenu") return g_variant_new_boolean(TRUE);
        if (name == "Menu") return g_variant_new_object_path(kMenuPath);
        return nullptr;
    }

    static GVariant* GetItemProperty(GDBusConnection*, const gchar*, const gchar*, const gchar*, const gchar* name,
                                     GError** error, gpointer data) {
        GVariant* value = static_cast<Impl*>(data)->ItemProperty(name);
        if (!value) g_set_error(error, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_PROPERTY, "unknown property %s", name);
        return value;
    }

    // アイコンを押したときなど。メニューのアイコン（ItemIsMenu）なので受け手がメニューを開く。何もしない
    static void CallItemMethod(GDBusConnection*, const gchar*, const gchar*, const gchar*, const gchar*, GVariant*,
                               GDBusMethodInvocation* invocation, gpointer) {
        g_dbus_method_invocation_return_value(invocation, nullptr);
    }

    // ---- メニュー（dbusmenu。項目の番号は items の添字 + 1、0 は根） ----

    GVariant* Properties(int id) const {
        GVariantBuilder builder;
        g_variant_builder_init(&builder, G_VARIANT_TYPE("a{sv}"));
        if (id == 0) {
            g_variant_builder_add(&builder, "{sv}", "children-display", g_variant_new_string("submenu"));
            return g_variant_builder_end(&builder);
        }
        const menu::Item& item = items[static_cast<size_t>(id - 1)];
        using Type = menu::Item::Type;
        if (item.type == Type::Separator) {
            g_variant_builder_add(&builder, "{sv}", "type", g_variant_new_string("separator"));
            return g_variant_builder_end(&builder);
        }
        g_variant_builder_add(&builder, "{sv}", "label", g_variant_new_string(MenuLabel(item.label).c_str()));
        g_variant_builder_add(&builder, "{sv}", "enabled", g_variant_new_boolean(item.type != Type::Status));
        if (item.type == Type::Radio || item.type == Type::Toggle) {
            g_variant_builder_add(&builder, "{sv}", "toggle-type",
                                  g_variant_new_string(item.type == Type::Radio ? "radio" : "checkmark"));
            g_variant_builder_add(&builder, "{sv}", "toggle-state", g_variant_new_int32(item.checked ? 1 : 0));
        }
        if (item.command == menu::Command::Settings) {
            g_variant_builder_add(&builder, "{sv}", "icon-name", g_variant_new_string("preferences-system"));
        }
        return g_variant_builder_end(&builder);
    }

    bool ValidId(int id) const { return id >= 0 && id <= static_cast<int>(items.size()); }

    GVariant* Layout(int id, bool withChildren) const {
        GVariantBuilder children;
        g_variant_builder_init(&children, G_VARIANT_TYPE("av"));
        if (id == 0 && withChildren) {
            for (size_t i = 0; i < items.size(); ++i) {
                g_variant_builder_add(&children, "v", Layout(static_cast<int>(i + 1), false));
            }
        }
        return g_variant_new("(i@a{sv}@av)", id, Properties(id), g_variant_builder_end(&children));
    }

    // 項目が押された。D-Bus の返事を先に返すため、動かすのは main loop に戻ってから
    void Clicked(int id) {
        if (id <= 0 || !ValidId(id)) return;
        const menu::Item& item = items[static_cast<size_t>(id - 1)];
        if (item.command == menu::Command::None) return;
        struct Pending {
            Impl* impl;
            std::string key;
        };
        g_idle_add(
            [](gpointer data) -> gboolean {
                auto* pending = static_cast<Pending*>(data);
                if (pending->impl->activate) pending->impl->activate(pending->key);
                delete pending;
                return G_SOURCE_REMOVE;
            },
            new Pending{this, item.key});
    }

    static void CallMenuMethod(GDBusConnection*, const gchar*, const gchar*, const gchar*, const gchar* method,
                               GVariant* parameters, GDBusMethodInvocation* invocation, gpointer data) {
        Impl* impl = static_cast<Impl*>(data);
        const std::string name = method;
        if (name == "GetLayout") {
            gint32 parent = 0;
            gint32 depth = -1;
            g_variant_get(parameters, "(ii^a&s)", &parent, &depth, nullptr);
            if (!impl->ValidId(parent)) {
                g_dbus_method_invocation_return_error(invocation, G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS,
                                                      "unknown id %d", parent);
                return;
            }
            g_dbus_method_invocation_return_value(
                invocation, g_variant_new("(u@(ia{sv}av))", impl->revision, impl->Layout(parent, depth != 0)));
        } else if (name == "GetGroupProperties") {
            GVariantIter* ids = nullptr;
            g_variant_get(parameters, "(ai^a&s)", &ids, nullptr);
            GVariantBuilder builder;
            g_variant_builder_init(&builder, G_VARIANT_TYPE("a(ia{sv})"));
            gint32 id = 0;
            bool any = false;
            while (g_variant_iter_next(ids, "i", &id)) {
                any = true;
                if (impl->ValidId(id)) g_variant_builder_add(&builder, "(i@a{sv})", id, impl->Properties(id));
            }
            g_variant_iter_free(ids);
            if (!any) { // 番号を渡さなければすべて
                for (int i = 0; i <= static_cast<int>(impl->items.size()); ++i) {
                    g_variant_builder_add(&builder, "(i@a{sv})", i, impl->Properties(i));
                }
            }
            g_dbus_method_invocation_return_value(invocation, g_variant_new("(a(ia{sv}))", &builder));
        } else if (name == "GetProperty") {
            gint32 id = 0;
            const gchar* property = nullptr;
            g_variant_get(parameters, "(i&s)", &id, &property);
            GVariant* value = nullptr;
            if (impl->ValidId(id)) {
                GVariant* properties = impl->Properties(id);
                value = g_variant_lookup_value(properties, property, nullptr);
                g_variant_unref(g_variant_ref_sink(properties));
            }
            if (!value) {
                g_dbus_method_invocation_return_error(invocation, G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS,
                                                      "no property %s on %d", property, id);
                return;
            }
            g_dbus_method_invocation_return_value(invocation, g_variant_new("(v)", value));
            g_variant_unref(value);
        } else if (name == "Event") {
            gint32 id = 0;
            const gchar* event = nullptr;
            g_variant_get(parameters, "(i&svu)", &id, &event, nullptr, nullptr);
            if (std::strcmp(event, "clicked") == 0) impl->Clicked(id);
            g_dbus_method_invocation_return_value(invocation, nullptr);
        } else if (name == "EventGroup") {
            GVariantIter* events = nullptr;
            g_variant_get(parameters, "(a(isvu))", &events);
            gint32 id = 0;
            const gchar* event = nullptr;
            GVariant* eventData = nullptr;
            guint32 timestamp = 0;
            while (g_variant_iter_loop(events, "(i&svu)", &id, &event, &eventData, &timestamp)) {
                if (std::strcmp(event, "clicked") == 0) impl->Clicked(id);
            }
            g_variant_iter_free(events);
            g_dbus_method_invocation_return_value(invocation, g_variant_new("(ai)", nullptr));
        } else if (name == "AboutToShow") {
            g_dbus_method_invocation_return_value(invocation, g_variant_new("(b)", FALSE));
        } else if (name == "AboutToShowGroup") {
            g_dbus_method_invocation_return_value(invocation, g_variant_new("(aiai)", nullptr, nullptr));
        } else {
            g_dbus_method_invocation_return_error(invocation, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_METHOD,
                                                  "unknown method %s", method);
        }
    }

    static GVariant* GetMenuProperty(GDBusConnection*, const gchar*, const gchar*, const gchar*, const gchar* name,
                                     GError** error, gpointer) {
        const std::string property = name;
        if (property == "Version") return g_variant_new_uint32(3);
        if (property == "TextDirection") return g_variant_new_string("ltr");
        if (property == "Status") return g_variant_new_string("normal");
        if (property == "IconThemePath") return g_variant_new_strv(nullptr, 0);
        g_set_error(error, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_PROPERTY, "unknown property %s", name);
        return nullptr;
    }

    void Emit(const char* path, const char* interface, const char* signal, GVariant* parameters) {
        if (!connection) return;
        g_dbus_connection_emit_signal(connection, nullptr, path, interface, signal, parameters, nullptr);
    }
};

TrayIcon::TrayIcon(Activate activate, Logger log) : impl_(std::make_unique<Impl>()) {
    impl_->activate = std::move(activate);
    impl_->log = std::move(log);
}

TrayIcon::~TrayIcon() {
    Impl& impl = *impl_;
    if (impl.sources) {
        if (impl.sourcesHandler) g_signal_handler_disconnect(impl.sources, impl.sourcesHandler);
        g_object_unref(impl.sources);
    }
    if (impl.ownId) g_bus_unown_name(impl.ownId);
    if (impl.watcherWatch) g_bus_unwatch_name(impl.watcherWatch);
    if (impl.connection) {
        if (impl.itemRegistration) g_dbus_connection_unregister_object(impl.connection, impl.itemRegistration);
        if (impl.menuRegistration) g_dbus_connection_unregister_object(impl.connection, impl.menuRegistration);
        g_object_unref(impl.connection);
    }
    if (impl.itemInfo) g_dbus_node_info_unref(impl.itemInfo);
    if (impl.menuInfo) g_dbus_node_info_unref(impl.menuInfo);
}

void TrayIcon::Start() {
    Impl& impl = *impl_;
    if (impl.connection) return;
    if (const char* forced = std::getenv("IROHA_TRAY_ICON")) {
        impl.forced = std::strcmp(forced, "1") == 0 ? 1 : 0;
    }
    GError* error = nullptr;
    impl.connection = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, &error);
    if (!impl.connection) {
        impl.Log(std::string("tray icon: no session bus (") + (error ? error->message : "") + ")");
        if (error) g_error_free(error);
        return;
    }
    impl.itemInfo = g_dbus_node_info_new_for_xml(kItemXml, nullptr);
    impl.menuInfo = g_dbus_node_info_new_for_xml(kMenuXml, nullptr);
    static const GDBusInterfaceVTable itemTable = {Impl::CallItemMethod, Impl::GetItemProperty, nullptr, {}};
    static const GDBusInterfaceVTable menuTable = {Impl::CallMenuMethod, Impl::GetMenuProperty, nullptr, {}};
    impl.itemRegistration = g_dbus_connection_register_object(
        impl.connection, kItemPath, g_dbus_node_info_lookup_interface(impl.itemInfo, kItemInterface), &itemTable,
        &impl, nullptr, nullptr);
    impl.menuRegistration = g_dbus_connection_register_object(
        impl.connection, kMenuPath, g_dbus_node_info_lookup_interface(impl.menuInfo, kMenuInterface), &menuTable,
        &impl, nullptr, nullptr);
    impl.busName = "org.kde.StatusNotifierItem-" + std::to_string(getpid()) + "-1";
    impl.watcherWatch = g_bus_watch_name_on_connection(impl.connection, kWatcherName, G_BUS_NAME_WATCHER_FLAGS_NONE,
                                                       Impl::WatcherAppeared, nullptr, &impl, nullptr);
    // GNOME の入力ソースの一覧（GNOME でなければ設定の型が無いので、環境変数で決めない限り出さない）
    if (impl.forced < 0) {
        if (GSettingsSchemaSource* schemas = g_settings_schema_source_get_default()) {
            if (GSettingsSchema* schema = g_settings_schema_source_lookup(schemas, kSourcesSchema, TRUE)) {
                g_settings_schema_unref(schema);
                impl.sources = g_settings_new(kSourcesSchema);
                impl.sourcesHandler =
                    g_signal_connect(impl.sources, "changed::sources", G_CALLBACK(Impl::SourcesChanged), &impl);
            }
        }
    }
    impl.Evaluate();
}

void TrayIcon::Update(const std::vector<menu::Item>& items, const std::string& symbol) {
    Impl& impl = *impl_;
    std::string signature;
    for (const auto& item : items) {
        signature += item.key + '\t' + item.label + (item.checked ? "\t1\n" : "\t0\n");
    }
    if (signature != impl.itemsSignature) {
        impl.itemsSignature = signature;
        impl.items = items;
        ++impl.revision;
        impl.Emit(kMenuPath, kMenuInterface, "LayoutUpdated", g_variant_new("(ui)", impl.revision, 0));
    }
    if (symbol != impl.symbol) {
        impl.symbol = symbol;
        impl.Emit(kItemPath, kItemInterface, "NewIcon", nullptr);
        impl.Emit(kItemPath, kItemInterface, "NewToolTip", nullptr);
    }
}

} // namespace iroha::ibus
