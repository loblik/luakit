local ui = ipc_channel("webview_wm")

local mousedown_cb = function (event, page_id)
    -- Only consider left-click
    if event.button ~= 0 then return end

    -- Only consider editable text inputs
    local elem = event.target
    local tag = elem.tag_name
    if tag ~= "INPUT" and tag ~= "TEXTAREA" then return end
    if tag == "INPUT" and (elem.attr.type or ""):lower() == "button" then return end
    if elem.attr.disabled or elem.attr.readonly then return end

    ui:emit_signal("form-active", page_id)
end

extension:add_signal("page-created", function(_, page)
    page:add_signal("document-loaded", function(p)
        local doc = dom_document(p.id)
        doc.body:add_event_listener("mousedown", true, function (e)
            mousedown_cb(e, p.id)
        end)
    end)
end)

-- vim: et:sw=4:ts=8:sts=4:tw=80