#include "dk64vr_menu_ui.h"

#include <cstring>
#include <memory>
#include <mutex>

#include "recompui/recompui.h"
#include "recompui/config.h"
#include "elements/ui_element.h"
#include "util/file.h"

#include "dk64vr_input.h"
#include "dk64vr_log.h"

namespace dk64vr::menu {

namespace {

class VrMenuRoot;

struct State {
    std::mutex m;
    recompui::ContextId ctx = recompui::ContextId::null();
    VrMenuRoot *root = nullptr;
    bool created = false;
    bool open = false;

    // Host to UI (latest frame, with presses held over until the UI has seen them).
    FrameIn in;
    uint32_t navSeen = 0;      // held bits not yet consumed
    bool downSeen = false;
    Facts facts;
    std::vector<std::string> acts;
    std::string locate;

    // UI side (only touched under m by the UI thread once open).
    dk64vr_settings_t settings = {};
    std::unique_ptr<StructSettings> access;
    std::unique_ptr<Model> model;

    FrameOut out;
};

State &state() {
    static State s;
    return s;
}

bool isCovering() {
    // Over another recompui menu (the recomp config menu or the launcher), the panel is not
    // transparent around the window: the menu under it would show.
    return recompui::is_context_shown(recompui::config::get_config_context_id()) || recompui::is_context_shown(recompui::get_launcher_context_id());
}

Rml::Element *findAct(Rml::Element *e, const std::string &act) {
    if (e == nullptr) {
        return nullptr;
    }

    if (e->GetAttribute<Rml::String>("data-act", Rml::String()) == act) {
        return e;
    }

    for (int i = 0; i < e->GetNumChildren(); i++) {
        if (Rml::Element *found = findAct(e->GetChild(i), act)) {
            return found;
        }
    }

    return nullptr;
}

class VrMenuRoot : public recompui::Element {
public:
    VrMenuRoot(recompui::ResourceId rid, recompui::Element *parent) : recompui::Element(rid, parent, 0, "div", true) {}

protected:
    std::string_view get_type_name() override { return "VrMenuRoot"; }

