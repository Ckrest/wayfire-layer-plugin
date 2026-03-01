#include <algorithm>
#include <cstdint>
#include <optional>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#include <wayfire/core.hpp>
#include <wayfire/option-wrapper.hpp>
#include <wayfire/plugin.hpp>
#include <wayfire/plugins/common/shared-core-data.hpp>
#include <wayfire/plugins/ipc/ipc-helpers.hpp>
#include <wayfire/plugins/ipc/ipc-method-repository.hpp>
#include <wayfire/scene-operations.hpp>
#include <wayfire/signal-definitions.hpp>
#include <wayfire/util/log.hpp>
#include <wayfire/view.hpp>

static std::string trim_copy(const std::string& value)
{
    auto start = value.find_first_not_of(" \t");
    if (start == std::string::npos)
    {
        return "";
    }

    auto end = value.find_last_not_of(" \t");
    return value.substr(start, end - start + 1);
}

static std::vector<std::string> parse_namespaces(const std::string& raw)
{
    std::vector<std::string> result;
    std::istringstream ss(raw);
    std::string item;
    while (std::getline(ss, item, ';'))
    {
        auto trimmed = trim_copy(item);
        if (!trimmed.empty())
        {
            result.push_back(trimmed);
        }
    }

    return result;
}

static std::unordered_map<std::string, int> parse_priorities(const std::string& raw)
{
    std::unordered_map<std::string, int> result;
    std::istringstream ss(raw);
    std::string item;
    while (std::getline(ss, item, ';'))
    {
        auto trimmed = trim_copy(item);
        if (trimmed.empty())
        {
            continue;
        }

        auto sep = trimmed.find(':');
        if (sep == std::string::npos)
        {
            continue;
        }

        auto ns = trim_copy(trimmed.substr(0, sep));
        auto pr = trim_copy(trimmed.substr(sep + 1));
        if (ns.empty() || pr.empty())
        {
            continue;
        }

        try
        {
            result[ns] = std::stoi(pr);
        } catch (...)
        {
            LOGW("topmost-scene: invalid priority value '", pr, "' for namespace '", ns, "'");
        }
    }

    return result;
}

class topmost_scene_t : public wf::plugin_interface_t
{
    struct tracked_view_t
    {
        wayfire_view view;
        uint64_t insertion_order = 0;
        bool explicit_pin = false;
        std::optional<int> explicit_priority;
        bool auto_pin = false;
        std::string matched_namespace;
        std::weak_ptr<wf::scene::floating_inner_node_t> original_parent;
    };

  public:
    void init() override
    {
        wf::get_core().connect(&on_view_mapped);
        wf::get_core().connect(&on_view_pre_unmap);
        wf::get_core().connect(&on_view_focus_request);
        wf::get_core().connect(&on_view_geometry_changed);
        wf::get_core().scene()->connect(&on_root_node_updated);

        method_repository->register_method("topmost-scene/pin", ipc_pin);
        method_repository->register_method("topmost-scene/unpin", ipc_unpin);
        method_repository->register_method("topmost-scene/list", ipc_list);
        method_repository->register_method("topmost-scene/reapply", ipc_reapply);

        namespaces_opt.set_callback(namespaces_changed);
        priorities_opt.set_callback(priorities_changed);
        enabled_opt.set_callback(enabled_changed);

        reload_config();
        refresh_all_auto_memberships();
        if (enabled_opt)
        {
            reapply_order();
        }
    }

    void fini() override
    {
        method_repository->unregister_method("topmost-scene/pin");
        method_repository->unregister_method("topmost-scene/unpin");
        method_repository->unregister_method("topmost-scene/list");
        method_repository->unregister_method("topmost-scene/reapply");

        restore_all_to_original_parents();
        tracked_views.clear();
    }

  private:
    wf::option_wrapper_t<std::string> namespaces_opt{"topmost-scene/namespaces"};
    wf::option_wrapper_t<std::string> priorities_opt{"topmost-scene/priorities"};
    wf::option_wrapper_t<bool> enabled_opt{"topmost-scene/enabled"};

    wf::shared_data::ref_ptr_t<wf::ipc::method_repository_t> method_repository;

    std::vector<std::string> configured_namespaces;
    std::unordered_map<std::string, int> configured_priorities;
    std::vector<tracked_view_t> tracked_views;
    uint64_t next_insertion_order = 0;
    bool in_reapply = false;

    void reload_config()
    {
        configured_namespaces = parse_namespaces(namespaces_opt);
        configured_priorities = parse_priorities(priorities_opt);
    }

    std::optional<std::string> matching_namespace(wayfire_view view) const
    {
        if (!view)
        {
            return {};
        }

        auto app_id = view->get_app_id();
        for (const auto& ns : configured_namespaces)
        {
            if (app_id == ns)
            {
                return ns;
            }
        }

        return {};
    }

