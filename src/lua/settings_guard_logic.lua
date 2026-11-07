-- The settings guard's rules (menu state; no game calls, so tests run them in
-- plain Lua): which of the player's settings a server's Lua can change, and
-- what changed between the start of a session and its end.
--
-- Garry's Mod blocks server Lua from binds, volume, sensitivity, FOV, gamma
-- and a few others, but not from graphics, network, movement or HUD settings.
-- Saved ones (most of these) are written to config.cfg at quit, so a server's
-- "FPS booster" follows the player to every other server; the rest last until
-- the game closes. Garry's Mod's own menu checks 8 of them (problems.lua).
local Guard = {}

-- { convar, what it is }, in the order a change list shows them. Not here:
-- what the pack changes itself (cl_timeout, fps_max_nofocus, fps_max_menu,
-- voice_scale from the Escape card), what server Lua can't set, and choices
-- gamemodes make on purpose (player model, colours).
Guard.WATCH = {
	-- The 8 Garry's Mod's own menu checks.
	{ "mat_hdr_level", "HDR" },
	{ "mat_bumpmap", "Bump mapping" },
	{ "mat_specular", "Specular reflections" },
	{ "mat_viewportscale", "Render scale" },
	{ "voice_fadeouttime", "Voice fade-out" },
	{ "cl_forwardspeed", "Forward speed" },
	{ "cl_sidespeed", "Sideways speed" },
	{ "cl_backspeed", "Backward speed" },
	-- Graphics.
	{ "mat_picmip", "Texture detail" },
	{ "mat_antialias", "Anti-aliasing" },
	{ "mat_aaquality", "Anti-aliasing quality" },
	{ "mat_forceaniso", "Anisotropic filtering" },
	{ "mat_trilinear", "Trilinear filtering" },
	{ "mat_motion_blur_enabled", "Motion blur" },
	{ "mat_reducefillrate", "Reduced fill rate" },
	{ "mat_queue_mode", "Multicore rendering mode" },
	{ "mat_filterlightmaps", "Lightmap filtering" },
	{ "mat_filtertextures", "Texture filtering" },
	{ "mat_disable_bloom", "Bloom off" },
	{ "mat_bloomscale", "Bloom strength" },
	{ "mat_colorcorrection", "Colour correction" },
	{ "mat_vsync", "Vertical sync" },
	{ "mat_powersavingsmode", "Power savings mode" },
	{ "gmod_mcore_test", "Multicore" },
	{ "cl_threaded_bone_setup", "Threaded bone setup" },
	{ "r_threaded_particles", "Threaded particles" },
	{ "r_queued_ropes", "Queued ropes" },
	{ "r_3dsky", "3D skybox" },
	{ "r_shadows", "Shadows" },
	{ "r_shadowrendertotexture", "Shadow quality" },
	{ "r_shadowmaxrendered", "Most shadows drawn" },
	{ "r_flashlightdepthtexture", "Flashlight shadows" },
	{ "r_flashlightdepthres", "Flashlight shadow resolution" },
	{ "r_waterforceexpensive", "Water detail" },
	{ "r_waterforcereflectentities", "Water reflects everything" },
	{ "r_cheapwaterstart", "Cheap water from" },
	{ "r_cheapwaterend", "Cheap water to" },
	{ "r_rootlod", "Model detail" },
	{ "r_lod", "Model detail override" },
	{ "r_decals", "Most decals" },
	{ "r_drawmodeldecals", "Decals on models" },
	{ "r_maxmodeldecal", "Most decals per model" },
	{ "r_drawdetailprops", "Grass and detail props" },
	{ "cl_detaildist", "Detail prop distance" },
	{ "cl_detailfade", "Detail prop fade" },
	{ "r_dynamic", "Dynamic lights" },
	{ "r_worldlights", "World lights on models" },
	{ "r_eyes", "Eyes" },
	{ "r_teeth", "Teeth" },
	{ "r_fastzreject", "Fast Z reject" },
	{ "r_drawropes", "Ropes" },
	{ "r_ropetranslucent", "Smooth ropes" },
	{ "rope_smooth", "Rope smoothing" },
	{ "r_drawflecks", "Impact flecks" },
	{ "r_drawsprites", "Sprites" },
	{ "r_drawparticles", "Particles" },
	{ "r_farz", "Draw distance" },
	{ "r_radiosity", "Ambient lighting" },
	{ "cl_ejectbrass", "Shell casings" },
	{ "muzzleflash_light", "Muzzle flash light" },
	{ "cl_new_impact_effects", "New impact effects" },
	{ "cl_show_splashes", "Water splashes" },
	{ "cl_drawmonitors", "Monitors" },
	{ "cl_drawownshadow", "Your own shadow" },
	{ "cl_ragdoll_collide", "Ragdoll collisions" },
	{ "cl_ragdoll_fade_time", "Ragdoll fade time" },
	{ "cl_ragdoll_forcefade", "Ragdolls fade at once" },
	{ "g_ragdoll_maxcount", "Most ragdolls" },
	{ "props_break_max_pieces", "Most gibs per prop" },
	{ "cl_phys_props_enable", "Client physics props" },
	{ "cl_phys_props_max", "Most client physics props" },
	{ "violence_ablood", "Alien blood" },
	{ "violence_hblood", "Human blood" },
	{ "violence_agibs", "Alien gibs" },
	{ "violence_hgibs", "Human gibs" },
	-- Sound.
	{ "snd_mixahead", "Sound buffer" },
	{ "snd_pitchquality", "Sound pitch quality" },
	{ "dsp_enhance_stereo", "Enhanced stereo" },
	{ "dsp_slow_cpu", "Sound effects for slow CPUs" },
	{ "snd_legacy_surround", "Legacy surround" },
	{ "snd_musicvolume", "Music volume" },
	{ "snd_mute_losefocus", "Mute in the background" },
	{ "snd_disable_mixer_duck", "No ducking" },
	{ "voice_enable", "Voice chat" },
	-- Network.
	{ "cl_interp", "Interpolation" },
	{ "cl_interp_ratio", "Interpolation ratio" },
	{ "cl_updaterate", "Update rate" },
	{ "cl_cmdrate", "Command rate" },
	{ "rate", "Bandwidth" },
	{ "cl_smooth", "Prediction smoothing" },
	{ "cl_smoothtime", "Prediction smoothing time" },
	{ "cl_lagcompensation", "Lag compensation" },
	{ "cl_pred_optimize", "Prediction optimisation" },
	-- Movement and the mouse.
	{ "cl_upspeed", "Up speed" },
	{ "cl_yawspeed", "Turn speed (keys)" },
	{ "cl_pitchspeed", "Look speed (keys)" },
	{ "m_pitch", "Mouse vertical (invert)" },
	{ "m_yaw", "Mouse horizontal" },
	{ "m_side", "Mouse sideways" },
	{ "m_forward", "Mouse forward" },
	{ "m_rawinput", "Raw mouse input" },
	{ "m_customaccel", "Mouse acceleration" },
	{ "m_filter", "Mouse smoothing" },
	{ "joystick", "Controller" },
	-- HUD and view model.
	{ "cl_drawhud", "HUD" },
	{ "r_drawviewmodel", "View model" },
	{ "viewmodel_fov", "View model FOV" },
	{ "crosshair", "Crosshair" },
	{ "hud_fastswitch", "Fast weapon switch" },
	{ "hud_quickinfo", "Quick info" },
	{ "hud_draw_fixed_reticle", "Fixed reticle" },
	{ "hud_saytext_time", "Chat fade time" },
	{ "cl_showfps", "FPS counter" },
	{ "cl_showpos", "Position display" },
	{ "net_graph", "Net graph" },
}

