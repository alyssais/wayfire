#include <wayfire/singleton-plugin.hpp>
#include <wayfire/output.hpp>
#include <wayfire/signal-definitions.hpp>
#include <wayfire/render-manager.hpp>
#include <wayfire/workspace-manager.hpp>
#include <wayfire/debug.hpp>
#include <type_traits>
#include <map>
#include <wayfire/core.hpp>
#include "system_fade.hpp"
#include "basic_animations.hpp"
#include "fire/fire.hpp"

#include "../matcher/matcher.hpp"

void animation_base::init(wayfire_view, int, wf_animation_type) {}
bool animation_base::step() {return false;}
animation_base::~animation_base() {}

struct animation_list_t
{
    /**
     * A list of views with their respective outputs which are being animated.
     * Note that the output value is just the output the view was last animated
     * on, so it may be a dangling pointer.
     */
    std::map<wayfire_view, wf::output_t*> last_output;
};

static constexpr const char* custom_data_id = "animation-hook";

/* Represents an animation running for a specific view
 * animation_t is which animation to use (i.e fire, zoom, etc). */
struct animation_hook_base : public wf::custom_data_t
{
    virtual void stop_hook(bool) = 0;
    virtual ~animation_hook_base() = default;
};

template<class animation_t>
struct animation_hook : public animation_hook_base
{
    static_assert(std::is_base_of<animation_base, animation_t>::value,
            "animation_type must be derived from animation_base!");

    wf_animation_type type;
    wayfire_view view;
    wf::output_t *current_output = nullptr;
    std::unique_ptr<animation_base> animation;

    /* Update animation right before each frame */
    wf::effect_hook_t update_animation_hook = [=] ()
    {
        view->damage();
        bool result = animation->step();
        view->damage();

        if (!result)
            stop_hook(false);
    };

    /**
     * Switch the output the view is being animated on, and update the lastly
     * animated output in the global list.
     */
    void set_output(wf::output_t *new_output)
    {
        if (current_output)
            current_output->render->rem_effect(&update_animation_hook);

        if (new_output)
        {
            new_output->render->add_effect(&update_animation_hook,
                wf::OUTPUT_EFFECT_PRE);
            auto& global_list = wf::get_core().get_data<
                wf::detail::singleton_data_t<animation_list_t>>()->ptr;
            global_list.last_output[view] = new_output;
        }

        current_output = new_output;
    }

    wf::signal_connection_t on_set_output = {[this] (wf::signal_data_t *data)
    { set_output(get_signaled_output(data)); }};

    animation_hook(wayfire_view view, int duration, wf_animation_type type)
    {
        this->type = type;
        this->view = view;
        if (type == ANIMATION_TYPE_UNMAP)
        {
            view->take_ref();
            view->take_snapshot();
        }

        animation = std::make_unique<animation_t> ();
        animation->init(view, duration, type);

        set_output(view->get_output());
        /* Animation is driven by the output render cycle the view is on.
         * Thus, we need to keep in sync with the current output. */
        view->connect_signal("set-output", &on_set_output);
    }

    void stop_hook(bool detached) override
    {
        /* We don't want to change the state of the view if it was detached */
        if (type == ANIMATION_TYPE_MINIMIZE && !detached)
            view->set_minimized(true);

        /* Will also delete this */
        view->erase_data(custom_data_id);
    }

    ~animation_hook()
    {
        /**
         * Order here is very important.
         * After doing unref() the view will be potentially destroyed.
         * Hence, we want to deinitialize everything else before that.
         */
        set_output(nullptr);
        on_set_output.disconnect();
        this->animation.reset();

        // remove from list
        auto& global_list = wf::get_core().get_data<
            wf::detail::singleton_data_t<animation_list_t>>()->ptr;
        global_list.last_output.erase(view);

        if (type == ANIMATION_TYPE_UNMAP)
            view->unref();
    }
};

class wayfire_animation : public wf::singleton_plugin_t<animation_list_t, true>
{
    wf::option_wrapper_t<std::string> open_animation{"animate/open_animation"};
    wf::option_wrapper_t<std::string> close_animation{"animate/close_animation"};

    wf::option_wrapper_t<int> default_duration{"animate/duration"};
    wf::option_wrapper_t<int> fade_duration{"animate/fade_duration"};
    wf::option_wrapper_t<int> zoom_duration{"animate/zoom_duration"};
    wf::option_wrapper_t<int> fire_duration{"animate/fire_duration"};

    wf::option_wrapper_t<int> startup_duration{"animate/startup_duration"};
    wf::option_wrapper_t<std::string> animation_enabled_for{"animate/enabled_for"};
    wf::option_wrapper_t<std::string> fade_enabled_for{"animate/fade_enabled_for"};
    wf::option_wrapper_t<std::string> zoom_enabled_for{"animate/zoom_enabled_for"};
    wf::option_wrapper_t<std::string> fire_enabled_for{"animate/fire_enabled_for"};

