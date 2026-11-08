-- The content platform's rules (src/lua/content_logic.lua), no game:
--   lua tests/content_logic_test.lua        (from the repository root; Lua 5.1+)
local here = (arg and arg[0] or ""):match("^(.*)[/\\]") or "."
local C = dofile(here .. "/../src/lua/content_logic.lua")

local failures, count = 0, 0
local function check(name, got, want)
	count = count + 1
	if got ~= want then
		failures = failures + 1
		print(("FAIL %s: got %s, want %s"):format(name, tostring(got), tostring(want)))
	end
end
local function list(t) return t and table.concat(t, ",") or "nil" end
local DOT = " \194\183 "

-- IDs
check("id", C.validId("3791005669"), true)
check("id: largest", C.validId("18446744073709551615"), true)
check("id: past 2^64-1", C.validId("18446744073709551616"), false)
check("id: 21 digits", C.validId("100000000000000000000"), false)
check("id: leading zero", C.validId("0123"), false)
check("id: zero", C.validId("0"), false)
check("id: a number, not a string", C.validId(123), false)
check("id: sign", C.validId("-5"), false)

-- Adverts
local a = C.parseAdvert("1;ws=3791005669,3791005866,~3657646236")
check("advert: ids", list(a and a.ids), "3791005669,3791005866,3657646236")
check("advert: deferred", a and a.deferred["3657646236"], true)
check("advert: not deferred", a and a.deferred["3791005669"], nil)
check("advert: version only", C.parseAdvert("1"), nil)
check("advert: unknown version", C.parseAdvert("2;ws=1"), nil)
check("advert: version glued to text", C.parseAdvert("1ws=5"), nil)
check("advert: empty", C.parseAdvert(""), nil)
check("advert: not a string", C.parseAdvert(nil), nil)
check("advert: too long", C.parseAdvert("1;ws=" .. string.rep("1,", 2100)), nil)
a = C.parseAdvert("1;url=https://example.com/m.json#abcd;ws=5,5,6")
check("advert: unknown field skipped, duplicates once", list(a and a.ids), "5,6")
a = C.parseAdvert("1;ws=5,x,,7,~,-9,0,9;8")
check("advert: junk entries skipped", list(a and a.ids), "5,7,9")
check("advert: no usable ids", C.parseAdvert("1;ws=x,y"), nil)
local many = {}
for i = 1, 250 do many[i] = tostring(1000 + i) end
a = C.parseAdvert("1;ws=" .. table.concat(many, ","))
check("advert: capped", a and #a.ids, C.MAX_ITEMS)
check("advert: cap keeps the first", a and a.ids[1], "1001")
local _, why = C.parseAdvert("3;ws=1")
check("advert: reason", why, "not version 1")

-- Merging the advert with what the last join learned
check("merge", list(C.merge({ "1", "2" }, { "2", "3" })), "1,2,3")
check("merge: learned only", list(C.merge(nil, { "4" })), "4")
check("merge: bad ids dropped", list(C.merge({ "1", "x" }, { "007", "8" })), "1,8")

-- What needs work
check("needs: not installed", C.needs(false, false, false, false, nil), true)
check("needs: update", C.needs(true, true, false, false, nil), true)
check("needs: downloading", C.needs(true, false, true, false, nil), true)
check("needs: legacy, unpacked copy gone", C.needs(true, false, false, true, false), true)
check("needs: legacy, unpacked", C.needs(true, false, false, true, true), false)
check("needs: installed", C.needs(true, false, false, false, nil), false)
check("needs: Steam can't say", C.needs(nil), nil)
check("newer: by more than the slack", C.newer(1000 + 300, 1000, 256), true)
check("newer: within the float's rounding", C.newer(1000 + 128, 1000, 256), false)
check("newer: unknown", C.newer(nil, 1000), false)

-- Consent
local GB = 1024 * 1024 * 1024
check("consent: ours", C.consent({ ours = true, bytes = GB }), "go")
check("consent: joined before", C.consent({ known = true, bytes = GB }), "go")
check("consent: never joined", C.consent({ bytes = 10 }), "ask")
check("consent: ours, but over 4 GB", C.consent({ ours = true, bytes = 5 * GB }), "ask")
check("consent: clicked, over 4 GB", C.consent({ asked = true, bytes = 9 * GB }), "go")
check("consent: no size known", C.consent({ known = true }), "go")

-- Sizes and the row's line
check("size: 0", C.size(0), "0 MB")
check("size: under 1 MB", C.size(1000), "1 MB")
check("size: MB", C.size(412 * 1048576), "412 MB")
check("size: GB", C.size(1.3 * GB), "1.3 GB")
check("size: junk", C.size("x"), "0 MB")
local text, button = C.label({ state = "ask", items = 3, bytes = 1.3 * GB })
check("label: ask", text, "Get ready" .. DOT .. "1.3 GB")
check("label: ask is the button", button, true)
text, button = C.label({ state = "going", items = 3, bytes = 1.3 * GB, done = 412 * 1048576 })
check("label: going", text, "Getting ready" .. DOT .. "412 MB of 1.3 GB")
check("label: going is no button", button, false)
text = C.label({ state = "going", items = 1, bytes = 100, done = 500 })
check("label: done capped at the total", text, "Getting ready" .. DOT .. "1 MB of 1 MB")
check("label: ready", (C.label({ state = "ready", items = 2 })), "Ready" .. DOT .. "content downloaded")
check("label: nothing was needed", C.label({ state = "ready", items = 0 }), nil)
check("label: checking", C.label({ state = "checking" }), nil)
check("label: stopped", C.label({ state = "stopped", items = 2 }), nil)
check("label: no plan", C.label(nil), nil)
-- Items Steam didn't download: said so, and the button stays, to try again.
text, button = C.label({ state = "ready", items = 3, failed = 2 })
check("label: some failed", text, "Partly ready" .. DOT .. "2 items didn't download" .. DOT .. "Try again")
check("label: failed is the button", button, true)
check("label: all failed", (C.label({ state = "ready", items = 1, failed = 1 })), "Not ready" .. DOT .. "1 item didn't download" .. DOT .. "Try again")
check("retry: none yet", C.retry(nil), true)
check("retry: stopped", C.retry({ state = "stopped" }), true)
check("retry: failed items", C.retry({ state = "ready", items = 2, failed = 1 }), true)
check("retry: all downloaded", C.retry({ state = "ready", items = 2, failed = 0 }), false)
check("retry: still going", C.retry({ state = "going" }), false)

print(("%d checks, %d failed"):format(count, failures))
if failures > 0 then os.exit(1) end