-- The 8 Garry's Mod's menu itself flags as a problem when changed.
Guard.GMOD_CHECKS = {
	mat_hdr_level = true, mat_bumpmap = true, mat_specular = true, mat_viewportscale = true,
	voice_fadeouttime = true, cl_forwardspeed = true, cl_sidespeed = true, cl_backspeed = true,
}

-- { name = value } of every watched setting that exists: get(name) is
-- GetConVarString (an empty string for one the game doesn't have).
function Guard.snapshot(get)
	local snap = {}
	for _, w in ipairs(Guard.WATCH) do
		local v = get(w[1])
		if type(v) == "string" and v ~= "" then snap[w[1]] = v end
	end
	return snap
end

-- The same value: as numbers when both are ("1" and "1.000000"), else as text.
function Guard.same(a, b)
	if a == b then return true end
	local x, y = tonumber(a), tonumber(b)
	return x ~= nil and y ~= nil and math.abs(x - y) <= 1e-6 * math.max(1, math.abs(x))
end

-- What changed since `snap`: { { name, label, was, now } }, in WATCH order.
function Guard.diff(snap, get)
	local changes = {}
	for _, w in ipairs(Guard.WATCH) do
		local was = snap[w[1]]
		if was ~= nil then
			local now = get(w[1])
			if type(now) == "string" and now ~= "" and not Guard.same(was, now) then
				changes[#changes + 1] = { name = w[1], label = w[2], was = was, now = now }
			end
		end
	end
	return changes
end

-- A value as the player reads it: numbers without trailing zeros.
function Guard.show(v)
	local n = tonumber(v)
	if n == nil then return v end
	if n == math.floor(n) and math.abs(n) < 1e15 then return string.format("%d", n) end
	return (string.format("%.4f", n):gsub("0+$", ""))
end

-- The change list for the prompt: one line each, at most `max`, then "and N more".
function Guard.describe(changes, max)
	max = max or 8
	local lines = {}
	for i, c in ipairs(changes) do
		if i > max then
			lines[#lines + 1] = string.format("and %d more", #changes - max)
			break
		end
		lines[#lines + 1] = string.format("%s (%s): %s, now %s", c.label, c.name, Guard.show(c.was), Guard.show(c.now))
	end
	return table.concat(lines, "\n")
end

-- Servers whose changes are always put back: addresses separated by spaces,
-- newest last, at most `max` kept.
function Guard.parseList(text)
	local list = {}
	for a in tostring(text or ""):gmatch("%S+") do
		if a:match("^[%w%.%-%[%]:]+$") then list[#list + 1] = a end
	end
	return list
end

function Guard.listHas(list, address)
	for _, a in ipairs(list) do
		if a == address then return true end
	end
	return false
end

function Guard.listAdd(list, address, max)
	local out = {}
	for _, a in ipairs(list) do
		if a ~= address then out[#out + 1] = a end
	end
	out[#out + 1] = address
	while #out > (max or 20) do table.remove(out, 1) end
	return out
end

return Guard