    std::unique_ptr<wf::matcher::view_matcher> animation_enabled_matcher,
        fade_enabled_matcher, zoom_enabled_matcher, fire_enabled_matcher;

    public:
    void init() override
    {
        singleton_plugin_t::init();

        grab_interface->name = "animate";
        grab_interface->capabilities = 0;

        output->connect_signal("map-view", &on_view_mapped);
        output->connect_signal("pre-unmap-view", &on_view_unmapped);
        output->connect_signal("start-rendering", &on_render_start);
        output->connect_signal("view-minimize-request", &on_minimize_request);

        animation_enabled_matcher =
            wf::matcher::get_matcher(animation_enabled_for);
        fade_enabled_matcher = wf::matcher::get_matcher(fade_enabled_for);
        zoom_enabled_matcher = wf::matcher::get_matcher(zoom_enabled_for);
        fire_enabled_matcher = wf::matcher::get_matcher(fire_enabled_for);
    }

    struct view_animation_t
    {
        std::string animation_name;
        int duration;
    };

    view_animation_t get_animation_for_view(
        wf::option_wrapper_t<std::string>& anim_type, wayfire_view view)
    {
        /* Determine the animation for the given view.
         * Note that the matcher plugin might not have been loaded, so
         * we need to have a fallback algorithm */
        if (animation_enabled_matcher)
        {
            if (wf::matcher::evaluate(fade_enabled_matcher, view))
                return {"fade", fade_duration};
            if (wf::matcher::evaluate(zoom_enabled_matcher, view))
                return {"zoom", zoom_duration};
            if (wf::matcher::evaluate(fire_enabled_matcher, view))
                return {"fire", fire_duration};
            if (wf::matcher::evaluate(animation_enabled_matcher, view))
                return {anim_type, default_duration};
        }
        else if (view->role == wf::VIEW_ROLE_TOPLEVEL ||
            (view->role == wf::VIEW_ROLE_UNMANAGED && view->is_focuseable()))
        {
            return {anim_type, default_duration};
        }

        return {"none", 0};
    }

    template<class animation_t>
        void set_animation(wayfire_view view,
            wf_animation_type type, int duration)
    {
        view->store_data(
            std::make_unique<animation_hook<animation_t>> (view, duration, type),
            custom_data_id);
    }

    /* TODO: enhance - add more animations */
    wf::signal_callback_t on_view_mapped =
        [=] (wf::signal_data_t *ddata) -> void
    {
        auto view = get_signaled_view(ddata);
        auto animation = get_animation_for_view(open_animation, view);

        if (animation.animation_name == "fade")
            set_animation<fade_animation> (view, ANIMATION_TYPE_MAP, animation.duration);
        else if (animation.animation_name == "zoom")
            set_animation<zoom_animation> (view, ANIMATION_TYPE_MAP, animation.duration);
        else if (animation.animation_name == "fire")
            set_animation<FireAnimation> (view, ANIMATION_TYPE_MAP, animation.duration);
    };

    wf::signal_callback_t on_view_unmapped = [=] (wf::signal_data_t *data)
    {
        auto view = get_signaled_view(data);
        auto animation = get_animation_for_view(close_animation, view);

        if (animation.animation_name == "fade")
            set_animation<fade_animation> (view, ANIMATION_TYPE_UNMAP, animation.duration);
        else if (animation.animation_name == "zoom")
            set_animation<zoom_animation> (view, ANIMATION_TYPE_UNMAP, animation.duration);
        else if (animation.animation_name == "fire")
            set_animation<FireAnimation> (view, ANIMATION_TYPE_UNMAP, animation.duration);
    };

    wf::signal_callback_t on_minimize_request = [=] (wf::signal_data_t *data)
    {
        auto ev = static_cast<view_minimize_request_signal*> (data);
        if (ev->state)
        {
            ev->carried_out = true;
            set_animation<zoom_animation>(
                ev->view, ANIMATION_TYPE_MINIMIZE, default_duration);
        }
        else
        {
            set_animation<zoom_animation>(
                ev->view, ANIMATION_TYPE_RESTORE, default_duration);
        }
    };

    wf::signal_callback_t on_render_start = [=] (wf::signal_data_t *data)
    {
        new wf_system_fade(output, startup_duration);
    };

    void fini() override
    {
        output->disconnect_signal("map-view", &on_view_mapped);
        output->disconnect_signal("pre-unmap-view", &on_view_unmapped);
        output->disconnect_signal("start-rendering", &on_render_start);
        output->disconnect_signal("view-minimize-request", &on_minimize_request);

        /* Clear up all active animations on the current output */
        auto list = this->get_instance().last_output;
        for (auto pair : list)
        {
            if (pair.second == this->output)
            {
                pair.first->get_data<animation_hook_base>(custom_data_id)
                    ->stop_hook(true);
            }
        }

        singleton_plugin_t::fini();
    }
};

DECLARE_WAYFIRE_PLUGIN(wayfire_animation);