    tracked_view_t *find_tracked(wayfire_view view)
    {
        auto it = std::find_if(tracked_views.begin(), tracked_views.end(),
            [&] (const tracked_view_t& entry)
        {
            return entry.view == view;
        });

        if (it == tracked_views.end())
        {
            return nullptr;
        }

        return &*it;
    }

    tracked_view_t&ensure_tracked(wayfire_view view)
    {
        auto existing = find_tracked(view);
        if (existing)
        {
            return *existing;
        }

        tracked_view_t entry;
        entry.view = view;
        entry.insertion_order = next_insertion_order++;
        tracked_views.push_back(entry);
        return tracked_views.back();
    }

    int effective_priority(const tracked_view_t& entry) const
    {
        if (entry.explicit_priority.has_value())
        {
            return entry.explicit_priority.value();
        }

        auto it = configured_priorities.find(entry.matched_namespace);
        if (it != configured_priorities.end())
        {
            return it->second;
        }

        return 0;
    }

    void maybe_snapshot_original_parent(tracked_view_t& entry)
    {
        if (!entry.view || !entry.original_parent.expired())
        {
            return;
        }

        auto node = entry.view->get_root_node();
        if (!node || !node->parent())
        {
            return;
        }

        auto root = wf::get_core().scene().get();
        if (node->parent() == root)
        {
            return;
        }

        auto parent = dynamic_cast<wf::scene::floating_inner_node_t*>(node->parent());
        if (!parent)
        {
            return;
        }

        entry.original_parent = std::dynamic_pointer_cast<wf::scene::floating_inner_node_t>(
            parent->shared_from_this());
    }

    void restore_original_parent(tracked_view_t& entry)
    {
        if (!entry.view || !entry.view->is_mapped())
        {
            return;
        }

        auto node = entry.view->get_root_node();
        if (!node || !node->parent())
        {
            return;
        }

        auto root = wf::get_core().scene().get();
        if (node->parent() != root)
        {
            return;
        }

        auto parent = entry.original_parent.lock();
        if (!parent || (parent.get() == root))
        {
            return;
        }

        wf::scene::readd_front(parent, node);
    }

    void restore_all_to_original_parents()
    {
        for (auto& entry : tracked_views)
        {
            restore_original_parent(entry);
        }
    }

    void refresh_auto_state_for_view(wayfire_view view)
    {
        auto matched = matching_namespace(view);
        auto entry = find_tracked(view);
        if (matched.has_value())
        {
            if (!entry)
            {
                entry = &ensure_tracked(view);
            }

            entry->auto_pin = true;
            entry->matched_namespace = matched.value();
        } else if (entry && entry->auto_pin)
        {
            entry->auto_pin = false;
            entry->matched_namespace.clear();
            if (!entry->explicit_pin)
            {
                restore_original_parent(*entry);
                tracked_views.erase(std::remove_if(tracked_views.begin(), tracked_views.end(),
                    [&] (const tracked_view_t& candidate)
                {
                    return candidate.view == view;
                }), tracked_views.end());
            }
        }
    }

    void refresh_all_auto_memberships()
    {
        for (auto& entry : tracked_views)
        {
            entry.auto_pin = false;
            entry.matched_namespace.clear();
        }

        for (auto& view : wf::get_core().get_all_views())
        {
            if (view->is_mapped())
            {
                refresh_auto_state_for_view(view);
            }
        }

        std::vector<wayfire_view> to_remove;
        for (auto& entry : tracked_views)
        {
            if (!entry.auto_pin && !entry.explicit_pin)
            {
                to_remove.push_back(entry.view);
            }
        }

        for (auto& view : to_remove)
        {
            auto entry = find_tracked(view);
            if (entry)
            {
                restore_original_parent(*entry);
            }

            tracked_views.erase(std::remove_if(tracked_views.begin(), tracked_views.end(),
                [&] (const tracked_view_t& candidate)
            {
                return candidate.view == view;
            }), tracked_views.end());
        }
    }

    void reapply_order()
    {
        if (!enabled_opt || in_reapply)
        {
            return;
        }

        in_reapply = true;

        std::vector<tracked_view_t*> candidates;
        candidates.reserve(tracked_views.size());
        for (auto& entry : tracked_views)
        {
            if (entry.view && entry.view->is_mapped() &&
                entry.view->get_root_node() && entry.view->get_root_node()->parent())
            {
                candidates.push_back(&entry);
            }
        }

        std::sort(candidates.begin(), candidates.end(),
            [&] (const tracked_view_t *a, const tracked_view_t *b)
        {
            auto pa = effective_priority(*a);
            auto pb = effective_priority(*b);
            if (pa != pb)
            {
                return pa > pb;
            }

            return a->insertion_order < b->insertion_order;
        });

        auto root = wf::get_core().scene();
        for (auto it = candidates.rbegin(); it != candidates.rend(); ++it)
        {
            auto& entry = **it;
            maybe_snapshot_original_parent(entry);
            wf::scene::readd_front(root, entry.view->get_root_node());
        }

        in_reapply = false;
    }

