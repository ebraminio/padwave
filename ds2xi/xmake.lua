set_allowedplats("windows", "mingw")
set_languages("c++23")

target("hidapi", function ()
    set_kind("static")
    add_includedirs("hidapi/hidapi", "hidapi/windows", {public = true})
    add_defines("HID_API_NO_EXPORT_DEFINE", {public = true})
    add_defines("HIDAPI_NO_CRT")
    add_files("hidapi/windows/hid.c")
    set_runtimes("none")
    set_exceptions("none")
    if is_plat("mingw") then
        add_cflags("-fno-stack-protector", {force = true})
    else
        add_cxflags("/GS-", {force = true})
    end
end)

local xinput_variants = {
    { name = "xinput1_1", def = "xinput1_1.def" },
    { name = "xinput1_2", def = "xinput1_2.def" },
    { name = "xinput1_3", def = "xinput1_3.def" },
    { name = "XInput1_4", def = "xinput1_4.def" },
    { name = "XInput9_1_0", def = "xinput9_1_0.def" },
    { name = "XInputUap", def = "xinputuap.def" },
}

for _, variant in ipairs(xinput_variants) do
    target(variant.name, function ()
        set_kind("shared")
        add_deps("hidapi")
        add_files("main.cc", variant.def)
        add_includedirs("..", "hidapi/hidapi")
        add_syslinks("shlwapi", "kernel32", "user32")
        set_runtimes("none")
        set_exceptions("none")
        set_basename(variant.name)
        set_prefixname("")
        set_warnings("allextra")
        set_optimize("faster")
        add_defines('DS2XI_SYSTEM_XINPUT="' .. variant.name .. '.dll"')
        if is_plat("mingw") then
            add_cxxflags("-fno-stack-protector", "-fno-rtti", {force = true})
            local entry = is_arch("i386", "x86") and "_DllMain@12" or "DllMain"
            add_shflags("-nostartfiles", "-nodefaultlibs", "-Wl,--kill-at", "-Wl,--entry=" .. entry,
                        {force = true})
        else
            add_cxflags("/GS-", {force = true})
            add_shflags("/ENTRY:DllMain", "/NODEFAULTLIB", "/SUBSYSTEM:WINDOWS", {force = true})
        end
        if is_mode("release") then
            set_strip("all")
        end
    end)
end