    void process_event(const recompui::Event &e) override {
        if (e.type != recompui::EventType::Update) {
            return;
        }

        State &s = state();
        std::scoped_lock lock(s.m);
        if (!s.open || !s.model) {
            return;
        }

        Rml::ElementDocument *doc = s.ctx.get_document();
        Model &model = *s.model;

        // Layout of the document as last drawn.
        Layout layout;
        Rml::Element *list = doc->GetElementById("list");
        Rml::Element *listIn = doc->GetElementById("listin");
        if ((list != nullptr) && (listIn != nullptr)) {
            layout.valid = true;
            layout.viewHeight = list->GetClientHeight();
            layout.contentHeight = listIn->GetOffsetHeight();
            if (Rml::Element *focus = doc->GetElementById("focus")) {
                layout.focusValid = true;
                layout.focusTop = focus->GetAbsoluteTop() - listIn->GetAbsoluteTop();
                layout.focusBottom = layout.focusTop + focus->GetOffsetHeight();
            }
        }

        // The pointer against what is under it.
        Pointer pointer;
        const FrameIn in = s.in;
        pointer.on = in.pointerOn;
        pointer.down = in.pointerDown || s.downSeen;
        pointer.x = in.px;
        pointer.y = in.py;
        if (in.pointerOn) {
            Rml::Context *rc = doc->GetContext();
            Rml::Element *hit = (rc != nullptr) ? rc->GetElementAtPoint(Rml::Vector2f(in.px, in.py)) : nullptr;
            for (Rml::Element *el = hit; el != nullptr; el = el->GetParentNode()) {
                if (el->GetOwnerDocument() != doc) {
                    break;
                }

                const Rml::String act = el->GetAttribute<Rml::String>("data-act", Rml::String());
                if (!act.empty()) {
                    pointer.act = act;
                    if (act.rfind("slider:", 0) == 0) {
                        pointer.trackLeft = el->GetAbsoluteLeft() + el->GetClientLeft();
                        pointer.trackWidth = el->GetClientWidth();
                    }
                    break;
                }
            }
        }

        Input input;
        input.nowS = in.nowS;
        input.navHeld = in.navHeld | s.navSeen;
        input.scroll = in.scroll;
        s.navSeen = 0;
        s.downSeen = false;
        model.setFacts(s.facts);
        Effects fx = model.step(input, pointer, layout);
        for (const std::string &act : s.acts) {
            const Effects more = model.act(act, in.nowS);
            fx.recentre = fx.recentre || more.recentre;
            fx.close = fx.close || more.close;
        }
        s.acts.clear();

        FrameOut &out = s.out;
        for (StructSettings::Change &c : s.access->changes) {
            out.changes.push_back(std::move(c));
        }
        s.access->changes.clear();
        out.recentre = out.recentre || fx.recentre;
        out.close = out.close || fx.close;
        out.hover = out.hover || fx.hover;
        out.screen = model.screen();
        out.tab = model.tab();
        out.focus = model.focusedAct();

        if (!s.locate.empty()) {
            if (Rml::Element *el = findAct(doc, s.locate)) {
                out.locateValid = true;
                out.locateX = el->GetAbsoluteLeft() + 0.5f * el->GetOffsetWidth();
                out.locateY = el->GetAbsoluteTop() + 0.5f * el->GetOffsetHeight();
            }
            else {
                out.locateValid = false;
            }
        }

        if (model.dirty) {
            model.dirty = false;
            set_text_unsafe(model.render(isCovering()));
            out.rendered = true;
        }
    }
};

}

void uiCreate() {
    State &s = state();
    if (s.created) {
        return;
    }

    const std::filesystem::path path = recompui::file::get_asset_path("vr_menu.rml");
    if (!std::filesystem::exists(path)) {
        dk64vr_logf("menu: %s is missing; the VR settings menu is off", path.string().c_str());
        return;
    }

    // The launcher's context is open on this thread during its init callback.
    recompui::ContextId prev = recompui::try_close_current_context();
    s.ctx = recompui::create_context(path);
    s.ctx.open();
    s.root = s.ctx.create_element<VrMenuRoot>(s.ctx.get_root_element());
    s.ctx.set_captures_input(true);
    s.ctx.set_captures_mouse(false);  // the host feeds the pointer itself (no SDL mouse events)
    s.ctx.close();
    if (prev != recompui::ContextId::null()) {
        prev.open();
    }

    s.created = true;
    dk64vr_logf("menu: VR settings document ready (%s)", path.string().c_str());
}

bool uiReady() {
    return state().created;
}

bool uiOpen(const dk64vr_settings_t &settings, const Facts &facts, double nowS, uint32_t navHeld) {
    State &s = state();
    if (!s.created) {
        return false;
    }

    {
        std::scoped_lock lock(s.m);
        s.settings = settings;
        s.settings.raw = nullptr;  // the menu's copy never formats or saves the file
        s.settings.raw_len = 0;
        s.access = std::make_unique<StructSettings>(&s.settings);
        s.model = std::make_unique<Model>(*s.access, facts);
        s.model->open(nowS, navHeld);
        s.facts = facts;
        s.in = FrameIn();
        s.in.nowS = nowS;
        s.in.navHeld = navHeld;
        s.navSeen = 0;
        s.downSeen = false;
        s.acts.clear();
        s.out = FrameOut();
        s.open = true;
    }

    recompui::show_context(s.ctx, "");
    s.ctx.open();
    s.root->queue_update();
    s.ctx.close();
    return true;
}

void uiClose() {
    State &s = state();
    bool wasOpen;
    {
        std::scoped_lock lock(s.m);
        wasOpen = s.open;
        s.open = false;
    }

    if (wasOpen && recompui::is_context_shown(s.ctx)) {
        recompui::hide_context(s.ctx);
    }
}

bool uiIsOpen() {
    State &s = state();
    std::scoped_lock lock(s.m);
    return s.open;
}

void uiFrame(const FrameIn &in, const Facts &facts) {
    State &s = state();
    {
        std::scoped_lock lock(s.m);
        if (!s.open) {
            return;
        }

        s.navSeen |= in.navHeld;  // a press shorter than a UI frame still counts
        s.downSeen = s.downSeen || in.pointerDown;
        s.in = in;
        s.facts = facts;
    }

    s.ctx.open();
    s.root->queue_update();
    s.ctx.close();
}

FrameOut uiTake() {
    State &s = state();
    std::scoped_lock lock(s.m);
    FrameOut out = std::move(s.out);
    s.out = FrameOut();
    s.out.screen = out.screen;
    s.out.tab = out.tab;
    s.out.focus = out.focus;
    s.out.locateValid = out.locateValid;
    s.out.locateX = out.locateX;
    s.out.locateY = out.locateY;
    return out;
}

void uiAct(const std::string &act) {
    State &s = state();
    std::scoped_lock lock(s.m);
    s.acts.push_back(act);
}

void uiLocate(const std::string &act) {
    State &s = state();
    std::scoped_lock lock(s.m);
    s.locate = act;
    if (act.empty()) {
        s.out.locateValid = false;
    }
}

}