    void remove_view(wayfire_view view, bool restore_parent)
    {
        auto entry = find_tracked(view);
        if (!entry)
        {
            return;
        }

        if (restore_parent)
        {
            restore_original_parent(*entry);
        }

        tracked_views.erase(std::remove_if(tracked_views.begin(), tracked_views.end(),
            [&] (const tracked_view_t& candidate)
        {
            return candidate.view == view;
        }), tracked_views.end());
    }

    wf::signal::connection_t<wf::view_mapped_signal> on_view_mapped = [=] (wf::view_mapped_signal *ev)
    {
        refresh_auto_state_for_view(ev->view);
        reapply_order();
    };

    wf::signal::connection_t<wf::view_pre_unmap_signal> on_view_pre_unmap = [=] (wf::view_pre_unmap_signal *ev)
    {
        remove_view(ev->view, false);
    };

    wf::signal::connection_t<wf::view_focus_request_signal> on_view_focus_request =
        [=] (wf::view_focus_request_signal*)
    {
        if (!tracked_views.empty())
        {
            reapply_order();
        }
    };

    wf::signal::connection_t<wf::view_geometry_changed_signal> on_view_geometry_changed =
        [=] (wf::view_geometry_changed_signal*)
    {
        if (!tracked_views.empty())
        {
            reapply_order();
        }
    };

    wf::signal::connection_t<wf::scene::root_node_update_signal> on_root_node_updated =
        [=] (wf::scene::root_node_update_signal *ev)
    {
        if (in_reapply || !enabled_opt)
        {
            return;
        }

        if (!(ev->flags & wf::scene::update_flag::CHILDREN_LIST))
        {
            return;
        }

        if (!tracked_views.empty())
        {
            reapply_order();
        }
    };

    wf::config::option_base_t::updated_callback_t namespaces_changed = [=] ()
    {
        reload_config();
        refresh_all_auto_memberships();
        reapply_order();
    };

    wf::config::option_base_t::updated_callback_t priorities_changed = [=] ()
    {
        reload_config();
        reapply_order();
    };

    wf::config::option_base_t::updated_callback_t enabled_changed = [=] ()
    {
        if (enabled_opt)
        {
            reapply_order();
        } else
        {
            restore_all_to_original_parents();
        }
    };

    wf::ipc::method_callback ipc_pin = [=] (wf::json_t data) -> wf::json_t
    {
        auto view_id = wf::ipc::json_get_view_id(data);
        auto view    = wf::ipc::find_view_by_id(view_id);
        if (!view)
        {
            return wf::ipc::json_error("View not found");
        }

        auto& entry = ensure_tracked(view);
        entry.explicit_pin = true;

        auto explicit_prio = wf::ipc::json_get_optional_int64(data, "priority");
        if (explicit_prio.has_value())
        {
            entry.explicit_priority = (int)explicit_prio.value();
        } else if (!entry.explicit_priority.has_value())
        {
            entry.explicit_priority = 0;
        }

        reapply_order();
        return wf::ipc::json_ok();
    };

    wf::ipc::method_callback ipc_unpin = [=] (wf::json_t data) -> wf::json_t
    {
        auto view_id = wf::ipc::json_get_view_id(data);
        auto view    = wf::ipc::find_view_by_id(view_id);
        if (!view)
        {
            return wf::ipc::json_error("View not found");
        }

        auto entry = find_tracked(view);
        if (!entry)
        {
            return wf::ipc::json_error("View is not pinned");
        }

        entry->explicit_pin = false;
        entry->explicit_priority.reset();
        if (!entry->auto_pin)
        {
            remove_view(view, true);
        }

        reapply_order();
        return wf::ipc::json_ok();
    };

    wf::ipc::method_callback ipc_list = [=] (wf::json_t) -> wf::json_t
    {
        auto response = wf::json_t::array();
        for (const auto& entry : tracked_views)
        {
            if (!entry.view)
            {
                continue;
            }

            wf::json_t js_entry;
            js_entry["view_id"] = entry.view->get_id();
            js_entry["app_id"] = entry.view->get_app_id();
            js_entry["mapped"] = entry.view->is_mapped();
            js_entry["explicit_pin"] = entry.explicit_pin;
            js_entry["auto_pin"] = entry.auto_pin;
            js_entry["namespace"] = entry.matched_namespace;
            js_entry["priority"] = effective_priority(entry);
            response.append(js_entry);
        }

        return response;
    };

    wf::ipc::method_callback ipc_reapply = [=] (wf::json_t) -> wf::json_t
    {
        reapply_order();
        return wf::ipc::json_ok();
    };
};

DECLARE_WAYFIRE_PLUGIN(topmost_scene_t);
